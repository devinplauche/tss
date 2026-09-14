/* FaceTss: the FACE TS interface. See tss.h for contract. */

#include "face_tss/tss.h"
#include "face_tss/typed.h"
#include "tss_priv.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif

typedef struct FACE_TSS_CONN {
    FACE_TSS_CONNECTION_CONFIG cfg;
    FACE_TSS_TRANSPORT *transport;
    int closed;
    uint64_t send_seq;
    FACE_TSS_MESSAGE_CB cb;
    void *cb_user;
    void (*cb_user_fini)(void *); /* cleanup for cb_user, may be NULL */
    void *cb_ctx; /* CB_CTX owned by this conn, freed on unregister/destroy */
} FACE_TSS_CONN;

/* Registered data types (FACE TypedTS type support). Fixed array; entries
 * are never removed, so pointers handed out stay valid. */
#define FACE_TSS_MAX_TYPES 32

struct FACE_TSS {
    char instance_name[FACE_TSS_MAX_STRING];
    FACE_TSS_UID_TYPE source_id;
    FACE_TSS_UID_TYPE next_instance_uid;      /* per-message instance UIDs */
    FACE_TSS_TRANSACTION_ID_TYPE next_txn;    /* assigned transaction IDs */
#if defined(_WIN32)
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
    int initialized;
    FACE_TSS_CONFIG config;
    FACE_TSS_CONNECTION_ID_TYPE next_id;
    FACE_TSS_CONN **conns;      /* indexed by (id - 1), NULL when free */
    size_t conns_cap;
    FACE_TSS_TYPE_SUPPORT types[FACE_TSS_MAX_TYPES];
    size_t ntypes;
    FACE_TSS_STATS stats;
};

/* ------------------------------------------------------------------ */
/* platform bits                                                      */
/* ------------------------------------------------------------------ */

static void lock_init(FACE_TSS *t)
{
#if defined(_WIN32)
    InitializeCriticalSection(&t->lock);
#else
    pthread_mutex_init(&t->lock, NULL);
#endif
}

static void lock_fini(FACE_TSS *t)
{
#if defined(_WIN32)
    DeleteCriticalSection(&t->lock);
#else
    pthread_mutex_destroy(&t->lock);
#endif
}

static void lock_take(FACE_TSS *t)
{
#if defined(_WIN32)
    EnterCriticalSection(&t->lock);
#else
    pthread_mutex_lock(&t->lock);
#endif
}

static void lock_drop(FACE_TSS *t)
{
#if defined(_WIN32)
    LeaveCriticalSection(&t->lock);
#else
    pthread_mutex_unlock(&t->lock);
#endif
}

