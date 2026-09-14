/* Live data-movement tests over real nng sockets (loopback).
 *
 * - pubsub fan-out: publisher TSS -> subscriber TSS, payload + header
 *   intact, sequence numbers increasing, timed-out poll -> TIMED_OUT.
 * - bus mesh: listen anchor + dialer exchange.
 * - buffer-too-small: oversize send rejected before touching the wire.
 * - callback: Register_Callback delivers without polling.
 * - topic isolation at the transport level (BBB frames never arrive on an
 *   AAA subscription).
 */
#include "test.h"

#include <string.h>
#if defined(_WIN32)
#include <windows.h>
static void msleep(long ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void msleep(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

#include "face_tss/config.h"
#include "face_tss/envelope.h"
#include "face_tss/transport.h"
#include "face_tss/tss.h"

static int next_port = 49201;

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
    if (face_tss_config_add(cfg, &c) != FACE_TSS_RC_NO_ERROR) {
        printf("FAIL\n    setup: config_add failed\n");
        face_tss_test_failures++;
    }
}

static void t_pubsub_round_trip(void)
{
    char a[64];
    FACE_TSS_CONFIG pc, sc;
    FACE_TSS *pub, *sub;
    FACE_TSS_CONNECTION_ID_TYPE pid, sid;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    FACE_TSS_MESSAGE m;
    static const uint8_t hello[] = { 'h', 'e', 'l', 'l', 'o', 0, 255 };
    TEST_BEGIN("pubsub_round_trip");
    addr(a);
    mk_cfg(&pc, "POSITION", a, FACE_TSS_BI_DIRECTIONAL,
           FACE_TSS_TRANSPORT_PUBSUB, FACE_TSS_ROLE_PUBLISHER);
    mk_cfg(&sc, "POSITION", a, FACE_TSS_BI_DIRECTIONAL,
           FACE_TSS_TRANSPORT_PUBSUB, FACE_TSS_ROLE_SUBSCRIBER);
    pub = face_tss_create("pub");
    sub = face_tss_create("sub");
    CHECK_RC(face_tss_initialize(pub, &pc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(sub, &sc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(pub, "position", &pid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(sub, "POSITION", &sid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    msleep(400);
    CHECK_RC(face_tss_send_message(pub, pid, hello, sizeof(hello), 11),
             FACE_TSS_RC_NO_ERROR);
    memset(&m, 0, sizeof(m));
    CHECK_RC(face_tss_receive_message(sub, sid, 5000000000LL, 0, &m),
             FACE_TSS_RC_NO_ERROR);
    CHECK(m.payload_len == sizeof(hello));
    CHECK(memcmp(m.payload, hello, sizeof(hello)) == 0);
    CHECK(m.header.transaction_id == 11);
    CHECK(strcmp(m.header.connection_name, "POSITION") == 0);
    CHECK(m.header.sequence_number == 1);
    CHECK(m.header.timestamp_ns > 0);
    CHECK(m.header.source_id == face_tss_source_id(pub));
    face_tss_message_fini(&m);
    CHECK_RC(face_tss_send_message(pub, pid, (const uint8_t *)"second", 6, 12),
             FACE_TSS_RC_NO_ERROR);
    memset(&m, 0, sizeof(m));
    CHECK_RC(face_tss_receive_message(sub, sid, 5000000000LL, 0, &m),
             FACE_TSS_RC_NO_ERROR);
    CHECK(m.header.sequence_number == 2);
    face_tss_message_fini(&m);
    face_tss_destroy(pub);
    face_tss_destroy(sub);
    face_tss_config_fini(&pc);
    face_tss_config_fini(&sc);
    TEST_END();
}

static void t_timeout(void)
{
    char a[64];
    FACE_TSS_CONFIG pc, sc;
    FACE_TSS *pub, *sub;
    FACE_TSS_CONNECTION_ID_TYPE pid, sid;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    FACE_TSS_MESSAGE m;
    bool has = true;
    TEST_BEGIN("receive_timeout");
    addr(a);
    mk_cfg(&pc, "POSITION", a, FACE_TSS_BI_DIRECTIONAL,
           FACE_TSS_TRANSPORT_PUBSUB, FACE_TSS_ROLE_PUBLISHER);
    mk_cfg(&sc, "POSITION", a, FACE_TSS_BI_DIRECTIONAL,
           FACE_TSS_TRANSPORT_PUBSUB, FACE_TSS_ROLE_SUBSCRIBER);
    pub = face_tss_create("pub");
    sub = face_tss_create("sub");
    CHECK_RC(face_tss_initialize(pub, &pc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(sub, &sc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(pub, "position", &pid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(sub, "POSITION", &sid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    msleep(300);
    memset(&m, 0, sizeof(m));
    CHECK(face_tss_receive_message(sub, sid, 200 * 1000000LL, 0, &m) ==
          FACE_TSS_RC_TIMED_OUT);
    CHECK_RC(face_tss_try_receive(sub, sid, &m, &has), FACE_TSS_RC_NO_ERROR);
    CHECK(!has);
    face_tss_destroy(pub);
    face_tss_destroy(sub);
    face_tss_config_fini(&pc);
    face_tss_config_fini(&sc);
    TEST_END();
}

static void t_topic_isolation(void)
{
    char a[64];
    nng_socket raw_pub, raw_sub;
    FACE_TSS_ENVELOPE env;
    uint8_t *body = NULL;
    size_t body_len = 0;
    nng_msg *msg = NULL;
    char got[256];
    size_t got_len = sizeof(got);
    TEST_BEGIN("topic_isolation");
    addr(a);
    CHECK(nng_pub0_open(&raw_pub) == 0);
    CHECK(nng_sub0_open(&raw_sub) == 0);
    CHECK(nng_listen(raw_pub, a, NULL, 0) == 0);
    CHECK(nng_dial(raw_sub, a, NULL, 0) == 0);
    CHECK(nng_socket_set(raw_sub, "sub:subscribe", "AAA", 3) == 0
          || nng_sub0_socket_subscribe(raw_sub, "AAA", 3) == 0);
    /* NOTE: subscribe to "AAA\0" (with NUL) - plain "AAA" would also match
     * "AAA2..." while "AAA\0" cannot. */
    nng_socket_set(raw_sub, "sub:unsubscribe", "AAA", 3);
    CHECK(nng_sub0_socket_subscribe(raw_sub, "AAA\0", 4) == 0);
    msleep(400);

    face_tss_envelope_init(&env);
    strcpy(env.connection_name, "BBB");
    env.payload = (uint8_t *)"for-bbb";
    env.payload_len = 7;
    CHECK_RC(face_tss_envelope_encode(&env, &body, &body_len),
             FACE_TSS_RC_NO_ERROR);
    CHECK(nng_msg_alloc(&msg, 0) == 0);
    CHECK(nng_msg_append(msg, "BBB", 4) == 0);
    CHECK(nng_msg_append(msg, body, body_len) == 0);
    free(body);
    body = NULL;
    CHECK(nng_sendmsg(raw_pub, msg, 0) == 0);
    CHECK(nng_socket_set_ms(raw_sub, "recv-timeout", 400) == 0);
    CHECK(nng_recv(raw_sub, got, &got_len, 0) == NNG_ETIMEDOUT);

    face_tss_envelope_init(&env);
    strcpy(env.connection_name, "AAA");
    env.payload = (uint8_t *)"for-aaa";
    env.payload_len = 7;
    CHECK_RC(face_tss_envelope_encode(&env, &body, &body_len),
             FACE_TSS_RC_NO_ERROR);
    CHECK(nng_msg_alloc(&msg, 0) == 0);
    CHECK(nng_msg_append(msg, "AAA", 4) == 0);
    CHECK(nng_msg_append(msg, body, body_len) == 0);
    free(body);
    CHECK(nng_sendmsg(raw_pub, msg, 0) == 0);
    got_len = sizeof(got);
    CHECK(nng_socket_set_ms(raw_sub, "recv-timeout", 2000) == 0);
    CHECK(nng_recv(raw_sub, got, &got_len, 0) == 0);
    CHECK(got_len > 4 && memcmp(got, "AAA", 4) == 0);
    nng_close(raw_pub);
    nng_close(raw_sub);
    TEST_END();
}

static void t_bus_exchange(void)
{
    char a[64];
    FACE_TSS_CONFIG ac, bc;
    FACE_TSS *ta, *tb;
    FACE_TSS_CONNECTION_ID_TYPE aid, bid;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    FACE_TSS_MESSAGE m;
    TEST_BEGIN("bus_exchange");
    addr(a);
    mk_cfg(&ac, "CMD", a, FACE_TSS_BI_DIRECTIONAL, FACE_TSS_TRANSPORT_BUS,
           FACE_TSS_ROLE_BUS);
    mk_cfg(&bc, "CMD", a, FACE_TSS_BI_DIRECTIONAL, FACE_TSS_TRANSPORT_BUS,
           FACE_TSS_ROLE_BUS);
    ta = face_tss_create("a");
    tb = face_tss_create("b");
    CHECK_RC(face_tss_initialize(ta, &ac), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(tb, &bc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(ta, "cmd", &aid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(tb, "cmd", &bid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    msleep(600);
    CHECK_RC(face_tss_send_message(ta, aid, (const uint8_t *)"hello-from-a",
                                   12, 1),
             FACE_TSS_RC_NO_ERROR);
    memset(&m, 0, sizeof(m));
    CHECK_RC(face_tss_receive_message(tb, bid, 5000000000LL, 0, &m),
             FACE_TSS_RC_NO_ERROR);
    CHECK(m.payload_len == 12);
    CHECK(memcmp(m.payload, "hello-from-a", 12) == 0);
    CHECK(m.header.transaction_id == 1);
    face_tss_message_fini(&m);
    face_tss_destroy(ta);
    face_tss_destroy(tb);
    face_tss_config_fini(&ac);
    face_tss_config_fini(&bc);
    TEST_END();
}

static void t_oversize(void)
{
    char a[64];
    FACE_TSS_CONFIG cfg;
    FACE_TSS_CONNECTION_CONFIG c;
    FACE_TSS *t;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    TEST_BEGIN("oversize_rejected");
    addr(a);
    memset(&c, 0, sizeof(c));
    strcpy(c.name, "C");
    strcpy(c.address, a);
    c.direction = FACE_TSS_BI_DIRECTIONAL;
    c.transport = FACE_TSS_TRANSPORT_PUBSUB;
    c.role = FACE_TSS_ROLE_PUBLISHER;
    c.max_message_size = 8;
    c.queue_depth = 64;
    face_tss_config_init(&cfg, "t");
    CHECK_RC(face_tss_config_add(&cfg, &c), FACE_TSS_RC_NO_ERROR);
    t = face_tss_create("t");
    CHECK_RC(face_tss_initialize(t, &cfg), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(t, "c", &id, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK(mx == 8);
    CHECK(face_tss_send_message(t, id, (const uint8_t *)"012345678", 9, 0) ==
          FACE_TSS_RC_BUFFER_TOO_SMALL);
    CHECK_RC(face_tss_send_message(t, id, (const uint8_t *)"01234567", 8, 0),
             FACE_TSS_RC_NO_ERROR);
    face_tss_destroy(t);
    face_tss_config_fini(&cfg);
    TEST_END();
}

typedef struct { volatile int n; char last[64]; size_t last_len; } cb_state_t;

static void on_msg(const FACE_TSS_MESSAGE *m, void *user)
{
    cb_state_t *s = (cb_state_t *)user;
    size_t n = m->payload_len < sizeof(s->last) - 1 ? m->payload_len
                                                    : sizeof(s->last) - 1;
    memcpy(s->last, m->payload, n);
    s->last[n] = '\0';
    s->last_len = m->payload_len;
    s->n++;
}

static void t_callback(void)
{
    char a[64];
    FACE_TSS_CONFIG pc, sc;
    FACE_TSS *pub, *sub;
    FACE_TSS_CONNECTION_ID_TYPE pid, sid;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    cb_state_t st;
    int waited = 0;
    TEST_BEGIN("callback_delivery");
    memset(&st, 0, sizeof(st));
    addr(a);
    mk_cfg(&pc, "POSITION", a, FACE_TSS_BI_DIRECTIONAL,
           FACE_TSS_TRANSPORT_PUBSUB, FACE_TSS_ROLE_PUBLISHER);
    mk_cfg(&sc, "POSITION", a, FACE_TSS_BI_DIRECTIONAL,
           FACE_TSS_TRANSPORT_PUBSUB, FACE_TSS_ROLE_SUBSCRIBER);
    pub = face_tss_create("pub");
    sub = face_tss_create("sub");
    CHECK_RC(face_tss_initialize(pub, &pc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(sub, &sc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(pub, "position", &pid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(sub, "POSITION", &sid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_register_callback(sub, sid, on_msg, &st),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_register_callback(sub, sid, on_msg, &st),
             FACE_TSS_RC_NO_ACTION);
    msleep(300);
    CHECK_RC(face_tss_send_message(pub, pid, (const uint8_t *)"via-callback",
                                   12, 0),
             FACE_TSS_RC_NO_ERROR);
    while (!st.n && waited < 50) {
        msleep(100);
        waited++;
    }
    CHECK(st.n == 1);
    CHECK(strcmp(st.last, "via-callback") == 0);
    CHECK_RC(face_tss_unregister_callback(sub, sid), FACE_TSS_RC_NO_ERROR);
    face_tss_destroy(pub);
    face_tss_destroy(sub);
    face_tss_config_fini(&pc);
    face_tss_config_fini(&sc);
    TEST_END();
}

int main(void)
{
    printf("[live]\n");
    t_pubsub_round_trip();
    t_timeout();
    t_topic_isolation();
    t_bus_exchange();
    t_oversize();
    t_callback();
    return TEST_SUMMARY();
}
