/* FaceTss: the FACE TS interface. See tss.h for contract. */

#include "face_tss/tss.h"
#include "face_tss/configuration.h"
#include "face_tss/qos.h"
#include "face_tss/typed.h"
#include "tss_priv.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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
    /* Reference count, guarded by the TSS lock. The connection table
     * holds one reference; each operation that drops the TSS lock across
     * blocking I/O holds another. The last release tears the connection
     * down with no locks held (teardown_conn). */
    int refcount;
    uint64_t send_seq;
    /* Receive-side sequence baseline for QoS reliability gap monitoring.
     * Guarded by the TSS lock; only meaningful while a RELIABILITY policy
     * is set on this connection. A source change (or the first message)
     * re-baselines without counting a gap. */
    FACE_TSS_UID_TYPE rx_source;
    uint64_t rx_seq;
    int rx_seen;
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
    CONDITION_VARIABLE zero_cond;
#else
    pthread_mutex_t lock;
    pthread_cond_t zero_cond;
#endif
    int initialized;
    /* destroy() sets destroying under the lock; afterwards every public
     * entry point fails with NOT_AVAILABLE, in-flight blocking I/O is
     * aborted, and destroy() waits on zero_cond until refcount (the
     * number of operations currently running without the lock) drains. */
    int destroying;
    int refcount;
    FACE_TSS_CONFIG config;
    FACE_TSS_CONNECTION_ID_TYPE next_id;
    FACE_TSS_CONN **conns;      /* indexed by (id - 1), NULL when free */
    size_t conns_cap;
    size_t conns_open;          /* live connections; bounded (3.2 limit) */
    FACE_TSS_TYPE_SUPPORT types[FACE_TSS_MAX_TYPES];
    size_t ntypes;
    FACE_TSS_STATS stats;
    FACE_TSS_CONFIGURATION config_iface; /* copied by Set_Reference */
    int config_iface_set;
    FACE_TSS_QOS *qos; /* per-connection QoS policies (staleness etc.) */
};

/* ------------------------------------------------------------------ */
/* platform bits                                                      */
/* ------------------------------------------------------------------ */

static void lock_init(FACE_TSS *t)
{
#if defined(_WIN32)
    InitializeCriticalSection(&t->lock);
    InitializeConditionVariable(&t->zero_cond);
#else
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->zero_cond, NULL);
#endif
}