static int64_t now_ns(void)
{
#if defined(_WIN32)
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimePreciseAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* 100-ns ticks since 1601 -> ns since 1970 */
    return (int64_t)(u.QuadPart * 100ULL - 11644473600000000000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

static FACE_TSS_UID_TYPE make_guid(void)
{
    uint64_t v = 0;
#if defined(_WIN32)
    BCRYPT_ALG_HANDLE h = NULL;
    if (BCryptOpenAlgorithmProvider(&h, BCRYPT_RNG_ALGORITHM, NULL, 0) == 0) {
        BCryptGenRandom(h, (PUCHAR)&v, sizeof(v), 0);
        BCryptCloseAlgorithmProvider(h, 0);
    } else {
        v = ((uint64_t)rand() << 32) ^ (uint64_t)rand();
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, &v, sizeof(v));
        (void)n;
        close(fd);
        if (!v)
            v = (uint64_t)rand();
    } else {
        v = ((uint64_t)rand() << 32) ^ (uint64_t)rand();
    }
#endif
    v &= (uint64_t)0x7FFFFFFFFFFFFFFFULL; /* keep positive */
    return (FACE_TSS_UID_TYPE)(v ? v : 1);
}

/* ------------------------------------------------------------------ */
/* messages                                                           */
/* ------------------------------------------------------------------ */

void face_tss_message_fini(FACE_TSS_MESSAGE *msg)
{
    if (!msg)
        return;
    free(msg->payload);
    msg->payload = NULL;
    msg->payload_len = 0;
}

const char *face_tss_rc_str(FACE_TSS_RETURN_CODE rc)
{
    switch (rc) {
    case FACE_TSS_RC_NO_ERROR: return "NO_ERROR";
    case FACE_TSS_RC_NO_ACTION: return "NO_ACTION";
    case FACE_TSS_RC_NOT_AVAILABLE: return "NOT_AVAILABLE";
    case FACE_TSS_RC_INVALID_PARAM: return "INVALID_PARAM";
    case FACE_TSS_RC_INVALID_CONFIG: return "INVALID_CONFIG";
    case FACE_TSS_RC_INVALID_MODE: return "INVALID_MODE";
    case FACE_TSS_RC_TIMED_OUT: return "TIMED_OUT";
    case FACE_TSS_RC_ADDR_IN_USE: return "ADDR_IN_USE";
    case FACE_TSS_RC_PERMISSION_DENIED: return "PERMISSION_DENIED";
    case FACE_TSS_RC_MESSAGE_STALE: return "MESSAGE_STALE";
    case FACE_TSS_RC_IN_PROGRESS: return "IN_PROGRESS";
    case FACE_TSS_RC_CONNECTION_CLOSED: return "CONNECTION_CLOSED";
    case FACE_TSS_RC_DATA_BUFFER_TOO_SMALL: return "DATA_BUFFER_TOO_SMALL";
    case FACE_TSS_RC_DATA_OVERFLOW: return "DATA_OVERFLOW";
    default: return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

FACE_TSS *face_tss_create(const char *instance_name)
{
    FACE_TSS *t = (FACE_TSS *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    if (instance_name) {
        strncpy(t->instance_name, instance_name, sizeof(t->instance_name) - 1);
    } else {
        strcpy(t->instance_name, "face-tss");
    }
    t->source_id = make_guid();
    t->next_instance_uid = make_guid();
    t->next_txn = 1;
    t->next_id = 1;
    face_tss_config_init(&t->config, t->instance_name);
    lock_init(t);
    return t;
}

static void destroy_conn(FACE_TSS_CONN *c)
{
    if (!c)
        return;
    c->closed = 1;
    c->cb = NULL;
    face_tss_transport_close(c->transport);
    free(c->cb_ctx);
    c->cb_ctx = NULL;
    if (c->cb_user_fini) {
        c->cb_user_fini(c->cb_user);
        c->cb_user_fini = NULL;
    }
    c->cb_user = NULL;
    free(c);
}

void face_tss_destroy(FACE_TSS *tss)
{
    size_t i;
    if (!tss)
        return;
    lock_take(tss);
    for (i = 0; i < tss->conns_cap; i++) {
        destroy_conn(tss->conns[i]);
        tss->conns[i] = NULL;
    }
    free(tss->conns);
    tss->conns = NULL;
    tss->conns_cap = 0;
    face_tss_config_fini(&tss->config);
    tss->initialized = 0;
    lock_drop(tss);
    lock_fini(tss);
    free(tss);
}

FACE_TSS_RETURN_CODE face_tss_initialize(
    FACE_TSS *tss, const FACE_TSS_CONFIG *config)
{
    FACE_TSS_CONFIG copy;
    size_t i;
    FACE_TSS_RETURN_CODE rc;
    if (!tss || !config)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    if (tss->initialized) {
        lock_drop(tss);
        return FACE_TSS_RC_NO_ACTION;
    }
    face_tss_config_init(&copy, config->instance_name);
    rc = FACE_TSS_RC_NO_ERROR;
    for (i = 0; i < config->count; i++) {
        rc = face_tss_config_add(&copy, &config->connections[i]);
        if (rc != FACE_TSS_RC_NO_ERROR)
            break;
    }
    if (rc != FACE_TSS_RC_NO_ERROR) {
        face_tss_config_fini(&copy);
        lock_drop(tss);
        return rc;
    }
    face_tss_config_fini(&tss->config);
    tss->config = copy;
    tss->initialized = 1;
    lock_drop(tss);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_UID_TYPE face_tss_source_id(FACE_TSS *tss)
{
    FACE_TSS_UID_TYPE v = 0;
    if (!tss)
        return 0;
    lock_take(tss);
    v = tss->source_id;
    lock_drop(tss);
    return v;
}

FACE_TSS_RETURN_CODE face_tss_stats(FACE_TSS *tss, FACE_TSS_STATS *out)
{
    if (!tss || !out)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    *out = tss->stats;
    lock_drop(tss);
    return FACE_TSS_RC_NO_ERROR;
}

/* ------------------------------------------------------------------ */
/* typed type-support registry (see typed.c for the public operations) */
/* ------------------------------------------------------------------ */

FACE_TSS_RETURN_CODE face_tss_priv_typed_register(
    FACE_TSS *tss, const FACE_TSS_TYPE_SUPPORT *tsupport)
{
    size_t i;
    if (!tss || !tsupport || !tsupport->type_name[0] ||
        !tsupport->serialize || !tsupport->deserialize || !tsupport->fini ||
        tsupport->value_size == 0)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    for (i = 0; i < tss->ntypes; i++) {
        if (strcmp(tss->types[i].type_name, tsupport->type_name) == 0) {
            lock_drop(tss);
            return FACE_TSS_RC_NO_ACTION;
        }
    }
    if (tss->ntypes >= FACE_TSS_MAX_TYPES) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    tss->types[tss->ntypes++] = *tsupport;
    lock_drop(tss);
    return FACE_TSS_RC_NO_ERROR;
}

const FACE_TSS_TYPE_SUPPORT *face_tss_priv_typed_lookup(
    FACE_TSS *tss, const char *type_name)
{
    size_t i;
    const FACE_TSS_TYPE_SUPPORT *found = NULL;
    if (!tss || !type_name)
        return NULL;
    lock_take(tss);
    for (i = 0; i < tss->ntypes; i++) {
        if (strcmp(tss->types[i].type_name, type_name) == 0) {
            found = &tss->types[i];
            break;
        }
    }
    lock_drop(tss);
    /* Entries are never removed and the array is fixed, so the pointer
     * stays valid after the lock is released. */
    return found;
}

/* ------------------------------------------------------------------ */
/* connections                                                        */
/* ------------------------------------------------------------------ */

/* Caller holds the lock. Returns NULL when id invalid/closed. */
static FACE_TSS_CONN *find_open(FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE id)
{
    size_t idx;
    FACE_TSS_CONN *c;
    if (id <= 0)
        return NULL;
    idx = (size_t)(id - 1);
    if (idx >= tss->conns_cap)
        return NULL;
    c = tss->conns[idx];
    if (!c || c->closed)
        return NULL;
    return c;
}

FACE_TSS_RETURN_CODE face_tss_create_connection(
    FACE_TSS *tss, const char *name,
    FACE_TSS_CONNECTION_ID_TYPE *connection_id,
    FACE_TSS_MESSAGE_SIZE_TYPE *max_message_size,
    FACE_TIMEOUT_TYPE timeout_ns)
{
    const FACE_TSS_CONNECTION_CONFIG *cfg;
    FACE_TSS_TRANSPORT *tr = NULL;
    FACE_TSS_CONN *c;
    FACE_TSS_RETURN_CODE rc;
    size_t idx;
    (void)timeout_ns; /* accepted for FACE signature parity; unused */
    if (!tss || !name || !connection_id || !max_message_size)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    if (!tss->initialized) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    cfg = face_tss_config_lookup(&tss->config, name);
    if (!cfg) {
        lock_drop(tss);
        return FACE_TSS_RC_INVALID_PARAM;
    }
    /* Open the socket before taking a table slot (may block briefly on
     * dial; done under lock for table consistency - matches the Python
     * implementation's locking discipline). */
    rc = face_tss_transport_open(cfg, &tr);
    if (rc != FACE_TSS_RC_NO_ERROR) {
        lock_drop(tss);
        return rc;
    }
    c = (FACE_TSS_CONN *)calloc(1, sizeof(*c));
    if (!c) {
        face_tss_transport_close(tr);
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    c->cfg = *cfg;
    c->transport = tr;
    idx = (size_t)(tss->next_id - 1);
    if (idx >= tss->conns_cap) {
        size_t want = tss->conns_cap ? tss->conns_cap * 2 : 8;
        FACE_TSS_CONN **p;
        while (idx >= want)
            want *= 2;
        p = (FACE_TSS_CONN **)realloc(tss->conns, want * sizeof(*p));
        if (!p) {
            face_tss_transport_close(tr);
            free(c);
            lock_drop(tss);
            return FACE_TSS_RC_NOT_AVAILABLE;
        }
        memset(p + tss->conns_cap, 0,
               (want - tss->conns_cap) * sizeof(*p));
        tss->conns = p;
        tss->conns_cap = want;
    }
    tss->conns[idx] = c;
    *connection_id = tss->next_id++;
    *max_message_size = cfg->max_message_size;
    lock_drop(tss);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_destroy_connection(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id)
{
    size_t idx;
    if (!tss)
        return FACE_TSS_RC_INVALID_PARAM;
    if (connection_id == FACE_TSS_CONNECTION_ID_INVALID)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    if (!tss->initialized) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    idx = (size_t)(connection_id - 1);
    if (idx >= tss->conns_cap || !tss->conns[idx]) {
        lock_drop(tss);
        return FACE_TSS_RC_NO_ACTION;
    }
    destroy_conn(tss->conns[idx]);
    tss->conns[idx] = NULL;
    lock_drop(tss);
    return FACE_TSS_RC_NO_ERROR;
}

static int can_send(const FACE_TSS_CONN *c)
{
    return c->cfg.direction == FACE_TSS_SOURCE ||
           c->cfg.direction == FACE_TSS_BI_DIRECTIONAL;
}

static int can_receive(const FACE_TSS_CONN *c)
{
    return c->cfg.direction == FACE_TSS_DESTINATION ||
           c->cfg.direction == FACE_TSS_BI_DIRECTIONAL;
}

/* ------------------------------------------------------------------ */
/* messaging                                                          */
/* ------------------------------------------------------------------ */

FACE_TSS_RETURN_CODE face_tss_priv_send_guid(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    FACE_TSS_MESSAGE_GUID_TYPE message_guid,
    const uint8_t *payload, size_t payload_len)
{
    FACE_TSS_CONN *c;
    FACE_TSS_ENVELOPE env;
    FACE_TSS_RETURN_CODE rc;
    if (!tss || !transaction_id)
        return FACE_TSS_RC_INVALID_PARAM;
    if (payload_len > 0 && !payload)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    if (!tss->initialized) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    c = find_open(tss, connection_id);
    if (!c) {
        lock_drop(tss);
        return FACE_TSS_RC_CONNECTION_CLOSED;
    }
    if (!can_send(c)) {
        lock_drop(tss);
        return FACE_TSS_RC_INVALID_MODE;
    }
    if ((FACE_TSS_MESSAGE_SIZE_TYPE)payload_len > c->cfg.max_message_size ||
        payload_len > (size_t)INT32_MAX) {
        lock_drop(tss);
        return FACE_TSS_RC_DATA_BUFFER_TOO_SMALL;
    }
    /* inout transaction_id: assign one when the caller passes unspecified. */
    if (*transaction_id == FACE_TSS_TRANSACTION_ID_UNSPECIFIED)
        *transaction_id = tss->next_txn++;
    c->send_seq++;
    face_tss_envelope_init(&env);
    strncpy(env.connection_name, c->cfg.name, sizeof(env.connection_name) - 1);
    env.transaction_id = *transaction_id;
    env.source_id = tss->source_id;
    env.sequence_number = c->send_seq;
    env.timestamp_ns = now_ns();
    env.instance_uid = tss->next_instance_uid++;
    env.message_guid = message_guid;
    /* borrow caller bytes: encode copies into the builder, no copy here */
    env.payload = (uint8_t *)payload;
    env.payload_len = payload_len;
    rc = face_tss_transport_send(c->transport, &env, timeout_ns);
    env.payload = NULL; /* not owned */
    env.payload_len = 0;
    if (rc == FACE_TSS_RC_NO_ERROR)
        tss->stats.sent++;
    else
        tss->stats.send_errors++;
    lock_drop(tss);
    return rc;
}

FACE_TSS_RETURN_CODE face_tss_send_message(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    const uint8_t *payload, size_t payload_len)
{
    return face_tss_priv_send_guid(tss, connection_id, timeout_ns,
                                   transaction_id,
                                   FACE_TSS_MESSAGE_GUID_UNSPECIFIED,
                                   payload, payload_len);
}

static void envelope_to_message(const FACE_TSS_ENVELOPE *env, FACE_TSS_MESSAGE *msg)
{
    memset(msg, 0, sizeof(*msg));
    /* FACE::TSS::HEADER_TYPE: instance UID, source UID, timestamp. */
    msg->header.instance_uid = env->instance_uid;
    msg->header.source_uid = env->source_id;
    msg->header.timestamp = env->timestamp_ns;
    msg->message_guid = env->message_guid;
    if (env->payload_len > 0) {
        msg->payload = (uint8_t *)malloc(env->payload_len);
        if (msg->payload) {
            memcpy(msg->payload, env->payload, env->payload_len);
            msg->payload_len = env->payload_len;
        }
    }
}

FACE_TSS_RETURN_CODE face_tss_receive_message(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns, size_t min_message_size,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    FACE_TSS_MESSAGE *msg_out,
    FACE_TSS_QOS_EVENT *qos_out)
{
    FACE_TSS_CONN *c;
    FACE_TSS_TRANSPORT *tr;
    FACE_TSS_RETURN_CODE rc;
    FACE_TSS_ENVELOPE env;
    if (!tss || !transaction_id || !msg_out)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    if (!tss->initialized) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    c = find_open(tss, connection_id);
    if (!c) {
        lock_drop(tss);
        return FACE_TSS_RC_CONNECTION_CLOSED;
    }
    if (!can_receive(c)) {
        lock_drop(tss);
        return FACE_TSS_RC_INVALID_MODE;
    }
    tr = c->transport;
    /* Receive while holding the lock (matches the Python implementation;
     * nng sockets are thread-safe so callbacks can proceed). */
    face_tss_envelope_init(&env);
    rc = face_tss_transport_receive(tr, timeout_ns, &env);
    if (rc == FACE_TSS_RC_TIMED_OUT) {
        tss->stats.receive_timeouts++;
        lock_drop(tss);
        return rc;
    }
    if (rc != FACE_TSS_RC_NO_ERROR) {
        lock_drop(tss);
        return rc;
    }
    if (env.payload_len < min_message_size) {
        face_tss_envelope_fini(&env);
        lock_drop(tss);
        return FACE_TSS_RC_DATA_BUFFER_TOO_SMALL;
    }
    envelope_to_message(&env, msg_out);
    *transaction_id = env.transaction_id;
    if (qos_out)
        face_tss_qos_event_init(qos_out); /* no QoS policies yet */
    face_tss_envelope_fini(&env);
    tss->stats.received++;
    lock_drop(tss);
    if (msg_out->payload_len > 0 && !msg_out->payload)
        return FACE_TSS_RC_NOT_AVAILABLE;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_try_receive(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    FACE_TSS_MESSAGE *msg_out, FACE_TSS_QOS_EVENT *qos_out,
    bool *has_msg)
{
    FACE_TSS_RETURN_CODE rc;
    if (!has_msg)
        return FACE_TSS_RC_INVALID_PARAM;
    *has_msg = false;
    rc = face_tss_receive_message(tss, connection_id, 0, 0,
                                  transaction_id, msg_out, qos_out);
    if (rc == FACE_TSS_RC_TIMED_OUT)
        return FACE_TSS_RC_NO_ERROR;
    if (rc == FACE_TSS_RC_NO_ERROR)
        *has_msg = true;
    return rc;
}

/* ------------------------------------------------------------------ */
/* callbacks                                                          */
/* ------------------------------------------------------------------ */

typedef struct CB_CTX {
    FACE_TSS *tss;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_CB cb;
    void *user;
} CB_CTX;

static void cb_dispatch(const FACE_TSS_ENVELOPE *env, void *user)
{
    CB_CTX *ctx = (CB_CTX *)user;
    FACE_TSS_MESSAGE msg;
    FACE_TSS_MESSAGE_CB cb = NULL;
    void *cb_user = NULL;
    FACE_TSS *tss = ctx->tss;
    FACE_TSS_CONNECTION_ID_TYPE id = ctx->id;
    FACE_TSS_QOS_EVENT qos;
    FACE_TSS_RETURN_CODE cb_rc = FACE_TSS_RC_NO_ERROR;
    lock_take(tss);
    {
        FACE_TSS_CONN *c = find_open(tss, id);
        if (c && c->cb) {
            cb = c->cb;
            cb_user = c->cb_user;
        }
        tss->stats.received++;
    }
    lock_drop(tss);
    if (!cb)
        return;
    /* NOTE: ctx is owned by the connection; freed on unregister/destroy. */
    envelope_to_message(env, &msg);
    face_tss_qos_event_init(&qos); /* no QoS policies yet */
    cb(id, env->transaction_id, env->message_guid,
       msg.payload, msg.payload_len,
       &msg.header, &qos, cb_user, &cb_rc);
    (void)cb_rc;
    face_tss_message_fini(&msg);
}

FACE_TSS_RETURN_CODE face_tss_priv_register_callback_ex(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_MESSAGE_CB cb, void *user, void (*user_fini)(void *))
{
    FACE_TSS_CONN *c;
    CB_CTX *ctx;
    FACE_TSS_RETURN_CODE rc;
    if (!tss || !cb)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    if (!tss->initialized) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    c = find_open(tss, connection_id);
    if (!c) {
        lock_drop(tss);
        return FACE_TSS_RC_CONNECTION_CLOSED;
    }
    if (!can_receive(c)) {
        lock_drop(tss);
        return FACE_TSS_RC_INVALID_MODE;
    }
    if (c->cb) {
        lock_drop(tss);
        return FACE_TSS_RC_NO_ACTION;
    }
    ctx = (CB_CTX *)malloc(sizeof(*ctx));
    if (!ctx) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    ctx->tss = tss;
    ctx->id = connection_id;
    ctx->cb = cb;
    ctx->user = user;
    rc = face_tss_transport_callback_start(c->transport, cb_dispatch, ctx);
    if (rc != FACE_TSS_RC_NO_ERROR) {
        free(ctx);
        lock_drop(tss);
        return rc;
    }
    c->cb = cb;
    c->cb_user = user;
    c->cb_user_fini = user_fini;
    c->cb_ctx = ctx;
    lock_drop(tss);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_register_callback(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_MESSAGE_CB cb, void *user)
{
    return face_tss_priv_register_callback_ex(tss, connection_id, cb, user,
                                              NULL);
}

FACE_TSS_RETURN_CODE face_tss_unregister_callback(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id)
{
    FACE_TSS_CONN *c;
    if (!tss)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    if (!tss->initialized) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    c = find_open(tss, connection_id);
    if (!c) {
        lock_drop(tss);
        return FACE_TSS_RC_CONNECTION_CLOSED;
    }
    if (!c->cb) {
        lock_drop(tss);
        return FACE_TSS_RC_NO_ACTION;
    }
    face_tss_transport_callback_stop(c->transport);
    c->cb = NULL;
    if (c->cb_user_fini) {
        c->cb_user_fini(c->cb_user);
        c->cb_user_fini = NULL;
    }
    c->cb_user = NULL;
    free(c->cb_ctx);
    c->cb_ctx = NULL;
    lock_drop(tss);
    return FACE_TSS_RC_NO_ERROR;
}
