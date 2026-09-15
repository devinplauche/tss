/* FACE 3.2 TSS TPM implementation: channel-based transport. */

#include "face_tss/tpm.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
#endif

#define TPM_MAX_CHANNELS 16
#define TPM_MAX_MSG 65536

typedef struct {
    char endpoint_name[128];
    int in_use;
    int open;
    /* Simple message queue for the channel. */
    uint8_t *pending_msg;
    size_t pending_len;
    FACE_TSS_TRANSACTION_ID_TYPE pending_txn;
    /* Callbacks. */
    FACE_TSS_TPM_DATA_CB data_cb;
    FACE_TSS_TPM_EVENT_CB event_cb;
    void *cb_user;
    FACE_TSS_TPM_CALLBACK_KIND cb_kind;
    int cb_registered;
} tpm_channel_t;

struct FACE_TSS_TPM {
    char name[64];
    int initialized;
    FACE_TSS_TPM_STATE_TYPE state;
    FACE_TSS_TPM_EVENT_TYPE status;
    tpm_channel_t channels[TPM_MAX_CHANNELS];
    FACE_TSS_TPM_CHANNEL_ID_TYPE next_channel_id;
    /* destroy() sets destroying under the lock; afterwards every entry
     * point fails with NOT_AVAILABLE, blocked waiters are woken, and
     * destroy() waits until waiters (threads inside a cond_wait) drains
     * before freeing anything. */
    int destroying;
    int waiters;
#if defined(_WIN32)
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE data_cond;
    CONDITION_VARIABLE destroy_cond;
#else
    pthread_mutex_t lock;
    pthread_cond_t data_cond;
    pthread_cond_t destroy_cond;
#endif
};

/* Monotonic clock, nanoseconds. */
static int64_t tpm_now_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER ctr;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return (int64_t)(ctr.QuadPart * 1000000000LL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

#if defined(_WIN32)
static void tpm_lock(FACE_TSS_TPM *tpm) { EnterCriticalSection(&tpm->lock); }
static void tpm_unlock(FACE_TSS_TPM *tpm) { LeaveCriticalSection(&tpm->lock); }
/* Wait for a signal or until deadline_ns (monotonic). infinite: wait forever.
 * Returns 1 if signaled, 0 on timeout. The waiter is counted in
 * tpm->waiters so destroy() can wait for it before freeing anything. */
static int tpm_wait_until(FACE_TSS_TPM *tpm, int64_t deadline_ns, int infinite)
{
    DWORD ms;
    int signaled;
    if (infinite)
        ms = INFINITE;
    else {
        int64_t rem = deadline_ns - tpm_now_ns();
        if (rem <= 0)
            return 0;
        ms = (DWORD)(rem / 1000000LL);
    }
    tpm->waiters++;
    signaled =
        SleepConditionVariableCS(&tpm->data_cond, &tpm->lock, ms) ? 1 : 0;
    tpm->waiters--;
    if (tpm->destroying && tpm->waiters == 0)
        WakeConditionVariable(&tpm->destroy_cond);
    return signaled;
}
static void tpm_broadcast_data(FACE_TSS_TPM *tpm)
{
    WakeAllConditionVariable(&tpm->data_cond);
}
#else
static void tpm_lock(FACE_TSS_TPM *tpm) { pthread_mutex_lock(&tpm->lock); }
static void tpm_unlock(FACE_TSS_TPM *tpm) { pthread_mutex_unlock(&tpm->lock); }
/* Wait for a signal or until deadline_ns (monotonic). infinite: wait forever.
 * Returns 1 if signaled, 0 on timeout. The waiter is counted in
 * tpm->waiters so destroy() can wait for it before freeing anything. */
static int tpm_wait_until(FACE_TSS_TPM *tpm, int64_t deadline_ns, int infinite)
{
    /* data_cond uses CLOCK_MONOTONIC (see create), so the monotonic
     * deadline converts directly to an absolute timespec. */
    struct timespec abs_ts;
    int rv, signaled;
    if (infinite) {
        tpm->waiters++;
        rv = pthread_cond_wait(&tpm->data_cond, &tpm->lock);
        tpm->waiters--;
        signaled = (rv == 0);
    } else {
        int64_t rem = deadline_ns - tpm_now_ns();
        if (rem <= 0)
            return 0;
        abs_ts.tv_sec = (time_t)(deadline_ns / 1000000000LL);
        abs_ts.tv_nsec = (long)(deadline_ns % 1000000000LL);
        tpm->waiters++;
        rv = pthread_cond_timedwait(&tpm->data_cond, &tpm->lock, &abs_ts);
        tpm->waiters--;
        signaled = (rv == 0);
    }
    if (tpm->destroying && tpm->waiters == 0)
        pthread_cond_signal(&tpm->destroy_cond);
    return signaled;
}
static void tpm_broadcast_data(FACE_TSS_TPM *tpm)
{
    pthread_cond_broadcast(&tpm->data_cond);
}
#endif

/* Caller holds the lock. Returns 1 when the TPM is being destroyed. */
static int tpm_destroying(FACE_TSS_TPM *tpm)
{
    return tpm->destroying;
}

FACE_TSS_TPM *face_tss_tpm_create(const char *name)
{
    FACE_TSS_TPM *tpm = (FACE_TSS_TPM *)calloc(1, sizeof(*tpm));
    if (!tpm)
        return NULL;
    if (name)
        strncpy(tpm->name, name, sizeof(tpm->name) - 1);
    tpm->state = FACE_TSS_TPM_STATE_NORMAL;
    tpm->status = FACE_TSS_TPM_INIT_COMPLETE;
    tpm->next_channel_id = 1;
#if defined(_WIN32)
    InitializeCriticalSection(&tpm->lock);
    InitializeConditionVariable(&tpm->data_cond);
    InitializeConditionVariable(&tpm->destroy_cond);
#else
    pthread_mutex_init(&tpm->lock, NULL);
    {
        /* Monotonic clock: immune to wall-clock jumps. */
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&tpm->data_cond, &attr);
        pthread_condattr_destroy(&attr);
    }
    pthread_cond_init(&tpm->destroy_cond, NULL);
#endif
    return tpm;
}

