/* Threading regression tests: the TSS lock is never held across blocking
 * transport I/O, callback unregister/destroy never join under the TSS
 * lock, self-unregister from a data callback is safe, concurrent
 * receives keep their own per-call timeouts, and TPM destroy wakes
 * blocked readers. */

#include "test.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static void msleep(long ms) { Sleep((DWORD)ms); }
static int64_t test_now_ns(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)(c.QuadPart * 1000000000LL / f.QuadPart);
}
typedef HANDLE thread_t;
static thread_t thread_start(
#if defined(_MSC_VER)
    unsigned(__stdcall *fn)(void *)
#else
    DWORD(WINAPI *fn)(void *)
#endif
    , void *arg)
{
    return CreateThread(NULL, 0, fn, arg, 0, NULL);
}
static void thread_join(thread_t t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
#include <pthread.h>
#include <time.h>
static void msleep(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
static int64_t test_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}
typedef pthread_t thread_t;
static thread_t thread_start(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0)
        abort();
    return t;
}
static void thread_join(thread_t t) { pthread_join(t, NULL); }
#endif

#include "face_tss/config.h"
#include "face_tss/tss.h"
#include "face_tss/tpm.h"

static int next_port = 49401;

static void addr(char out[64])
{
    snprintf(out, 64, "tcp://127.0.0.1:%d", next_port++);
}

static void mk_cfg(FACE_TSS_CONFIG *cfg, const char *name, const char *a,
                   FACE_TSS_DIRECTION dir, FACE_TSS_TRANSPORT_KIND tr,
                   FACE_TSS_ROLE role)
{
    FACE_TSS_CONNECTION_CONFIG c;
    memset(&c, 0, sizeof(c));
    strncpy(c.name, name, sizeof(c.name) - 1);
    strncpy(c.address, a, sizeof(c.address) - 1);
    c.direction = dir;
    c.transport = tr;
    c.role = role;
    c.max_message_size = 65536;
    c.queue_depth = 64;
    face_tss_config_init(cfg, "t");
    if (face_tss_config_add(cfg, &c) != FACE_TSS_RC_NO_ERROR)
        abort();
}

/* ------------------------------------------------------------------ */
/* blocked receive must not stall the instance                        */
/* ------------------------------------------------------------------ */

typedef struct {
    FACE_TSS *tss;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_RETURN_CODE rc;
} recv_arg_t;

