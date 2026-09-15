/* nng transport: one FACE connection = one nng socket. See transport.h. */

#include "face_tss/transport.h"

#include <stdlib.h>
#include <string.h>

#include <flatcc/flatcc_builder.h>
#include <nng/nng.h>
#include <nng/protocol/bus0/bus.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

/* Some nng versions expose these; define fallbacks otherwise. */
#ifndef NNG_OPT_RECVTIMEO
#define NNG_OPT_RECVTIMEO "recv-timeout"
#endif
#ifndef NNG_OPT_SENDTIMEO
#define NNG_OPT_SENDTIMEO "send-timeout"
#endif
#ifndef NNG_OPT_SUB_SUBSCRIBE
#define NNG_OPT_SUB_SUBSCRIBE "sub:subscribe"
#endif
#ifndef NNG_DURATION_INFINITE
#define NNG_DURATION_INFINITE (-1)
#endif
#ifndef NNG_DURATION_ZERO
#define NNG_DURATION_ZERO (0)
#endif

struct FACE_TSS_TRANSPORT {
    FACE_TSS_CONNECTION_CONFIG cfg;
    nng_socket sock;
    char topic[FACE_TSS_MAX_CONNECTION_NAME + 1];
    size_t topic_len;
    /* Callback state. Guarded by cb_lock: the dispatch thread and the
     * thread calling callback_stop run concurrently, and plain `volatile`
     * is not synchronization.
     *
     * Self-stop: a data callback may unregister its own connection, which
     * reaches callback_stop on the dispatch thread itself. Joining the
     * current thread is impossible, so a self-stop only signals the stop
     * and parks the thread handle in cb_zombie; the next callback_stop /
     * callback_start / shutdown / close from any other thread reaps it.
     * transport_close from the dispatch thread itself defers the free to
     * the dispatch loop's exit path (close_pending). */
    FACE_TSS_ENVELOPE_CB cb;
    void *cb_user;
    nng_thread *cb_thread;
#if defined(_WIN32)
    DWORD cb_tid;
#else
    pthread_t cb_tid;
#endif
    int cb_tid_valid;
    int cb_stop;
    nng_thread *cb_zombie;
#if defined(_WIN32)
    DWORD cb_zombie_tid;
#else
    pthread_t cb_zombie_tid;
#endif
    /* Set once the socket is closed; makes shutdown idempotent. */
    int closed;
    /* transport_close was called from the dispatch thread itself: the
     * loop frees the transport on exit instead. */
    int close_pending;
#if defined(_WIN32)
    CRITICAL_SECTION cb_lock;
    CRITICAL_SECTION send_lock;
    CRITICAL_SECTION recv_lock;
#else
    pthread_mutex_t cb_lock;
    pthread_mutex_t send_lock;
    pthread_mutex_t recv_lock;
#endif
};

/* send_lock serializes concurrent sends on one socket: send mutates the
 * socket's send-timeout option per call, so two overlapping sends could
 * apply each other's timeout. recv_lock does the same for receives.
 * Both are leaf locks: never held while acquiring another lock. */

static void cb_take(FACE_TSS_TRANSPORT *t)
{
#if defined(_WIN32)
    EnterCriticalSection(&t->cb_lock);
#else
    pthread_mutex_lock(&t->cb_lock);
#endif
}

static void cb_drop(FACE_TSS_TRANSPORT *t)
{
#if defined(_WIN32)
    LeaveCriticalSection(&t->cb_lock);
#else
    pthread_mutex_unlock(&t->cb_lock);
#endif
}

static void io_take(
#if defined(_WIN32)
    CRITICAL_SECTION *m)
{
    EnterCriticalSection(m);
#else
    pthread_mutex_t *m)
{
    pthread_mutex_lock(m);
#endif
}

static void io_drop(
#if defined(_WIN32)
    CRITICAL_SECTION *m)
{
    LeaveCriticalSection(m);
#else
    pthread_mutex_t *m)
{
    pthread_mutex_unlock(m);
#endif
}

static long long ns_to_ms(FACE_TIMEOUT_TYPE ns)
{
    if (ns == FACE_TSS_TIMEOUT_INFINITE)
        return NNG_DURATION_INFINITE;
    if (ns <= 0)
        return NNG_DURATION_ZERO;
    return (long long)((ns + 999999LL) / 1000000LL);
}