void face_tss_tpm_destroy(FACE_TSS_TPM *tpm)
{
    int i;
    if (!tpm)
        return;
    tpm_lock(tpm);
    tpm->destroying = 1;
    /* Wake every blocked waiter; each one decrements tpm->waiters on wake
     * and the last one signals destroy_cond. */
    tpm_broadcast_data(tpm);
    while (tpm->waiters > 0) {
#if defined(_WIN32)
        SleepConditionVariableCS(&tpm->destroy_cond, &tpm->lock, INFINITE);
#else
        pthread_cond_wait(&tpm->destroy_cond, &tpm->lock);
#endif
    }
    tpm_unlock(tpm);
    for (i = 0; i < TPM_MAX_CHANNELS; i++) {
        free(tpm->channels[i].pending_msg);
    }
#if defined(_WIN32)
    DeleteCriticalSection(&tpm->lock);
#else
    pthread_cond_destroy(&tpm->data_cond);
    pthread_cond_destroy(&tpm->destroy_cond);
    pthread_mutex_destroy(&tpm->lock);
#endif
    free(tpm);
}

FACE_TSS_RETURN_CODE face_tss_tpm_initialize(
    FACE_TSS_TPM *tpm, const char *configuration_resource)
{
    if (!tpm)
        return FACE_TSS_RC_INVALID_PARAM;
    (void)configuration_resource;
    tpm_lock(tpm);
    if (tpm_destroying(tpm)) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    if (tpm->initialized) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NO_ACTION;
    }
    tpm->initialized = 1;
    tpm->status = FACE_TSS_TPM_INIT_COMPLETE;
    tpm_unlock(tpm);
    return FACE_TSS_RC_NO_ERROR;
}

