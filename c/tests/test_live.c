/* Live data-movement tests over real nng sockets (loopback).
 *
 * - pubsub fan-out: publisher TSS -> subscriber TSS, payload + FACE header
 *   intact, transaction-id inout honored, timed-out poll -> TIMED_OUT.
 * - bus mesh: listen anchor + dialer exchange.
 * - data-buffer-too-small: oversize send rejected before touching the wire.
 * - callback: Register_Callback delivers without polling.
 * - topic isolation at the transport level (BBB frames never arrive on an
 *   AAA subscription).
 * - conformance: header carries instance_uid/source_uid/timestamp;
 *   transaction IDs pass through; QoS events carry message_age_ns.
 */
#include "test.h"

#include <stdlib.h>
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
    FACE_TSS_TRANSACTION_ID_TYPE txn;
    FACE_TSS_QOS_EVENT qos;
    FACE_TSS_MESSAGE m;
    FACE_TSS_UID_TYPE first_iuid;
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
    txn = 11;
    CHECK_RC(face_tss_send_message(pub, pid, 5000000000LL, &txn, hello,
                                   sizeof(hello)),
             FACE_TSS_RC_NO_ERROR);
    CHECK(txn == 11); /* explicit txn passes through (inout) */
    memset(&m, 0, sizeof(m));
    txn = 0;
    CHECK_RC(face_tss_receive_message(sub, sid, 5000000000LL, 0, &txn, &m,
                                      &qos),
             FACE_TSS_RC_NO_ERROR);
    CHECK(m.payload_len == sizeof(hello));
    CHECK(memcmp(m.payload, hello, sizeof(hello)) == 0);
    CHECK(txn == 11); /* inout transaction_id written back */
    CHECK(m.message_guid == FACE_TSS_MESSAGE_GUID_UNSPECIFIED);
    /* FACE HEADER_TYPE: instance UID, source UID, timestamp. */
    CHECK(m.header.instance_uid != 0);
    CHECK(m.header.source_uid == face_tss_source_id(pub));
    CHECK(m.header.timestamp > 0);
    CHECK(qos.count == 1); /* message_age_ns element */
    CHECK(strcmp(qos.elements[0].keyname, "message_age_ns") == 0);
    first_iuid = m.header.instance_uid;
    face_tss_message_fini(&m);
    txn = FACE_TSS_TRANSACTION_ID_UNSPECIFIED;
    CHECK_RC(face_tss_send_message(pub, pid, 5000000000LL, &txn,
                                   (const uint8_t *)"second", 6),
             FACE_TSS_RC_NO_ERROR);
    CHECK(txn != FACE_TSS_TRANSACTION_ID_UNSPECIFIED); /* TSS assigned one */
    memset(&m, 0, sizeof(m));
    txn = 0;
    CHECK_RC(face_tss_receive_message(sub, sid, 5000000000LL, 0, &txn, &m,
                                      NULL),
             FACE_TSS_RC_NO_ERROR);
    CHECK(m.header.instance_uid != 0);
    CHECK(m.header.instance_uid != first_iuid); /* UIDs unique per message */
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
    FACE_TSS_TRANSACTION_ID_TYPE txn = 0;
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
    CHECK(face_tss_receive_message(sub, sid, 200 * 1000000LL, 0, &txn, &m,
                                   NULL) == FACE_TSS_RC_TIMED_OUT);
    CHECK_RC(face_tss_try_receive(sub, sid, &txn, &m, NULL, &has),
             FACE_TSS_RC_NO_ERROR);
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
    FACE_TSS_TRANSACTION_ID_TYPE txn;
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
    txn = 1;
    CHECK_RC(face_tss_send_message(ta, aid, 5000000000LL, &txn,
                                   (const uint8_t *)"hello-from-a", 12),
             FACE_TSS_RC_NO_ERROR);
    memset(&m, 0, sizeof(m));
    txn = 0;
    CHECK_RC(face_tss_receive_message(tb, bid, 5000000000LL, 0, &txn, &m,
                                      NULL),
             FACE_TSS_RC_NO_ERROR);
    CHECK(m.payload_len == 12);
    CHECK(memcmp(m.payload, "hello-from-a", 12) == 0);
    CHECK(txn == 1);
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
    FACE_TSS_TRANSACTION_ID_TYPE txn = 0;
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
    CHECK(face_tss_send_message(t, id, 0, &txn,
                                (const uint8_t *)"012345678", 9) ==
          FACE_TSS_RC_DATA_BUFFER_TOO_SMALL);
    CHECK_RC(face_tss_send_message(t, id, 0, &txn,
                                   (const uint8_t *)"01234567", 8),
             FACE_TSS_RC_NO_ERROR);
    face_tss_destroy(t);
    face_tss_config_fini(&cfg);
    TEST_END();
}