static FACE_TSS_RETURN_CODE open_socket(
    const FACE_TSS_CONNECTION_CONFIG *cfg, int dial_only, nng_socket *sock)
{
    nng_socket s;
    int rv;
    memset(&s, 0, sizeof(s));
    if (cfg->transport == FACE_TSS_TRANSPORT_PUBSUB) {
        if (cfg->role == FACE_TSS_ROLE_PUBLISHER && !dial_only)
            rv = nng_pub0_open(&s);
        else
            rv = nng_sub0_open(&s);
    } else {
        rv = nng_bus0_open(&s);
    }
    if (rv != 0)
        return FACE_TSS_RC_NOT_AVAILABLE;

    if (cfg->transport == FACE_TSS_TRANSPORT_PUBSUB) {
        if (cfg->role == FACE_TSS_ROLE_PUBLISHER && !dial_only) {
            rv = nng_listen(s, cfg->address, NULL, 0);
            if (rv != 0) {
                nng_close(s);
                return (rv == NNG_EADDRINUSE) ? FACE_TSS_RC_ADDR_IN_USE
                                              : FACE_TSS_RC_NOT_AVAILABLE;
            }
        } else {
            char topic[FACE_TSS_MAX_CONNECTION_NAME + 1];
            size_t tlen = face_tss_topic_for(cfg->name, topic);
            /* Non-blocking dial: the dialer retries in the background, so
             * start order never matters (subscriber may start first). */
            rv = nng_dial(s, cfg->address, NULL, NNG_FLAG_NONBLOCK);
            if (rv != 0 && rv != NNG_ECONNREFUSED) {
                /* fall through: background retry still applies */
            }
            rv = 0;
            if (cfg->role != FACE_TSS_ROLE_PUBLISHER) {
                rv = nng_socket_set(
                    s, NNG_OPT_SUB_SUBSCRIBE, topic, tlen);
            }
            if (rv != 0) {
                nng_close(s);
                return FACE_TSS_RC_NOT_AVAILABLE;
            }
        }
    } else {
        if (dial_only)
            rv = nng_dial(s, cfg->address, NULL, NNG_FLAG_NONBLOCK);
        else {
            rv = nng_listen(s, cfg->address, NULL, 0);
            if (rv == NNG_EADDRINUSE)
                rv = nng_dial(s, cfg->address, NULL, NNG_FLAG_NONBLOCK);
        }
        if (rv != 0 && rv != NNG_ECONNREFUSED)
            rv = 0; /* dialers retry in the background */
        if (rv != 0) {
            nng_close(s);
            return FACE_TSS_RC_NOT_AVAILABLE;
        }
    }
    *sock = s;
    return FACE_TSS_RC_NO_ERROR;
}