/* Caller must hold tpm->lock. */
static tpm_channel_t *find_channel(FACE_TSS_TPM *tpm,
                                   FACE_TSS_TPM_CHANNEL_ID_TYPE id)
{
    int i;
    for (i = 0; i < TPM_MAX_CHANNELS; i++) {
        if (tpm->channels[i].in_use &&
            (FACE_TSS_TPM_CHANNEL_ID_TYPE)(i + 1) == id)
            return &tpm->channels[i];
    }
    return NULL;
}

FACE_TSS_RETURN_CODE face_tss_tpm_open_channel(
    FACE_TSS_TPM *tpm, const char *endpoint_name,
    const uint8_t *transport_config, size_t transport_config_len,
    const uint8_t *security_config, size_t security_config_len,
    FACE_TSS_TPM_CHANNEL_ID_TYPE *channel_id_out)
{
    int i;
    if (!tpm || !channel_id_out)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!endpoint_name)
        return FACE_TSS_RC_INVALID_PARAM;
    (void)transport_config;
    (void)transport_config_len;
    (void)security_config;
    (void)security_config_len;

    tpm_lock(tpm);
    if (tpm_destroying(tpm)) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    if (!tpm->initialized) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    if (tpm->state == FACE_TSS_TPM_STATE_SHUTDOWN) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_INVALID_MODE;
    }

    for (i = 0; i < TPM_MAX_CHANNELS; i++) {
        if (!tpm->channels[i].in_use) {
            tpm->channels[i].in_use = 1;
            tpm->channels[i].open = 1;
            strncpy(tpm->channels[i].endpoint_name, endpoint_name,
                    sizeof(tpm->channels[i].endpoint_name) - 1);
            *channel_id_out = (FACE_TSS_TPM_CHANNEL_ID_TYPE)(i + 1);
            tpm_unlock(tpm);
            return FACE_TSS_RC_NO_ERROR;
        }
    }
    tpm_unlock(tpm);
    return FACE_TSS_RC_RESOURCE_LIMIT_REACHED;
}