/* Caller-owned receive buffers: exact fit succeeds, undersized reports the
 * required size and discards, zero-length payloads work, QoS/header/guid
 * outputs are filled. */
static void t_receive_into(void)
{
    char a[64];
    FACE_TSS_CONFIG pc, sc;
    FACE_TSS *pub, *sub;
    FACE_TSS_CONNECTION_ID_TYPE pid, sid;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    FACE_TSS_TRANSACTION_ID_TYPE txn;
    FACE_TSS_QOS_EVENT qos;
    FACE_TSS_HEADER hdr;
    FACE_TSS_MESSAGE_GUID_TYPE guid;
    uint8_t buf[64];
    size_t got;
    static const uint8_t hello[] = { 'h', 'e', 'l', 'l', 'o' };
    TEST_BEGIN("receive_into");
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

    /* Undersized buffer: required size reported, message discarded. */
    txn = 21;
    CHECK_RC(face_tss_send_message(pub, pid, 5000000000LL, &txn, hello,
                                   sizeof(hello)),
             FACE_TSS_RC_NO_ERROR);
    txn = 0;
    got = 0;
    CHECK(face_tss_receive_message_into(sub, sid, 5000000000LL, 0, &txn,
                                        buf, 3, &got, &guid, &hdr, &qos) ==
          FACE_TSS_RC_DATA_BUFFER_TOO_SMALL);
    CHECK(got == sizeof(hello)); /* required size reported */

    /* Exact fit: payload copied, metadata filled. */
    txn = 22;
    CHECK_RC(face_tss_send_message(pub, pid, 5000000000LL, &txn, hello,
                                   sizeof(hello)),
             FACE_TSS_RC_NO_ERROR);
    txn = 0;
    got = 0;
    memset(&hdr, 0, sizeof(hdr));
    guid = 0xFFFFFFFFFFFFFFFFULL;
    CHECK_RC(face_tss_receive_message_into(sub, sid, 5000000000LL, 0, &txn,
                                           buf, sizeof(buf), &got, &guid,
                                           &hdr, &qos),
             FACE_TSS_RC_NO_ERROR);
    CHECK(got == sizeof(hello));
    CHECK(memcmp(buf, hello, sizeof(hello)) == 0);
    CHECK(txn == 22);
    CHECK(hdr.source_uid == face_tss_source_id(pub));
    CHECK(hdr.timestamp > 0);
    CHECK(guid == FACE_TSS_MESSAGE_GUID_UNSPECIFIED);
    CHECK(qos.count == 1);
    CHECK(strcmp(qos.elements[0].keyname, "message_age_ns") == 0);

    /* Zero-length payload: NULL buffer with capacity 0 works. */
    txn = 23;
    CHECK_RC(face_tss_send_message(pub, pid, 5000000000LL, &txn,
                                   NULL, 0),
             FACE_TSS_RC_NO_ERROR);
    txn = 0;
    got = 99;
    CHECK_RC(face_tss_receive_message_into(sub, sid, 5000000000LL, 0, &txn,
                                           NULL, 0, &got, NULL, NULL, NULL),
             FACE_TSS_RC_NO_ERROR);
    CHECK(got == 0);
    CHECK(txn == 23);

    face_tss_destroy(pub);
    face_tss_destroy(sub);
    face_tss_config_fini(&pc);
    face_tss_config_fini(&sc);
    TEST_END();
}

typedef struct { volatile int n; char last[64]; size_t last_len; } cb_state_t;

static void on_msg(FACE_TSS_CONNECTION_ID_TYPE id,
                   FACE_TSS_TRANSACTION_ID_TYPE txn,
                   FACE_TSS_MESSAGE_GUID_TYPE guid,
                   const uint8_t *payload, size_t payload_len,
                   const FACE_TSS_HEADER *header,
                   const FACE_TSS_QOS_EVENT *qos,
                   void *user,
                   FACE_TSS_RETURN_CODE *return_code)
{
    cb_state_t *s = (cb_state_t *)user;
    size_t n = payload_len < sizeof(s->last) - 1 ? payload_len
                                                 : sizeof(s->last) - 1;
    (void)id;
    (void)txn;
    (void)guid;
    (void)header;
    (void)qos;
    memcpy(s->last, payload, n);
    s->last[n] = '\0';
    s->last_len = payload_len;
    s->n++;
    *return_code = FACE_TSS_RC_NO_ERROR;
}

static void t_callback(void)
{
    char a[64];
    FACE_TSS_CONFIG pc, sc;
    FACE_TSS *pub, *sub;
    FACE_TSS_CONNECTION_ID_TYPE pid, sid;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    FACE_TSS_TRANSACTION_ID_TYPE txn = 0;
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
    CHECK_RC(face_tss_send_message(pub, pid, 5000000000LL, &txn,
                                   (const uint8_t *)"via-callback", 12),
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
    t_receive_into();
    t_callback();
    return TEST_SUMMARY();
}