static void lock_fini(FACE_TSS *t)
{
#if defined(_WIN32)
    DeleteCriticalSection(&t->lock);
    /* CONDITION_VARIABLE needs no destruction. */
#else
    pthread_cond_destroy(&t->zero_cond);
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

/* ------------------------------------------------------------------ */
/* reference counting                                                 */
/*                                                                    */
/* Operations that block (send/receive/unregister) drop the TSS lock   */
/* while they wait, so the connections they touch must stay alive      */
/* across the unlocked window. Every helper below documents whether    */
/* the caller holds the lock.                                         */
/* ------------------------------------------------------------------ */

/* Enter an operation that will drop the TSS lock across blocking I/O.
 * On success the TSS lock is NOT held and the instance cannot be freed
 * until tss_exit_io runs. Returns NOT_AVAILABLE when destroy() is in
 * progress. */
static FACE_TSS_RETURN_CODE tss_enter_io(FACE_TSS *tss)
{
    lock_take(tss);
    if (tss->destroying) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    tss->refcount++;
    lock_drop(tss);
    return FACE_TSS_RC_NO_ERROR;
}

/* Caller holds the TSS lock. */
static void tss_exit_io_locked(FACE_TSS *tss)
{
    if (--tss->refcount == 0 && tss->destroying) {
#if defined(_WIN32)
        WakeConditionVariable(&tss->zero_cond);
#else
        pthread_cond_signal(&tss->zero_cond);
#endif
    }
}

/* Caller does NOT hold the TSS lock. */
static void tss_exit_io(FACE_TSS *tss)
{
    lock_take(tss);
    tss_exit_io_locked(tss);
    lock_drop(tss);
}

/* Caller holds the TSS lock. */
static void conn_ref(FACE_TSS_CONN *c)
{
    c->refcount++;
}

/* Caller holds the TSS lock. Drops one reference; returns 1 when the
 * caller must run teardown_conn(c) with no locks held. While the
 * instance is being destroyed, teardown is destroy()'s job. */
static int conn_release_locked(FACE_TSS *tss, FACE_TSS_CONN *c)
{
    if (--c->refcount == 0 && c->closed && !tss->destroying)
        return 1;
    return 0;
}

/* Tear down a connection whose reference count reached zero. No TSS lock
 * is held: transport_close joins the callback dispatch thread, and the
 * dispatcher may be blocked acquiring the TSS lock, so joining under it
 * would deadlock. The join also guarantees no dispatch is in flight
 * when the callback context and user data are released. */
static void teardown_conn(FACE_TSS_CONN *c)
{
    void *ctx = c->cb_ctx;
    void *user = c->cb_user;
    void (*user_fini)(void *) = c->cb_user_fini;
    face_tss_transport_close(c->transport);
    free(ctx);
    if (user_fini)
        user_fini(user);
    free(c);
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
    case FACE_TSS_RC_RESOURCE_LIMIT_REACHED: return "RESOURCE_LIMIT_REACHED";
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
    t->qos = face_tss_qos_create();
    if (!t->qos) {
        face_tss_config_fini(&t->config);
        lock_init(t);
        lock_fini(t);
        free(t);
        return NULL;
    }
    lock_init(t);
    return t;
}

/* face_tss_destroy / face_tss_destroy_connection replace this; see
 * teardown_conn above. Connections are removed from the table under the
 * TSS lock and torn down once their reference count drains, with no
 * locks held across the callback-thread join. */

void face_tss_destroy(FACE_TSS *tss)
{
    FACE_TSS_CONN **conns;
    size_t cap, i;
    FACE_TSS_QOS *qos;
    if (!tss)
        return;
    lock_take(tss);
    tss->destroying = 1;
    /* Steal the connection table so no new operation can find a
     * connection; operations already past tss_enter_io hold their own
     * references and keep the instance alive via tss->refcount. */
    conns = tss->conns;
    cap = tss->conns_cap;
    tss->conns = NULL;
    tss->conns_cap = 0;
    tss->conns_open = 0;
    tss->initialized = 0;
    face_tss_config_fini(&tss->config);
    lock_drop(tss);
    /* Abort in-flight blocking I/O with no locks held: closing the
     * sockets makes nng_sendmsg/nng_recvmsg return promptly, so the
     * reference count drains. The callback-thread join inside the
     * shutdown cannot deadlock against a dispatcher blocked on the TSS
     * lock, because the lock is not held here. */
    for (i = 0; i < cap; i++) {
        FACE_TSS_CONN *c = conns[i];
        if (!c)
            continue;
        c->closed = 1;
        face_tss_transport_shutdown(c->transport);
    }
    /* Wait for in-flight operations to finish. */
    lock_take(tss);
    while (tss->refcount > 0) {
#if defined(_WIN32)
        SleepConditionVariableCS(&tss->zero_cond, &tss->lock, INFINITE);
#else
        pthread_cond_wait(&tss->zero_cond, &tss->lock);
#endif
    }
    qos = tss->qos;
    tss->qos = NULL;
    lock_drop(tss);
    for (i = 0; i < cap; i++) {
        if (conns[i])
            teardown_conn(conns[i]);
    }
    free(conns);
    face_tss_qos_destroy(qos);
    lock_fini(tss);
    free(tss);
}

/* Shared Initialize core: deep-copy the config and mark initialized.
 * Callers hold no lock; idempotent (NO_ACTION when already initialized). */
static FACE_TSS_RETURN_CODE initialize_with_config(
    FACE_TSS *tss, const FACE_TSS_CONFIG *config)
{
    FACE_TSS_CONFIG copy;
    size_t i;
    FACE_TSS_RETURN_CODE rc;
    if (!tss || !config)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    if (tss->destroying) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
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

FACE_TSS_RETURN_CODE face_tss_initialize(
    FACE_TSS *tss, const FACE_TSS_CONFIG *config)
{
    /* Convenience adapter (not the FACE IDL shape): initialize directly
     * from a parsed config object. See face_tss_initialize_from_resource
     * for the FACE::TSS::Base::Initialize(CONFIGURATION_RESOURCE) shape. */
    return initialize_with_config(tss, config);
}

/* Built-in JSON resource adapter: "json:{...}" is parsed inline,
 * anything else is treated as a file path. */
static FACE_TSS_RETURN_CODE json_resource_load(const char *resource,
                                               FACE_TSS_CONFIG *out)
{
    static const char prefix[] = "json:";
    if (!resource || !out)
        return FACE_TSS_RC_INVALID_PARAM;
    if (strncmp(resource, prefix, sizeof(prefix) - 1) == 0)
        return face_tss_config_from_json(resource + sizeof(prefix) - 1,
                                         strlen(resource) - sizeof(prefix) + 1,
                                         out);
    return face_tss_config_from_file(resource, out);
}

FACE_TSS_RETURN_CODE face_tss_set_reference(
    FACE_TSS *tss, const char *interface_name,
    const FACE_TSS_CONFIGURATION *configuration,
    FACE_TSS_UID_TYPE id)
{
    (void)id; /* id delineates interface instances; one slot here */
    if (!tss || !interface_name || !configuration || !configuration->load)
        return FACE_TSS_RC_INVALID_PARAM;
    if (strcmp(interface_name, FACE_TSS_CONFIGURATION_INTERFACE_NAME) != 0)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    if (tss->destroying) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    if (tss->initialized) {
        lock_drop(tss);
        return FACE_TSS_RC_INVALID_MODE; /* steady state */
    }
    if (tss->config_iface_set) {
        int same = (tss->config_iface.load == configuration->load &&
                    tss->config_iface.user == configuration->user);
        lock_drop(tss);
        return same ? FACE_TSS_RC_NO_ACTION : FACE_TSS_RC_NOT_AVAILABLE;
    }
    tss->config_iface = *configuration; /* copied; user ptr passes through */
    tss->config_iface_set = 1;
    lock_drop(tss);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_initialize_from_resource(
    FACE_TSS *tss, const char *configuration_resource)
{
    FACE_TSS_CONFIG cfg;
    FACE_TSS_RETURN_CODE rc;
    FACE_TSS_CONFIGURATION iface;
    int have_iface;
    if (!tss || !configuration_resource ||
        strlen(configuration_resource) >= FACE_TSS_CONFIGURATION_RESOURCE_MAX)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    have_iface = tss->config_iface_set;
    iface = tss->config_iface;
    lock_drop(tss);
    face_tss_config_init(&cfg, "tss");
    if (have_iface)
        rc = iface.load(configuration_resource, &cfg, iface.user);
    else
        rc = json_resource_load(configuration_resource, &cfg);
    if (rc != FACE_TSS_RC_NO_ERROR) {
        face_tss_config_fini(&cfg);
        return rc;
    }
    rc = initialize_with_config(tss, &cfg);
    face_tss_config_fini(&cfg);
    return rc;
}

FACE_TSS_UID_TYPE face_tss_source_id(FACE_TSS *tss)
{
    FACE_TSS_UID_TYPE v = 0;
    if (!tss)
        return 0;
    lock_take(tss);
    v = tss->destroying ? 0 : tss->source_id;
    lock_drop(tss);
    return v;
}

FACE_TSS_RETURN_CODE face_tss_stats(FACE_TSS *tss, FACE_TSS_STATS *out)
{
    if (!tss || !out)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    if (tss->destroying) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
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
    if (tss->destroying) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
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
    if (tss->destroying) {
        lock_drop(tss);
        return NULL;
    }
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
    if (tss->destroying) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    if (!tss->initialized) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    if (tss->conns_open >= FACE_TSS_MAX_CONNECTIONS) {
        lock_drop(tss);
        return FACE_TSS_RC_RESOURCE_LIMIT_REACHED;
    }
    cfg = face_tss_config_lookup(&tss->config, name);
    if (!cfg) {
        lock_drop(tss);
        return FACE_TSS_RC_INVALID_PARAM;
    }
    /* Open the socket under the lock for table consistency. The dial is
     * non-blocking and listen() does not block, so this never waits on
     * the network; the blocking send/receive paths below drop the lock. */
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
    c->refcount = 1; /* the table's reference */
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
    tss->conns_open++;
    *connection_id = tss->next_id++;
    *max_message_size = cfg->max_message_size;
    lock_drop(tss);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_destroy_connection(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id)
{
    size_t idx;
    FACE_TSS_CONN *c;
    FACE_TSS_TRANSPORT *tr;
    int teardown;
    if (!tss)
        return FACE_TSS_RC_INVALID_PARAM;
    if (connection_id == FACE_TSS_CONNECTION_ID_INVALID)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    if (tss->destroying) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    if (!tss->initialized) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    idx = (size_t)(connection_id - 1);
    if (idx >= tss->conns_cap || !tss->conns[idx]) {
        lock_drop(tss);
        return FACE_TSS_RC_NO_ACTION;
    }
    /* Remove from the table so no new operation can find it; in-flight
     * operations hold their own references. New dispatches observe
     * cb == NULL and drop. */
    c = tss->conns[idx];
    tss->conns[idx] = NULL;
    tss->conns_open--;
    c->closed = 1;
    c->cb = NULL;
    tr = c->transport;
    /* Drop any QoS policies so the (capped) policy table cannot fill
     * with entries for dead connections. */
    face_tss_qos_clear_policies(tss->qos, connection_id);
    teardown = conn_release_locked(tss, c); /* drop the table's reference */
    lock_drop(tss);
    /* Abort in-flight blocking I/O and join the callback thread with no
     * locks held (see teardown_conn). Teardown is deferred when an
     * operation still holds a reference. */
    face_tss_transport_shutdown(tr);
    if (teardown)
        teardown_conn(c);
    return FACE_TSS_RC_NO_ERROR;
}

/* ------------------------------------------------------------------ */
/* QoS policies (extensions; not in the FACE IDL)                      */
/* ------------------------------------------------------------------ */

FACE_TSS_RETURN_CODE face_tss_set_qos_policy(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_QOS_POLICY_KIND kind, int64_t value_ns)
{
    FACE_TSS_RETURN_CODE rc;
    FACE_TSS_CONN *c;
    if (!tss)
        return FACE_TSS_RC_INVALID_PARAM;
    if (kind == FACE_TSS_QOS_MAX_AGE)
        kind = FACE_TSS_QOS_STALENESS; /* documented alias */
    lock_take(tss);
    if (tss->destroying) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    if (!tss->initialized) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    c = find_open(tss, connection_id);
    if (!c) {
        lock_drop(tss);
        return FACE_TSS_RC_CONNECTION_CLOSED;
    }
    if (kind == FACE_TSS_QOS_RELIABILITY) {
        /* Only the two documented levels exist. */
        if (value_ns != FACE_TSS_QOS_BEST_EFFORT &&
            value_ns != FACE_TSS_QOS_RELIABLE) {
            lock_drop(tss);
            return FACE_TSS_RC_INVALID_PARAM;
        }
        /* Neither nng transport offers reliable delivery (both are
         * best-effort), so RELIABLE is rejected loudly instead of being
         * silently pretended. Setting either level opts the connection
         * into receive-side sequence-gap monitoring. */
        if (value_ns == FACE_TSS_QOS_RELIABLE &&
            (c->cfg.transport == FACE_TSS_TRANSPORT_PUBSUB ||
             c->cfg.transport == FACE_TSS_TRANSPORT_BUS)) {
            lock_drop(tss);
            return FACE_TSS_RC_NOT_AVAILABLE;
        }
    }
    rc = face_tss_qos_set_policy(tss->qos, connection_id, kind, value_ns);
    lock_drop(tss);
    return rc;
}

FACE_TSS_RETURN_CODE face_tss_get_qos_policy(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_QOS_POLICY_KIND kind, int64_t *value_ns_out)
{
    FACE_TSS_RETURN_CODE rc;
    if (!tss)
        return FACE_TSS_RC_INVALID_PARAM;
    if (kind == FACE_TSS_QOS_MAX_AGE)
        kind = FACE_TSS_QOS_STALENESS; /* documented alias */
    lock_take(tss);
    if (tss->destroying) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    if (!tss->initialized) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    if (!find_open(tss, connection_id)) {
        lock_drop(tss);
        return FACE_TSS_RC_CONNECTION_CLOSED;
    }
    rc = face_tss_qos_get_policy(tss->qos, connection_id, kind,
                                 value_ns_out);
    lock_drop(tss);
    return rc;
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
    FACE_TSS_TRANSPORT *tr;
    FACE_TSS_CONNECTION_CONFIG cfg;
    FACE_TSS_ENVELOPE env;
    FACE_TSS_UID_TYPE source_id, instance_uid;
    uint64_t seq;
    int64_t prio = 0; /* QoS priority stamped on the envelope */
    FACE_TSS_RETURN_CODE rc;
    int teardown;
    if (!tss || !transaction_id)
        return FACE_TSS_RC_INVALID_PARAM;
    if (payload_len > 0 && !payload)
        return FACE_TSS_RC_INVALID_PARAM;
    rc = tss_enter_io(tss);
    if (rc != FACE_TSS_RC_NO_ERROR)
        return rc;
    lock_take(tss);
    if (!tss->initialized) {
        lock_drop(tss);
        goto io_exit_not_available;
    }
    c = find_open(tss, connection_id);
    if (!c) {
        lock_drop(tss);
        goto io_exit_closed;
    }
    if (!can_send(c)) {
        lock_drop(tss);
        goto io_exit_invalid_mode;
    }
    if ((FACE_TSS_MESSAGE_SIZE_TYPE)payload_len > c->cfg.max_message_size ||
        payload_len > (size_t)INT32_MAX) {
        lock_drop(tss);
        goto io_exit_too_small;
    }
    /* inout transaction_id: assign one when the caller passes unspecified. */
    if (*transaction_id == FACE_TSS_TRANSACTION_ID_UNSPECIFIED)
        *transaction_id = tss->next_txn++;
    seq = ++c->send_seq;
    source_id = tss->source_id;
    instance_uid = tss->next_instance_uid++;
    /* Snapshot everything the blocking call needs; the connection stays
     * alive across the unlocked window via the reference. */
    cfg = c->cfg;
    tr = c->transport;
    {
        /* QoS priority: stamp the connection's priority policy value on
         * every outgoing message (0 when unset). */
        int64_t pv = 0;
        if (face_tss_qos_get_policy(tss->qos, connection_id,
                                    FACE_TSS_QOS_PRIORITY, &pv) ==
            FACE_TSS_RC_NO_ERROR)
            prio = pv;
    }
    conn_ref(c);
    lock_drop(tss);

    face_tss_envelope_init(&env);
    memcpy(env.connection_name, cfg.name, sizeof(env.connection_name));
    env.connection_name[sizeof(env.connection_name) - 1] = '\0';
    env.transaction_id = *transaction_id;
    env.source_id = source_id;
    env.sequence_number = seq;
    env.timestamp_ns = now_ns();
    env.instance_uid = instance_uid;
    env.message_guid = message_guid;
    env.priority = prio;
    /* borrow caller bytes: encode copies into the builder, no copy here */
    env.payload = (uint8_t *)payload;
    env.payload_len = payload_len;
    rc = face_tss_transport_send(tr, &env, timeout_ns);
    env.payload = NULL; /* not owned */
    env.payload_len = 0;

    lock_take(tss);
    if (rc == FACE_TSS_RC_NO_ERROR)
        tss->stats.sent++;
    else
        tss->stats.send_errors++;
    teardown = conn_release_locked(tss, c);
    tss_exit_io_locked(tss);
    lock_drop(tss);
    if (teardown)
        teardown_conn(c);
    return rc;

io_exit_not_available:
    tss_exit_io(tss);
    return FACE_TSS_RC_NOT_AVAILABLE;
io_exit_closed:
    tss_exit_io(tss);
    return FACE_TSS_RC_CONNECTION_CLOSED;
io_exit_invalid_mode:
    tss_exit_io(tss);
    return FACE_TSS_RC_INVALID_MODE;
io_exit_too_small:
    tss_exit_io(tss);
    return FACE_TSS_RC_DATA_BUFFER_TOO_SMALL;
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

/* Populate the QoS event with honest, transport-observable data:
 * message_age_ns (receive time minus the send timestamp), the sender's
 * priority from the envelope, and -- when reliability monitoring is
 * active for the connection (seq_gap >= 0) -- the sequence gap observed
 * immediately before this message. Staleness/priority enforcement
 * happens upstream in receive_envelope / cb_dispatch, so by the time
 * this runs the message is known-fresh and above-threshold (or no
 * respective policy is set); see issues #2. */
static void qos_fill(FACE_TSS_QOS_EVENT *qos, const FACE_TSS_ENVELOPE *env,
                     int64_t seq_gap)
{
    int64_t age;
    if (!qos || !env)
        return;
    face_tss_qos_event_init(qos);
    age = now_ns() - (int64_t)env->timestamp_ns;
    if (age < 0)
        age = 0;
    qos->count = 0;
    strncpy(qos->elements[0].keyname, "message_age_ns",
            sizeof(qos->elements[0].keyname) - 1);
    qos->elements[0].keyname[sizeof(qos->elements[0].keyname) - 1] = '\0';
    snprintf(qos->elements[0].value, sizeof(qos->elements[0].value),
             "%lld", (long long)age);
    qos->count = 1;
    strncpy(qos->elements[1].keyname, "priority",
            sizeof(qos->elements[1].keyname) - 1);
    qos->elements[1].keyname[sizeof(qos->elements[1].keyname) - 1] = '\0';
    snprintf(qos->elements[1].value, sizeof(qos->elements[1].value),
             "%lld", (long long)env->priority);
    qos->count = 2;
    if (seq_gap >= 0 && qos->count < FACE_TSS_MAX_QOS_ELEMENTS) {
        strncpy(qos->elements[qos->count].keyname, "sequence_gap",
                sizeof(qos->elements[qos->count].keyname) - 1);
        qos->elements[qos->count].keyname
            [sizeof(qos->elements[qos->count].keyname) - 1] = '\0';
        snprintf(qos->elements[qos->count].value,
                 sizeof(qos->elements[qos->count].value),
                 "%lld", (long long)seq_gap);
        qos->count++;
    }
}

/* Sequence-gap bookkeeping for QoS reliability monitoring. Call with the
 * TSS lock held, once per wire-observed envelope (before policy
 * filtering, so stale/priority drops do not surface as phantom gaps).
 * Returns -1 when no RELIABILITY policy is set on the connection
 * (monitoring inactive); otherwise updates the per-connection baseline
 * and returns the number of skipped sequence numbers immediately before
 * this message (0 when none). A new source (or the first message)
 * re-baselines without counting a gap: on pub/sub a late subscriber
 * legitimately misses the messages sent before it arrived. */
static int64_t rx_gap_locked(FACE_TSS *tss, FACE_TSS_CONN *c,
                             FACE_TSS_CONNECTION_ID_TYPE connection_id,
                             const FACE_TSS_ENVELOPE *env)
{
    int64_t level;
    int64_t gap = 0;
    if (face_tss_qos_get_policy(tss->qos, connection_id,
                                FACE_TSS_QOS_RELIABILITY, &level) !=
        FACE_TSS_RC_NO_ERROR)
        return -1;
    (void)level; /* BEST_EFFORT and RELIABLE both enable monitoring */
    if (!c->rx_seen || env->source_id != c->rx_source) {
        /* (Re)baseline: no gap counted. */
    } else if (env->sequence_number > c->rx_seq + 1) {
        gap = (int64_t)(env->sequence_number - c->rx_seq - 1);
        tss->stats.reliability_gaps += (uint64_t)gap;
    }
    if (!c->rx_seen || env->source_id != c->rx_source ||
        env->sequence_number > c->rx_seq) {
        c->rx_source = env->source_id;
        c->rx_seq = env->sequence_number;
        c->rx_seen = 1;
    }
    return gap;
}

/* Shared receive core: validate, block on the transport, enforce
 * min_message_size. Returns the envelope; caller must fini it. The TSS
 * lock is NOT held across the blocking transport call, so one thread's
 * receive never stalls the rest of the instance; the connection is kept
 * alive by a reference, and stats/QoS are updated under the re-taken
 * lock. */
static FACE_TSS_RETURN_CODE receive_envelope(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns, size_t min_message_size,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    FACE_TSS_ENVELOPE *env, int64_t *seq_gap_out)
{
    FACE_TSS_CONN *c;
    FACE_TSS_TRANSPORT *tr;
    FACE_TSS_RETURN_CODE rc;
    int64_t prio_threshold = 0; /* QoS priority filter, snapshotted */
    int64_t seq_gap = -1;       /* -1 = reliability monitoring inactive */
    int64_t deadline_ns = -1;   /* -1 = infinite */
    int teardown;
    if (!tss || !transaction_id || !env || !seq_gap_out)
        return FACE_TSS_RC_INVALID_PARAM;
    rc = tss_enter_io(tss);
    if (rc != FACE_TSS_RC_NO_ERROR)
        return rc;
    lock_take(tss);
    if (!tss->initialized) {
        lock_drop(tss);
        tss_exit_io(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    c = find_open(tss, connection_id);
    if (!c) {
        lock_drop(tss);
        tss_exit_io(tss);
        return FACE_TSS_RC_CONNECTION_CLOSED;
    }
    if (!can_receive(c)) {
        lock_drop(tss);
        tss_exit_io(tss);
        return FACE_TSS_RC_INVALID_MODE;
    }
    tr = c->transport;
    {
        int64_t pv = 0;
        if (face_tss_qos_get_policy(tss->qos, connection_id,
                                    FACE_TSS_QOS_PRIORITY, &pv) ==
            FACE_TSS_RC_NO_ERROR)
            prio_threshold = pv;
    }
    conn_ref(c);
    lock_drop(tss);

    /* Priority enforcement: messages below the connection's priority
     * threshold are dropped and the receive keeps waiting for a
     * qualifying message until the timeout expires (deadline-based, so
     * drops do not extend the caller's timeout). A poll (timeout 0)
     * tries once; an infinite timeout waits until a qualifying message
     * arrives or the wait is interrupted (e.g. by destroy). There is no
     * FACE return code for "dropped by priority policy", so the drop
     * surfaces as TIMED_OUT when nothing qualifying arrives in time. */
    if (timeout_ns > 0)
        deadline_ns = now_ns() + timeout_ns;
    for (;;) {
        FACE_TIMEOUT_TYPE remaining = timeout_ns;
        if (deadline_ns >= 0) {
            remaining = deadline_ns - now_ns();
            if (remaining <= 0) {
                rc = FACE_TSS_RC_TIMED_OUT;
                break;
            }
        }
        face_tss_envelope_init(env);
        rc = face_tss_transport_receive(tr, remaining, env);
        if (rc != FACE_TSS_RC_NO_ERROR) {
            face_tss_envelope_fini(env);
            break;
        }
        /* Reliability gap bookkeeping observes every envelope that
         * arrives on the wire, before policy filtering: a stale or
         * priority-dropped message was not lost in transport, so it
         * must not surface as a phantom gap on a later message. The
         * reported gap is always the delivered message's own. */
        lock_take(tss);
        seq_gap = rx_gap_locked(tss, c, connection_id, env);
        lock_drop(tss);
        if (prio_threshold > 0 && env->priority < prio_threshold) {
            face_tss_envelope_fini(env);
            lock_take(tss);
            tss->stats.priority_dropped++;
            lock_drop(tss);
            if (timeout_ns == 0) {
                /* Poll: one attempt only. */
                rc = FACE_TSS_RC_TIMED_OUT;
                break;
            }
            continue;
        }
        break;
    }

    lock_take(tss);
    if (rc == FACE_TSS_RC_TIMED_OUT) {
        tss->stats.receive_timeouts++;
    } else if (rc == FACE_TSS_RC_NO_ERROR) {
        if (env->payload_len < min_message_size) {
            rc = FACE_TSS_RC_DATA_BUFFER_TOO_SMALL;
        } else {
            /* Staleness enforcement: discard messages older than the
             * connection's QoS staleness threshold and report
             * MESSAGE_STALE. No policy -> NO_ACTION -> delivered
             * normally. The QoS manager is only touched under the TSS
             * lock, and destroy() cannot free it while this operation
             * holds a TSS reference. */
            int64_t age = now_ns() - (int64_t)env->timestamp_ns;
            FACE_TSS_RETURN_CODE qrc;
            if (age < 0)
                age = 0;
            qrc = face_tss_qos_check_staleness(tss->qos, connection_id,
                                              age);
            if (qrc == FACE_TSS_RC_MESSAGE_STALE) {
                tss->stats.stale_dropped++;
                rc = FACE_TSS_RC_MESSAGE_STALE;
            } else {
                /* seq_gap was already recorded per wire-observed
                 * envelope in the loop above. */
                tss->stats.received++;
            }
        }
    }
    if (rc != FACE_TSS_RC_NO_ERROR)
        face_tss_envelope_fini(env);
    *seq_gap_out = seq_gap;
    teardown = conn_release_locked(tss, c);
    tss_exit_io_locked(tss);
    lock_drop(tss);
    if (teardown)
        teardown_conn(c);
    return rc;
}

FACE_TSS_RETURN_CODE face_tss_receive_message(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns, size_t min_message_size,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    FACE_TSS_MESSAGE *msg_out,
    FACE_TSS_QOS_EVENT *qos_out)
{
    FACE_TSS_RETURN_CODE rc;
    FACE_TSS_ENVELOPE env;
    int64_t seq_gap = -1;
    if (!msg_out)
        return FACE_TSS_RC_INVALID_PARAM;
    rc = receive_envelope(tss, connection_id, timeout_ns, min_message_size,
                          transaction_id, &env, &seq_gap);
    if (rc != FACE_TSS_RC_NO_ERROR)
        return rc;
    envelope_to_message(&env, msg_out);
    *transaction_id = env.transaction_id;
    qos_fill(qos_out, &env, seq_gap);
    face_tss_envelope_fini(&env);
    if (msg_out->payload_len > 0 && !msg_out->payload)
        return FACE_TSS_RC_NOT_AVAILABLE;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_receive_message_into(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns, size_t min_message_size,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    uint8_t *buffer, size_t buffer_capacity, size_t *payload_len_out,
    FACE_TSS_MESSAGE_GUID_TYPE *message_guid_out,
    FACE_TSS_HEADER *header_out,
    FACE_TSS_QOS_EVENT *qos_out)
{
    FACE_TSS_RETURN_CODE rc;
    FACE_TSS_ENVELOPE env;
    int64_t seq_gap = -1;
    if (!payload_len_out || (buffer_capacity > 0 && !buffer))
        return FACE_TSS_RC_INVALID_PARAM;
    rc = receive_envelope(tss, connection_id, timeout_ns, min_message_size,
                          transaction_id, &env, &seq_gap);
    if (rc != FACE_TSS_RC_NO_ERROR)
        return rc;
    if (env.payload_len > buffer_capacity) {
        *payload_len_out = env.payload_len;
        face_tss_envelope_fini(&env);
        return FACE_TSS_RC_DATA_BUFFER_TOO_SMALL;
    }
    if (env.payload_len > 0)
        memcpy(buffer, env.payload, env.payload_len);
    *payload_len_out = env.payload_len;
    *transaction_id = env.transaction_id;
    if (message_guid_out)
        *message_guid_out = env.message_guid;
    if (header_out) {
        header_out->instance_uid = env.instance_uid;
        header_out->source_uid = env.source_id;
        header_out->timestamp = env.timestamp_ns;
    }
    qos_fill(qos_out, &env, seq_gap);
    face_tss_envelope_fini(&env);
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
    int64_t seq_gap = -1; /* -1 = reliability monitoring inactive */
    lock_take(tss);
    {
        FACE_TSS_CONN *c = find_open(tss, id);
        if (c && c->cb) {
            /* Reliability gap bookkeeping observes every envelope that
             * arrives on the wire, before policy filtering: a stale or
             * priority-dropped message was not lost in transport, so it
             * must not surface as a phantom gap on a later message. */
            seq_gap = rx_gap_locked(tss, c, id, env);
            /* Stale messages are dropped, not delivered to the
             * callback; there is no return-code channel here, so the
             * drop is recorded in stats. */
            int64_t age = now_ns() - (int64_t)env->timestamp_ns;
            FACE_TSS_RETURN_CODE qrc;
            if (age < 0)
                age = 0;
            qrc = face_tss_qos_check_staleness(tss->qos, id, age);
            if (qrc == FACE_TSS_RC_MESSAGE_STALE) {
                tss->stats.stale_dropped++;
            } else {
                /* Priority enforcement: below-threshold messages are
                 * dropped, not delivered; the drop is recorded in
                 * stats. */
                int64_t pt = 0, pv = 0;
                if (face_tss_qos_get_policy(tss->qos, id,
                                            FACE_TSS_QOS_PRIORITY,
                                            &pv) == FACE_TSS_RC_NO_ERROR)
                    pt = pv;
                if (pt > 0 && env->priority < pt) {
                    tss->stats.priority_dropped++;
                } else {
                    cb = c->cb;
                    cb_user = c->cb_user;
                }
            }
        }
        tss->stats.received++;
    }
    lock_drop(tss);
    if (!cb)
        return;
    /* NOTE: ctx is owned by the connection; freed on unregister/destroy. */
    envelope_to_message(env, &msg);
    qos_fill(&qos, env, seq_gap);
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
    if (tss->destroying) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
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
    FACE_TSS_TRANSPORT *tr;
    void *ctx;
    void *user;
    void (*user_fini)(void *);
    int teardown;
    if (!tss)
        return FACE_TSS_RC_INVALID_PARAM;
    lock_take(tss);
    if (tss->destroying) {
        lock_drop(tss);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
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
    /* Detach first: new dispatches observe cb == NULL and drop. Hold a
     * connection reference and a TSS reference across the join below so
     * a concurrent destroy_connection/destroy cannot free the transport
     * or the instance out from under it. */
    c->cb = NULL;
    ctx = c->cb_ctx;
    c->cb_ctx = NULL;
    user = c->cb_user;
    c->cb_user = NULL;
    user_fini = c->cb_user_fini;
    c->cb_user_fini = NULL;
    tr = c->transport;
    conn_ref(c);
    tss->refcount++;
    lock_drop(tss);
    /* Join the dispatch thread with no TSS lock held: a dispatch in
     * progress may be blocked acquiring it, so joining under it would
     * deadlock. After the join, no dispatch is in flight, so the
     * context and user data are safe to release. */
    face_tss_transport_callback_stop(tr);
    free(ctx);
    if (user_fini)
        user_fini(user);
    lock_take(tss);
    teardown = conn_release_locked(tss, c);
    tss_exit_io_locked(tss);
    lock_drop(tss);
    if (teardown)
        teardown_conn(c);
    return FACE_TSS_RC_NO_ERROR;
}