FACE_TSS_RETURN_CODE face_tss_tpm_close_channel(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id)
{
    tpm_channel_t *ch;
    if (!tpm)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    tpm_lock(tpm);
    if (tpm_destroying(tpm)) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    ch = find_channel(tpm, channel_id);
    if (!ch) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_INVALID_PARAM;
    }
    free(ch->pending_msg);
    ch->pending_msg = NULL;
    ch->pending_len = 0;
    ch->in_use = 0;
    ch->open = 0;
    ch->cb_registered = 0;
    /* Wake any thread blocked in is_data_available/read_from_transport so it
     * re-scans and observes the closed channel. */
    tpm_broadcast_data(tpm);
    tpm_unlock(tpm);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_tpm_request_state_change(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_STATE_TYPE new_state,
    const void *data, size_t data_len)
{
    if (!tpm)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    (void)data;
    (void)data_len;
    /* Validate state transition. */
    switch (new_state) {
    case FACE_TSS_TPM_STATE_NORMAL:
    case FACE_TSS_TPM_STATE_TEST:
    case FACE_TSS_TPM_STATE_RESUME:
    case FACE_TSS_TPM_STATE_PAUSE:
    case FACE_TSS_TPM_STATE_SHUTDOWN:
    case FACE_TSS_TPM_STATE_SECURE:
        break;
    default:
        return FACE_TSS_RC_INVALID_PARAM;
    }
    tpm_lock(tpm);
    if (tpm_destroying(tpm)) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    tpm->state = new_state;
    /* Wake blocked waiters so they observe the new state. */
    tpm_broadcast_data(tpm);
    tpm_unlock(tpm);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_tpm_is_data_available(
    FACE_TSS_TPM *tpm,
    const FACE_TSS_TPM_CHANNEL_ID_TYPE *channel_ids, size_t channel_count,
    FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_TPM_CHANNEL_ID_TYPE *available_out, size_t *available_count_inout)
{
    size_t n = 0;
    size_t i;
    size_t capacity;
    int infinite;
    int64_t deadline_ns;
    if (!tpm || !available_out || !available_count_inout)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    if (channel_count > 0 && !channel_ids)
        return FACE_TSS_RC_INVALID_PARAM;
    capacity = *available_count_inout;
    infinite = (timeout_ns == FACE_TSS_TIMEOUT_INFINITE);
    {
        int64_t now = tpm_now_ns();
        int64_t wait_ns = timeout_ns > 0 ? timeout_ns : 0;
        deadline_ns =
            (wait_ns > INT64_MAX - now) ? INT64_MAX : now + wait_ns;
    }

    tpm_lock(tpm);
    if (tpm_destroying(tpm)) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    for (;;) {
        n = 0;
        for (i = 0; i < channel_count && n < capacity; i++) {
            tpm_channel_t *ch = find_channel(tpm, channel_ids[i]);
            if (ch && ch->pending_msg) {
                available_out[n++] = channel_ids[i];
            }
        }
        if (n > 0)
            break;
        /* Nothing available yet. */
        if (tpm_destroying(tpm))
            break;
        if (!infinite && tpm_now_ns() >= deadline_ns)
            break;
        /* Spurious wakeups are harmless: the loop re-scans. */
        tpm_wait_until(tpm, deadline_ns, infinite);
    }
    {
        int gone = tpm_destroying(tpm);
        tpm_unlock(tpm);
        *available_count_inout = n;
        return gone ? FACE_TSS_RC_NOT_AVAILABLE : FACE_TSS_RC_NO_ERROR;
    }
}

FACE_TSS_RETURN_CODE face_tss_tpm_get_status(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_EVENT_TYPE *status_out)
{
    FACE_TSS_TPM_EVENT_TYPE status;
    if (!tpm || !status_out)
        return FACE_TSS_RC_INVALID_PARAM;
    tpm_lock(tpm);
    if (tpm_destroying(tpm)) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    if (!tpm->initialized) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    status = tpm->status;
    tpm_unlock(tpm);
    *status_out = status;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_tpm_read_from_transport(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id,
    FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id_out,
    uint8_t *message_out, size_t *message_len_inout)
{
    tpm_channel_t *ch;
    int infinite;
    int64_t deadline_ns;
    FACE_TSS_RETURN_CODE rc = FACE_TSS_RC_TIMED_OUT;
    if (!tpm || !transaction_id_out || !message_out || !message_len_inout)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    infinite = (timeout_ns == FACE_TSS_TIMEOUT_INFINITE);
    {
        int64_t now = tpm_now_ns();
        int64_t wait_ns = timeout_ns > 0 ? timeout_ns : 0;
        deadline_ns =
            (wait_ns > INT64_MAX - now) ? INT64_MAX : now + wait_ns;
    }

    tpm_lock(tpm);
    if (tpm_destroying(tpm)) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    for (;;) {
        ch = find_channel(tpm, channel_id);
        if (!ch) {
            rc = FACE_TSS_RC_INVALID_PARAM;
            break;
        }
        if (!ch->open) {
            rc = FACE_TSS_RC_CONNECTION_CLOSED;
            break;
        }
        if (ch->pending_msg) {
            if (*message_len_inout < ch->pending_len) {
                *message_len_inout = ch->pending_len;
                rc = FACE_TSS_RC_DATA_BUFFER_TOO_SMALL;
            } else {
                memcpy(message_out, ch->pending_msg, ch->pending_len);
                *message_len_inout = ch->pending_len;
                *transaction_id_out = ch->pending_txn;
                free(ch->pending_msg);
                ch->pending_msg = NULL;
                ch->pending_len = 0;
                rc = FACE_TSS_RC_NO_ERROR;
            }
            break;
        }
        if (tpm_destroying(tpm)) {
            rc = FACE_TSS_RC_NOT_AVAILABLE;
            break;
        }
        if (!infinite && tpm_now_ns() >= deadline_ns)
            break; /* rc stays TIMED_OUT */
        /* Spurious wakeups are harmless: the loop re-checks. */
        tpm_wait_until(tpm, deadline_ns, infinite);
    }
    tpm_unlock(tpm);
    return rc;
}

FACE_TSS_RETURN_CODE face_tss_tpm_write_to_transport(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id,
    FACE_TIMEOUT_TYPE max_delay_ns,
    const uint8_t *message, size_t message_len,
    FACE_TSS_TRANSACTION_ID_TYPE transaction_id)
{
    tpm_channel_t *ch;
    uint8_t *buf;
    /* Callback fields are captured under the lock and fired after unlock:
     * the callback may re-enter the TPM. */
    FACE_TSS_TPM_DATA_CB data_cb = NULL;
    void *cb_user = NULL;
    FACE_TSS_TPM_CALLBACK_KIND cb_kind = 0;
    int cb_registered = 0;
    if (!tpm)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    tpm_lock(tpm);
    if (tpm_destroying(tpm)) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    ch = find_channel(tpm, channel_id);
    if (!ch) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_INVALID_PARAM;
    }
    if (!ch->open) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_CONNECTION_CLOSED;
    }
    (void)max_delay_ns;
    if (message_len > TPM_MAX_MSG) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_DATA_OVERFLOW;
    }
    if (message_len > 0 && !message) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_INVALID_PARAM;
    }
    /* Loopback: deliver to the channel's pending queue (for testing). */
    buf = (uint8_t *)malloc(message_len ? message_len : 1);
    if (!buf) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_RESOURCE_LIMIT_REACHED;
    }
    if (message_len > 0)
        memcpy(buf, message, message_len);
    free(ch->pending_msg);
    ch->pending_msg = buf;
    ch->pending_len = message_len;
    ch->pending_txn = transaction_id;
    data_cb = ch->data_cb;
    cb_user = ch->cb_user;
    cb_kind = ch->cb_kind;
    cb_registered = ch->cb_registered;
    /* Wake threads blocked in is_data_available / read_from_transport. */