static FACE_TSS_TRANSPORT *alloc_t(const FACE_TSS_CONNECTION_CONFIG *cfg)
{
    FACE_TSS_TRANSPORT *t =
        (FACE_TSS_TRANSPORT *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->cfg = *cfg;
    t->topic_len = face_tss_topic_for(cfg->name, t->topic);
#if defined(_WIN32)
    InitializeCriticalSection(&t->cb_lock);
    InitializeCriticalSection(&t->send_lock);
    InitializeCriticalSection(&t->recv_lock);
#else
    pthread_mutex_init(&t->cb_lock, NULL);
    pthread_mutex_init(&t->send_lock, NULL);
    pthread_mutex_init(&t->recv_lock, NULL);
#endif
    return t;
}

static void locks_fini(FACE_TSS_TRANSPORT *t)
{
#if defined(_WIN32)
    DeleteCriticalSection(&t->cb_lock);
    DeleteCriticalSection(&t->send_lock);
    DeleteCriticalSection(&t->recv_lock);
#else
    pthread_mutex_destroy(&t->cb_lock);
    pthread_mutex_destroy(&t->send_lock);
    pthread_mutex_destroy(&t->recv_lock);
#endif
}

FACE_TSS_RETURN_CODE face_tss_transport_open(
    const FACE_TSS_CONNECTION_CONFIG *cfg, FACE_TSS_TRANSPORT **out)
{
    FACE_TSS_TRANSPORT *t;
    FACE_TSS_RETURN_CODE rc;
    if (!cfg || !out)
        return FACE_TSS_RC_INVALID_PARAM;
    t = alloc_t(cfg);
    if (!t)
        return FACE_TSS_RC_NOT_AVAILABLE;
    rc = open_socket(cfg, 0, &t->sock);
    if (rc != FACE_TSS_RC_NO_ERROR) {
        locks_fini(t);
        free(t);
        return rc;
    }
    *out = t;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_transport_open_dial(
    const FACE_TSS_CONNECTION_CONFIG *cfg, FACE_TSS_TRANSPORT **out)
{
    FACE_TSS_TRANSPORT *t;
    FACE_TSS_RETURN_CODE rc;
    if (!cfg || !out)
        return FACE_TSS_RC_INVALID_PARAM;
    t = alloc_t(cfg);
    if (!t)
        return FACE_TSS_RC_NOT_AVAILABLE;
    rc = open_socket(cfg, 1, &t->sock);
    if (rc != FACE_TSS_RC_NO_ERROR) {
        locks_fini(t);
        free(t);
        return rc;
    }
    *out = t;
    return FACE_TSS_RC_NO_ERROR;
}

/* True when the calling thread is this transport's dispatch thread
 * (live or self-stopped zombie). Caller holds no locks. */
static int in_dispatch(FACE_TSS_TRANSPORT *t)
{
    int r;
#if defined(_WIN32)
    DWORD me = GetCurrentThreadId();
#else
    pthread_t me = pthread_self();
#endif
    cb_take(t);
#if defined(_WIN32)
    r = (t->cb_thread && t->cb_tid_valid && me == t->cb_tid) ||
        (t->cb_zombie && me == t->cb_zombie_tid);
#else
    r = (t->cb_thread && t->cb_tid_valid && pthread_equal(me, t->cb_tid)) ||
        (t->cb_zombie && pthread_equal(me, t->cb_zombie_tid));
#endif
    cb_drop(t);
    return r;
}

/* Stop the callback thread and close the socket without freeing the
 * handle. Idempotent: closing the socket aborts any thread blocked in
 * send/receive, which is how teardown interrupts in-flight I/O. A
 * self-stop leaves a zombie for later reaping; it does not block. */
void face_tss_transport_shutdown(FACE_TSS_TRANSPORT *t)
{
    int do_close;
    if (!t)
        return;
    face_tss_transport_callback_stop(t);
    cb_take(t);
    do_close = !t->closed;
    t->closed = 1;
    cb_drop(t);
    if (do_close)
        nng_close(t->sock);
}

void face_tss_transport_close(FACE_TSS_TRANSPORT *t)
{
    int defer;
    if (!t)
        return;
    face_tss_transport_shutdown(t);
    /* If the current thread is the dispatch thread (destroy issued from
     * within the data callback), the transport cannot be freed yet: the
     * dispatch loop still touches it on exit. Defer the free to the
     * loop's exit path via close_pending. */
    defer = in_dispatch(t);
    if (defer) {
        cb_take(t);
        t->close_pending = 1;
        cb_drop(t);
        return;
    }
    locks_fini(t);
    free(t);
}

FACE_TSS_RETURN_CODE face_tss_transport_send(
    FACE_TSS_TRANSPORT *t, const FACE_TSS_ENVELOPE *env,
    FACE_TIMEOUT_TYPE timeout_ns)
{
    uint8_t *body = NULL;
    size_t body_len = 0;
    nng_msg *msg = NULL;
    int rv;
    FACE_TSS_RETURN_CODE rc;
    if (!t || !env)
        return FACE_TSS_RC_INVALID_PARAM;
    rc = face_tss_envelope_encode(env, &body, &body_len);
    if (rc != FACE_TSS_RC_NO_ERROR)
        return rc;
    rv = nng_msg_alloc(&msg, 0);
    if (rv != 0) {
        flatcc_builder_free(body);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    if (t->cfg.transport == FACE_TSS_TRANSPORT_PUBSUB) {
        rv = nng_msg_append(msg, t->topic, t->topic_len);
        if (rv != 0)
            goto fail;
    }
    rv = nng_msg_append(msg, body, body_len);
    flatcc_builder_free(body);
    body = NULL;
    if (rv != 0)
        goto fail;
    /* Bound the blocking time per the FACE Send_Message timeout. The
     * socket option is per-socket, not per-call, so same-direction sends
     * are serialized (see send_lock). */
    io_take(&t->send_lock);
    rv = nng_socket_set_ms(
        t->sock, NNG_OPT_SENDTIMEO, (nng_duration)ns_to_ms(timeout_ns));
    if (rv != 0) {
        io_drop(&t->send_lock);
        goto fail;
    }
    rv = nng_sendmsg(t->sock, msg, 0);
    /* Restore non-blocking sends for the callback/poll paths. */
    nng_socket_set_ms(t->sock, NNG_OPT_SENDTIMEO, NNG_DURATION_ZERO);
    io_drop(&t->send_lock);
    if (rv == NNG_ETIMEDOUT) {
        nng_msg_free(msg);
        return FACE_TSS_RC_TIMED_OUT;
    }
    if (rv != 0) {
        nng_msg_free(msg);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    return FACE_TSS_RC_NO_ERROR;
fail:
    if (body)
        flatcc_builder_free(body);
    nng_msg_free(msg);
    return FACE_TSS_RC_NOT_AVAILABLE;
}

FACE_TSS_RETURN_CODE face_tss_transport_receive(
    FACE_TSS_TRANSPORT *t, FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_ENVELOPE *out)
{
    nng_msg *msg = NULL;
    int rv;
    const uint8_t *data;
    size_t len;
    FACE_TSS_RETURN_CODE rc;
    if (!t || !out)
        return FACE_TSS_RC_INVALID_PARAM;
    /* The receive-timeout option is per-socket, not per-call, so
     * same-direction receives are serialized (see recv_lock). */
    io_take(&t->recv_lock);
    rv = nng_socket_set_ms(
        t->sock, NNG_OPT_RECVTIMEO, (nng_duration)ns_to_ms(timeout_ns));
    if (rv != 0) {
        io_drop(&t->recv_lock);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    rv = nng_recvmsg(t->sock, &msg, 0);
    io_drop(&t->recv_lock);
    if (rv == NNG_ETIMEDOUT)
        return FACE_TSS_RC_TIMED_OUT;
    if (rv != 0)
        return FACE_TSS_RC_NOT_AVAILABLE;
    data = (const uint8_t *)nng_msg_body(msg);
    len = nng_msg_len(msg);
    if (t->cfg.transport == FACE_TSS_TRANSPORT_PUBSUB) {
        if (len < t->topic_len ||
            memcmp(data, t->topic, t->topic_len) != 0) {
            nng_msg_free(msg);
            return FACE_TSS_RC_NOT_AVAILABLE; /* topic mismatch */
        }
        data += t->topic_len;
        len -= t->topic_len;
    }
    rc = face_tss_envelope_decode(data, len, out);
    nng_msg_free(msg);
    return rc;
}

/* Callback loop: 100 ms poll slices so stop() is responsive. Callback
 * state is copied under cb_lock; the user callback runs with no
 * transport locks held. */
static void cb_loop(void *arg)
{
    FACE_TSS_TRANSPORT *t = (FACE_TSS_TRANSPORT *)arg;
    int free_pending;
    cb_take(t);
#if defined(_WIN32)
    t->cb_tid = GetCurrentThreadId();
#else
    t->cb_tid = pthread_self();
#endif
    t->cb_tid_valid = 1;
    cb_drop(t);
    for (;;) {
        FACE_TSS_ENVELOPE env;
        FACE_TSS_ENVELOPE_CB cb;
        void *user;
        int stop;
        FACE_TSS_RETURN_CODE rc;
        cb_take(t);
        cb = t->cb;
        user = t->cb_user;
        stop = t->cb_stop;
        cb_drop(t);
        if (stop || !cb)
            break;
        rc = face_tss_transport_receive(t, 100 * 1000000LL, &env);
        if (rc == FACE_TSS_RC_TIMED_OUT)
            continue;
        if (rc != FACE_TSS_RC_NO_ERROR)
            continue;
        cb_take(t);
        cb = t->cb;
        user = t->cb_user;
        stop = t->cb_stop;
        cb_drop(t);
        if (!stop && cb)
            cb(&env, user);
        face_tss_envelope_fini(&env);
    }
    /* Exiting: if transport_close ran on this thread (destroy from
     * within the data callback), finish the free here. This thread
     * touches neither t nor its locks after this point. The nng_thread
     * handle for this thread is intentionally leaked: a thread cannot
     * join itself. */
    cb_take(t);
    free_pending = t->close_pending;
    cb_drop(t);
    if (free_pending) {
        locks_fini(t);
        free(t);
    }
}

FACE_TSS_RETURN_CODE face_tss_transport_callback_start(
    FACE_TSS_TRANSPORT *t, FACE_TSS_ENVELOPE_CB cb, void *user)
{
    nng_thread *zombie;
    int rv;
    if (!t || !cb)
        return FACE_TSS_RC_INVALID_PARAM;
    cb_take(t);
    if (t->cb_thread) {
        cb_drop(t);
        return FACE_TSS_RC_NO_ACTION;
    }
    /* Reap a self-stopped zombie from an earlier stop. It is guaranteed
     * to exit (stop was signaled); joining from any other thread is
     * safe. If we ARE that thread (re-register from within the data
     * callback), leave it: the next stop from another thread reaps it. */
    zombie = t->cb_zombie;
    if (zombie) {
#if defined(_WIN32)
        int self = (GetCurrentThreadId() == t->cb_zombie_tid);
#else
        int self = pthread_equal(pthread_self(), t->cb_zombie_tid);
#endif
        if (!self)
            t->cb_zombie = NULL;
        else
            zombie = NULL;
    }
    t->cb = cb;
    t->cb_user = user;
    t->cb_stop = 0;
    t->cb_tid_valid = 0;
    /* Create the thread while holding cb_lock so a concurrent stop
     * cannot slip between "cb is set" and "thread exists". The new
     * thread blocks briefly on cb_lock at entry; harmless. */
    rv = nng_thread_create(&t->cb_thread, cb_loop, t);
    if (rv != 0) {
        t->cb = NULL;
        t->cb_user = NULL;
        t->cb_thread = NULL;
        t->cb_zombie = zombie; /* put back; unreachable in practice */
        cb_drop(t);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    cb_drop(t);
    if (zombie)
        nng_thread_destroy(zombie);
    return FACE_TSS_RC_NO_ERROR;
}

/* Signal the dispatch thread to exit and join it. Joins run with no
 * locks held: the dispatch thread may be blocked acquiring a higher
 * level lock (e.g. the TSS instance lock), so joining under such a lock
 * would deadlock. Concurrent stops are safe: only one caller observes
 * each thread handle. A stop issued from the dispatch thread itself
 * (self-unregister from the data callback) signals the stop and parks
 * the handle as a zombie instead of joining: the thread exits on its
 * own and a later stop/start/shutdown/close from another thread reaps
 * it. */
FACE_TSS_RETURN_CODE face_tss_transport_callback_stop(FACE_TSS_TRANSPORT *t)
{
    nng_thread *thr, *zombie;
#if defined(_WIN32)
    DWORD me = GetCurrentThreadId();
#else
    pthread_t me = pthread_self();
#endif
    int self;
    if (!t)
        return FACE_TSS_RC_INVALID_PARAM;
    cb_take(t);
    thr = t->cb_thread;
    zombie = t->cb_zombie;
    t->cb_zombie = NULL;
    if (!thr) {
        cb_drop(t);
        if (!zombie)
            return FACE_TSS_RC_NO_ACTION;
        /* Reap a zombie from an earlier self-stop. */
#if defined(_WIN32)
        self = (me == t->cb_zombie_tid);
#else
        self = pthread_equal(me, t->cb_zombie_tid);
#endif
        if (self) {
            /* We are the zombie thread asking again: already stopping. */
            cb_take(t);
            t->cb_zombie = zombie;
            cb_drop(t);
            return FACE_TSS_RC_NO_ERROR;
        }
        nng_thread_destroy(zombie);
        return FACE_TSS_RC_NO_ACTION;
    }
    t->cb = NULL;
    t->cb_user = NULL;
    t->cb_stop = 1;
    t->cb_thread = NULL;
#if defined(_WIN32)
    self = (t->cb_tid_valid && me == t->cb_tid);
#else
    self = (t->cb_tid_valid && pthread_equal(me, t->cb_tid));
#endif
    if (self) {
        t->cb_zombie = thr;
        t->cb_zombie_tid = t->cb_tid;
    }
    cb_drop(t);
    if (self)
        return FACE_TSS_RC_NO_ERROR;
    /* Not the dispatch thread: wait for it to exit (bounded by the poll
     * slice) and reap it, plus any earlier zombie. */
    nng_thread_destroy(thr);
    if (zombie)
        nng_thread_destroy(zombie);
    return FACE_TSS_RC_NO_ERROR;
}