#if defined(_WIN32)
static DWORD WINAPI blocked_recv_fn(void *arg)
#else
static void *blocked_recv_fn(void *arg)
#endif
{
    recv_arg_t *ra = (recv_arg_t *)arg;
    FACE_TSS_TRANSACTION_ID_TYPE txn = FACE_TSS_TRANSACTION_ID_UNSPECIFIED;
    FACE_TSS_MESSAGE msg;
    memset(&msg, 0, sizeof(msg));
    ra->rc = face_tss_receive_message(ra->tss, ra->id,
                                      FACE_TSS_TIMEOUT_INFINITE, 0, &txn,
                                      &msg, NULL);
    face_tss_message_fini(&msg);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void t_blocked_receive_does_not_stall(void)
{
    FACE_TSS *tss = face_tss_create("thr");
    FACE_TSS_CONFIG both;
    FACE_TSS_CONNECTION_ID_TYPE ida, idb;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    FACE_TSS_TRANSACTION_ID_TYPE txn = FACE_TSS_TRANSACTION_ID_UNSPECIFIED;
    FACE_TSS_STATS stats;
    char aa[64], ab[64];
    recv_arg_t ra;
    thread_t thr;
    int64_t t0, dt_ms;
    static const uint8_t ping[] = "ping";

    TEST_BEGIN("blocked_receive_does_not_stall");
    addr(aa);
    addr(ab);
    {
        FACE_TSS_CONNECTION_CONFIG c2;
        face_tss_config_init(&both, "t");
        memset(&c2, 0, sizeof(c2));
        strncpy(c2.name, "a", sizeof(c2.name) - 1);
        strncpy(c2.address, aa, sizeof(c2.address) - 1);
        c2.direction = FACE_TSS_BI_DIRECTIONAL;
        c2.transport = FACE_TSS_TRANSPORT_BUS;
        c2.max_message_size = 65536;
        c2.queue_depth = 64;
        CHECK_RC(face_tss_config_add(&both, &c2), FACE_TSS_RC_NO_ERROR);
        memset(&c2, 0, sizeof(c2));
        strncpy(c2.name, "b", sizeof(c2.name) - 1);
        strncpy(c2.address, ab, sizeof(c2.address) - 1);
        c2.direction = FACE_TSS_BI_DIRECTIONAL;
        c2.transport = FACE_TSS_TRANSPORT_BUS;
        c2.max_message_size = 65536;
        c2.queue_depth = 64;
        CHECK_RC(face_tss_config_add(&both, &c2), FACE_TSS_RC_NO_ERROR);
        CHECK_RC(face_tss_initialize(tss, &both), FACE_TSS_RC_NO_ERROR);
    }
    CHECK_RC(face_tss_create_connection(tss, "a", &ida, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(tss, "b", &idb, &mx, 0),
             FACE_TSS_RC_NO_ERROR);

    /* Park a thread in an infinite receive on connection a. */
    ra.tss = tss;
    ra.id = ida;
    ra.rc = FACE_TSS_RC_NO_ERROR;
    thr = thread_start(blocked_recv_fn, &ra);
    msleep(200);

    /* The rest of the instance must stay responsive: stats and a send on
     * an unrelated connection complete promptly (not after the receive
     * ends). */
    t0 = test_now_ns();
    CHECK_RC(face_tss_stats(tss, &stats), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_send_message(tss, idb, 1000 * 1000000LL, &txn, ping,
                                   sizeof(ping)),
             FACE_TSS_RC_NO_ERROR);
    dt_ms = (test_now_ns() - t0) / 1000000LL;
    CHECK(dt_ms < 2000);

    /* Destroying the connection interrupts the blocked receive. */
    CHECK_RC(face_tss_destroy_connection(tss, ida), FACE_TSS_RC_NO_ERROR);
    thread_join(thr);
    CHECK(ra.rc != FACE_TSS_RC_NO_ERROR); /* woken with an error, not data */
    CHECK(ra.rc != FACE_TSS_RC_TIMED_OUT);

    face_tss_destroy(tss);
    face_tss_config_fini(&both);
    TEST_END();
}

/* ------------------------------------------------------------------ */
/* destroy interrupts a blocked infinite receive                      */
/* ------------------------------------------------------------------ */

static void t_destroy_interrupts_blocked_receive(void)
{
    FACE_TSS *tss = face_tss_create("thr2");
    FACE_TSS_CONFIG cfg;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    char a[64];
    recv_arg_t ra;
    thread_t thr;

    TEST_BEGIN("destroy_interrupts_blocked_receive");
    addr(a);
    mk_cfg(&cfg, "a", a, FACE_TSS_BI_DIRECTIONAL, FACE_TSS_TRANSPORT_BUS,
           FACE_TSS_ROLE_BUS);
    CHECK_RC(face_tss_initialize(tss, &cfg), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(tss, "a", &id, &mx, 0),
             FACE_TSS_RC_NO_ERROR);

    ra.tss = tss;
    ra.id = id;
    ra.rc = FACE_TSS_RC_NO_ERROR;
    thr = thread_start(blocked_recv_fn, &ra);
    msleep(200);

    /* Whole-instance destroy must interrupt the blocked receive; the
     * blocked thread returns without touching the freed instance. */
    face_tss_destroy(tss);
    thread_join(thr);
    CHECK(ra.rc != FACE_TSS_RC_NO_ERROR);
    face_tss_config_fini(&cfg);
    TEST_END();
}

/* ------------------------------------------------------------------ */
/* unregister while callbacks are firing; self-unregister             */
/* ------------------------------------------------------------------ */

typedef struct {
    FACE_TSS *tss;
    FACE_TSS_CONNECTION_ID_TYPE id;
    int count;
    int self_unregistered;
    FACE_TSS_RETURN_CODE self_rc;
#if defined(_WIN32)
    CRITICAL_SECTION mtx;
#else
    pthread_mutex_t mtx;
#endif
} cb_state_t;

static void cb_state_init(cb_state_t *st)
{
#if defined(_WIN32)
    InitializeCriticalSection(&st->mtx);
#else
    pthread_mutex_init(&st->mtx, NULL);
#endif
}

static void cb_state_fini(cb_state_t *st)
{
#if defined(_WIN32)
    DeleteCriticalSection(&st->mtx);
#else
    pthread_mutex_destroy(&st->mtx);
#endif
}

static void cb_state_lock(cb_state_t *st)
{
#if defined(_WIN32)
    EnterCriticalSection(&st->mtx);
#else
    pthread_mutex_lock(&st->mtx);
#endif
}

static void cb_state_unlock(cb_state_t *st)
{
#if defined(_WIN32)
    LeaveCriticalSection(&st->mtx);
#else
    pthread_mutex_unlock(&st->mtx);
#endif
}

static void counting_cb(FACE_TSS_CONNECTION_ID_TYPE connection_id,
                        FACE_TSS_TRANSACTION_ID_TYPE transaction_id,
                        FACE_TSS_MESSAGE_GUID_TYPE message_guid,
                        const uint8_t *payload, size_t payload_len,
                        const FACE_TSS_HEADER *header,
                        const FACE_TSS_QOS_EVENT *qos, void *user,
                        FACE_TSS_RETURN_CODE *return_code)
{
    cb_state_t *st = (cb_state_t *)user;
    (void)connection_id;
    (void)transaction_id;
    (void)message_guid;
    (void)payload;
    (void)payload_len;
    (void)header;
    (void)qos;
    cb_state_lock(st);
    st->count++;
    if (st->tss) {
        FACE_TSS *tss = st->tss;
        FACE_TSS_CONNECTION_ID_TYPE id = st->id;
        st->tss = NULL; /* only once */
        cb_state_unlock(st);
        /* Self-unregister from inside the data callback: must not
         * self-join or deadlock. Done without the state mutex held. */
        {
            FACE_TSS_RETURN_CODE rc = face_tss_unregister_callback(tss, id);
            cb_state_lock(st);
            st->self_rc = rc;
            st->self_unregistered = 1;
        }
    }
    cb_state_unlock(st);
    *return_code = FACE_TSS_RC_NO_ERROR;
}

static void plain_counting_cb(FACE_TSS_CONNECTION_ID_TYPE connection_id,
                              FACE_TSS_TRANSACTION_ID_TYPE transaction_id,
                              FACE_TSS_MESSAGE_GUID_TYPE message_guid,
                              const uint8_t *payload, size_t payload_len,
                              const FACE_TSS_HEADER *header,
                              const FACE_TSS_QOS_EVENT *qos, void *user,
                              FACE_TSS_RETURN_CODE *return_code)
{
    cb_state_t *st = (cb_state_t *)user;
    (void)connection_id;
    (void)transaction_id;
    (void)message_guid;
    (void)payload;
    (void)payload_len;
    (void)header;
    (void)qos;
    cb_state_lock(st);
    st->count++;
    cb_state_unlock(st);
    *return_code = FACE_TSS_RC_NO_ERROR;
}

typedef struct {
    FACE_TSS *tss;
    FACE_TSS_CONNECTION_ID_TYPE id;
    int n;
} pub_arg_t;

#if defined(_WIN32)
static DWORD WINAPI publisher_fn(void *arg)
#else
static void *publisher_fn(void *arg)
#endif
{
    pub_arg_t *pa = (pub_arg_t *)arg;
    int i;
    static const uint8_t ping[] = "ping";
    for (i = 0; i < pa->n; i++) {
        FACE_TSS_TRANSACTION_ID_TYPE txn =
            FACE_TSS_TRANSACTION_ID_UNSPECIFIED;
        face_tss_send_message(pa->tss, pa->id, 2000 * 1000000LL, &txn, ping,
                              sizeof(ping));
        msleep(5);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void t_unregister_while_firing(void)
{
    FACE_TSS *pub = face_tss_create("pub");
    FACE_TSS *sub = face_tss_create("sub");
    FACE_TSS_CONFIG pc, sc;
    FACE_TSS_CONNECTION_ID_TYPE pid, sid;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    char a[64];
    cb_state_t st;
    pub_arg_t pa;
    thread_t pthr;
    int c1, c2;

    TEST_BEGIN("unregister_while_callbacks_fire");
    addr(a);
    mk_cfg(&pc, "ch", a, FACE_TSS_SOURCE, FACE_TSS_TRANSPORT_PUBSUB,
           FACE_TSS_ROLE_PUBLISHER);
    mk_cfg(&sc, "ch", a, FACE_TSS_DESTINATION, FACE_TSS_TRANSPORT_PUBSUB,
           FACE_TSS_ROLE_SUBSCRIBER);
    CHECK_RC(face_tss_initialize(pub, &pc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(sub, &sc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(pub, "ch", &pid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(sub, "ch", &sid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);

    memset(&st, 0, sizeof(st));
    cb_state_init(&st);
    CHECK_RC(face_tss_register_callback(sub, sid, plain_counting_cb, &st),
             FACE_TSS_RC_NO_ERROR);

    /* Stream messages from a background thread; unregister mid-stream
     * from this thread. Neither may deadlock. */
    pa.tss = pub;
    pa.id = pid;
    pa.n = 60;
    pthr = thread_start(publisher_fn, &pa);
    for (;;) {
        int n;
        cb_state_lock(&st);
        n = st.count;
        cb_state_unlock(&st);
        if (n >= 3)
            break;
        msleep(10);
    }
    CHECK_RC(face_tss_unregister_callback(sub, sid), FACE_TSS_RC_NO_ERROR);
    thread_join(pthr);
    /* After unregister, no further dispatches happen. */
    msleep(300);
    cb_state_lock(&st);
    c1 = st.count;
    cb_state_unlock(&st);
    msleep(300);
    cb_state_lock(&st);
    c2 = st.count;
    cb_state_unlock(&st);
    CHECK(c1 == c2);

    CHECK_RC(face_tss_unregister_callback(sub, sid), FACE_TSS_RC_NO_ACTION);
    face_tss_destroy(sub);
    face_tss_destroy(pub);
    face_tss_config_fini(&pc);
    face_tss_config_fini(&sc);
    cb_state_fini(&st);
    TEST_END();
}

static void t_callback_self_unregister(void)
{
    FACE_TSS *pub = face_tss_create("pub2");
    FACE_TSS *sub = face_tss_create("sub2");
    FACE_TSS_CONFIG pc, sc;
    FACE_TSS_CONNECTION_ID_TYPE pid, sid;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    char a[64];
    cb_state_t st;
    pub_arg_t pa;
    thread_t pthr;
    int c1, c2;

    TEST_BEGIN("callback_self_unregister");
    addr(a);
    mk_cfg(&pc, "ch", a, FACE_TSS_SOURCE, FACE_TSS_TRANSPORT_PUBSUB,
           FACE_TSS_ROLE_PUBLISHER);
    mk_cfg(&sc, "ch", a, FACE_TSS_DESTINATION, FACE_TSS_TRANSPORT_PUBSUB,
           FACE_TSS_ROLE_SUBSCRIBER);
    CHECK_RC(face_tss_initialize(pub, &pc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(sub, &sc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(pub, "ch", &pid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(sub, "ch", &sid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);

    memset(&st, 0, sizeof(st));
    cb_state_init(&st);
    st.tss = sub; /* counting_cb unregisters itself on first delivery */
    st.id = sid;
    CHECK_RC(face_tss_register_callback(sub, sid, counting_cb, &st),
             FACE_TSS_RC_NO_ERROR);

    pa.tss = pub;
    pa.id = pid;
    pa.n = 10;
    pthr = thread_start(publisher_fn, &pa);
    thread_join(pthr);
    /* The self-unregister must have run without deadlocking. */
    cb_state_lock(&st);
    {
        int unreg = st.self_unregistered;
        FACE_TSS_RETURN_CODE self_rc = st.self_rc;
        int n = st.count;
        cb_state_unlock(&st);
        CHECK(unreg);
        CHECK_RC(self_rc, FACE_TSS_RC_NO_ERROR);
        CHECK(n >= 1);
    }
    /* Exactly one delivery: after self-unregister nothing more arrives. */
    msleep(400);
    cb_state_lock(&st);
    c1 = st.count;
    cb_state_unlock(&st);
    msleep(300);
    cb_state_lock(&st);
    c2 = st.count;
    cb_state_unlock(&st);
    CHECK(c1 == c2);
    CHECK(c1 == 1);

    face_tss_destroy(sub);
    face_tss_destroy(pub);
    face_tss_config_fini(&pc);
    face_tss_config_fini(&sc);
    cb_state_fini(&st);
    TEST_END();
}

/* ------------------------------------------------------------------ */
/* concurrent receives keep their own per-call timeouts               */
/* ------------------------------------------------------------------ */

typedef struct {
    FACE_TSS *tss;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_RETURN_CODE rc;
    int got_msg;
} long_recv_arg_t;

#if defined(_WIN32)
static DWORD WINAPI long_recv_fn(void *arg)
#else
static void *long_recv_fn(void *arg)
#endif
{
    long_recv_arg_t *la = (long_recv_arg_t *)arg;
    FACE_TSS_TRANSACTION_ID_TYPE txn = FACE_TSS_TRANSACTION_ID_UNSPECIFIED;
    FACE_TSS_MESSAGE msg;
    memset(&msg, 0, sizeof(msg));
    la->rc = face_tss_receive_message(la->tss, la->id, 10 * 1000000000LL, 0,
                                      &txn, &msg, NULL);
    la->got_msg = (la->rc == FACE_TSS_RC_NO_ERROR);
    face_tss_message_fini(&msg);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

typedef struct {
    FACE_TSS *tss;
    FACE_TSS_CONNECTION_ID_TYPE id;
    long delay_ms;
} delayed_send_arg_t;

#if defined(_WIN32)
static DWORD WINAPI delayed_send_fn(void *arg)
#else
static void *delayed_send_fn(void *arg)
#endif
{
    delayed_send_arg_t *da = (delayed_send_arg_t *)arg;
    FACE_TSS_TRANSACTION_ID_TYPE txn = FACE_TSS_TRANSACTION_ID_UNSPECIFIED;
    static const uint8_t ping[] = "ping";
    msleep(da->delay_ms);
    face_tss_send_message(da->tss, da->id, 2000 * 1000000LL, &txn, ping,
                          sizeof(ping));
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void t_concurrent_receive_timeouts(void)
{
    FACE_TSS *pub = face_tss_create("pub3");
    FACE_TSS *sub = face_tss_create("sub3");
    FACE_TSS_CONFIG pc, sc;
    FACE_TSS_CONNECTION_ID_TYPE pid, sid;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    char a[64];
    long_recv_arg_t la;
    delayed_send_arg_t da;
    thread_t thr, sthr;
    int64_t t0, dt_ms;
    int i;

    TEST_BEGIN("concurrent_receive_timeouts");
    addr(a);
    mk_cfg(&pc, "ch", a, FACE_TSS_SOURCE, FACE_TSS_TRANSPORT_PUBSUB,
           FACE_TSS_ROLE_PUBLISHER);
    mk_cfg(&sc, "ch", a, FACE_TSS_DESTINATION, FACE_TSS_TRANSPORT_PUBSUB,
           FACE_TSS_ROLE_SUBSCRIBER);
    CHECK_RC(face_tss_initialize(pub, &pc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(sub, &sc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(pub, "ch", &pid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(sub, "ch", &sid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);

    /* A 10 s receive parks while this thread issues many timeout-0
     * polls on the same socket; a message arrives mid-receive. The
     * per-socket timeout option is serialized per call, so the long
     * receive must still observe its own 10 s timeout and get the
     * message (not an immediate TIMED_OUT), and the polls must keep
     * their 0 timeout (not block for seconds on a clobbered 10 s). */
    la.tss = sub;
    la.id = sid;
    la.rc = FACE_TSS_RC_NO_ERROR;
    la.got_msg = 0;
    thr = thread_start(long_recv_fn, &la);
    msleep(300); /* let the long receive park first */
    da.tss = pub;
    da.id = pid;
    da.delay_ms = 500;
    sthr = thread_start(delayed_send_fn, &da);
    t0 = test_now_ns();
    for (i = 0; i < 50; i++) {
        FACE_TSS_TRANSACTION_ID_TYPE t2 =
            FACE_TSS_TRANSACTION_ID_UNSPECIFIED;
        FACE_TSS_MESSAGE m2;
        FACE_TSS_RETURN_CODE rc;
        memset(&m2, 0, sizeof(m2));
        rc = face_tss_receive_message(sub, sid, 0, 0, &t2, &m2, NULL);
        face_tss_message_fini(&m2);
        CHECK_RC(rc, FACE_TSS_RC_TIMED_OUT);
    }
    dt_ms = (test_now_ns() - t0) / 1000000LL;
    thread_join(sthr);
    thread_join(thr);
    CHECK_RC(la.rc, FACE_TSS_RC_NO_ERROR);
    CHECK(la.got_msg);
    /* 50 polls, each with timeout 0, must not have inherited the 10 s
     * timeout: generous bound for (message arrival ~800 ms) + polls. */
    CHECK(dt_ms < 5000);

    face_tss_destroy(sub);
    face_tss_destroy(pub);
    face_tss_config_fini(&pc);
    face_tss_config_fini(&sc);
    TEST_END();
}

/* ------------------------------------------------------------------ */
/* TPM: destroy wakes a blocked reader                                */
/* ------------------------------------------------------------------ */

typedef struct {
    FACE_TSS_TPM *tpm;
    FACE_TSS_TPM_CHANNEL_ID_TYPE ch;
    FACE_TSS_RETURN_CODE rc;
} tpm_read_arg_t;

#if defined(_WIN32)
static DWORD WINAPI tpm_blocked_read_fn(void *arg)
#else
static void *tpm_blocked_read_fn(void *arg)
#endif
{
    tpm_read_arg_t *ra = (tpm_read_arg_t *)arg;
    FACE_TSS_TRANSACTION_ID_TYPE txn = 0;
    uint8_t buf[64];
    size_t len = sizeof(buf);
    ra->rc = face_tss_tpm_read_from_transport(
        ra->tpm, ra->ch, FACE_TSS_TIMEOUT_INFINITE, &txn, buf, &len);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void t_tpm_destroy_wakes_blocked_reader(void)
{
    FACE_TSS_TPM *tpm = face_tss_tpm_create("tpm-thr");
    FACE_TSS_TPM_CHANNEL_ID_TYPE ch = 0;
    tpm_read_arg_t ra;
    thread_t thr;

    TEST_BEGIN("tpm_destroy_wakes_blocked_reader");
    CHECK(tpm != NULL);
    CHECK_RC(face_tss_tpm_initialize(tpm, ""), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_tpm_open_channel(tpm, "ep1", NULL, 0, NULL, 0, &ch),
             FACE_TSS_RC_NO_ERROR);

    ra.tpm = tpm;
    ra.ch = ch;
    ra.rc = FACE_TSS_RC_NO_ERROR;
    thr = thread_start(tpm_blocked_read_fn, &ra);
    msleep(200);

    /* Destroy must wake the blocked reader; the reader returns
     * NOT_AVAILABLE without touching the freed TPM. */
    face_tss_tpm_destroy(tpm);
    thread_join(thr);
    CHECK_RC(ra.rc, FACE_TSS_RC_NOT_AVAILABLE);
    TEST_END();
}

int main(void)
{
    t_blocked_receive_does_not_stall();
    t_destroy_interrupts_blocked_receive();
    t_unregister_while_firing();
    t_callback_self_unregister();
    t_concurrent_receive_timeouts();
    t_tpm_destroy_wakes_blocked_reader();
    return TEST_SUMMARY();
}