#if defined(_WIN32)
    WakeAllConditionVariable(&tpm->data_cond);
#else
    pthread_cond_broadcast(&tpm->data_cond);
#endif
    tpm_unlock(tpm);
    /* Fire data callback outside the lock (see above). */
    if (cb_registered && data_cb &&
        (cb_kind == FACE_TSS_TPM_CALLBACK_DATA ||
         cb_kind == FACE_TSS_TPM_CALLBACK_BOTH)) {
        FACE_TSS_RETURN_CODE cb_rc = FACE_TSS_RC_NO_ERROR;
        data_cb(channel_id, transaction_id, message, message_len,
                cb_user, &cb_rc);
    }
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_tpm_register_callback(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id,
    FACE_TSS_TPM_CALLBACK_KIND kind,
    FACE_TSS_TPM_DATA_CB data_cb, FACE_TSS_TPM_EVENT_CB event_cb,
    void *user)
{
    tpm_channel_t *ch;
    FACE_TSS_RETURN_CODE rc;
    if (!tpm)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    tpm_lock(tpm);
    if (tpm_destroying(tpm)) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    ch = find_channel(tpm, channel_id);
    if (!ch) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_INVALID_PARAM;
    }
    if (ch->cb_registered) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NO_ACTION;
    }
    ch->data_cb = data_cb;
    ch->event_cb = event_cb;
    ch->cb_user = user;
    ch->cb_kind = kind;
    ch->cb_registered = 1;
    rc = FACE_TSS_RC_NO_ERROR;
    tpm_unlock(tpm);
    return rc;
}

FACE_TSS_RETURN_CODE face_tss_tpm_unregister_callback(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id,
    FACE_TSS_TPM_CALLBACK_KIND kind)
{
    tpm_channel_t *ch;
    FACE_TSS_RETURN_CODE rc;
    if (!tpm)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    tpm_lock(tpm);
    if (tpm_destroying(tpm)) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    ch = find_channel(tpm, channel_id);
    if (!ch) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_INVALID_PARAM;
    }
    (void)kind;
    if (!ch->cb_registered) {
        tpm_unlock(tpm);
        return FACE_TSS_RC_NO_ACTION;
    }
    ch->cb_registered = 0;
    ch->data_cb = NULL;
    ch->event_cb = NULL;
    rc = FACE_TSS_RC_NO_ERROR;
    tpm_unlock(tpm);
    return rc;
}
