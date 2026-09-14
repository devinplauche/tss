/* Typed interface tests: register type support, typed send/receive over
 * real loopback sockets, typed callbacks, guid mismatch rejection. */
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

#include "face_tss/config.h"
#include "face_tss/tss.h"
#include "face_tss/typed.h"
#include "positionreport_typed.h"

static int next_port = 49301;

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

/* Build a pub/sub pair with PositionReport registered on both ends. */
static void make_pair(const char *a, FACE_TSS **pub, FACE_TSS **sub,
                      FACE_TSS_CONNECTION_ID_TYPE *pid,
                      FACE_TSS_CONNECTION_ID_TYPE *sid)
{
    FACE_TSS_CONFIG pc, sc;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    mk_cfg(&pc, "POSITION", a, FACE_TSS_BI_DIRECTIONAL,
           FACE_TSS_TRANSPORT_PUBSUB, FACE_TSS_ROLE_PUBLISHER);
    mk_cfg(&sc, "POSITION", a, FACE_TSS_BI_DIRECTIONAL,
           FACE_TSS_TRANSPORT_PUBSUB, FACE_TSS_ROLE_SUBSCRIBER);
    *pub = face_tss_create("pub");
    *sub = face_tss_create("sub");
    CHECK_RC(face_tss_initialize(*pub, &pc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(*sub, &sc), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_typed_register(*pub, &PositionReport_type_support),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_typed_register(*sub, &PositionReport_type_support),
             FACE_TSS_RC_NO_ERROR);
    /* duplicate registration is a no-op */
    CHECK_RC(face_tss_typed_register(*pub, &PositionReport_type_support),
             FACE_TSS_RC_NO_ACTION);
    CHECK(face_tss_typed_lookup(*pub, "FaceTSS.PositionReport") != NULL);
    CHECK(face_tss_typed_lookup(*pub, "Nope.Nope") == NULL);
    CHECK_RC(face_tss_create_connection(*pub, "position", pid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(*sub, "POSITION", sid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    face_tss_config_fini(&pc);
    face_tss_config_fini(&sc);
}

static void t_register_errors(void)
{
    FACE_TSS *t = face_tss_create("t");
    TEST_BEGIN("typed_register_errors");
    CHECK(face_tss_typed_register(NULL, &PositionReport_type_support) ==
          FACE_TSS_RC_INVALID_PARAM);
    CHECK(face_tss_typed_register(t, NULL) == FACE_TSS_RC_INVALID_PARAM);
    CHECK(face_tss_typed_lookup(t, "nope") == NULL);
    CHECK(face_tss_typed_send(t, 1, 0, NULL, "nope", NULL) ==
          FACE_TSS_RC_INVALID_PARAM);
    face_tss_destroy(t);
    TEST_END();
}

static void t_typed_round_trip(void)
{
    char a[64];
    FACE_TSS *pub, *sub;
    FACE_TSS_CONNECTION_ID_TYPE pid, sid;
    FACE_TSS_TRANSACTION_ID_TYPE txn;
    FACE_TSS_HEADER header;
    FACE_TSS_QOS_EVENT qos;
    PositionReport out, back;
    TEST_BEGIN("typed_round_trip");
    addr(a);
    make_pair(a, &pub, &sub, &pid, &sid);
    msleep(400);

    memset(&out, 0, sizeof(out));
    out.vehicle_id = "UAV-7";
    out.latitude_deg = 42.281234;
    out.longitude_deg = -83.743456;
    out.altitude_m = 250.5f;
    out.heading_deg = 90.0f;
    out.valid = true;

    txn = FACE_TSS_TRANSACTION_ID_UNSPECIFIED;
    CHECK_RC(face_tss_typed_send(pub, pid, 5000000000LL, &txn,
                                 "FaceTSS.PositionReport", &out),
             FACE_TSS_RC_NO_ERROR);
    CHECK(txn != FACE_TSS_TRANSACTION_ID_UNSPECIFIED);

    memset(&back, 0, sizeof(back));
    txn = 0;
    CHECK_RC(face_tss_typed_receive(sub, sid, 5000000000LL, 0, &txn,
                                    "FaceTSS.PositionReport", &back,
                                    &header, &qos),
             FACE_TSS_RC_NO_ERROR);
    CHECK(strcmp(back.vehicle_id, "UAV-7") == 0);
    CHECK(back.latitude_deg == 42.281234);
    CHECK(back.longitude_deg == -83.743456);
    CHECK(back.altitude_m == 250.5f);
    CHECK(back.heading_deg == 90.0f);
    CHECK(back.valid == true);
    CHECK(header.instance_uid != 0);
    CHECK(header.source_uid == face_tss_source_id(pub));
    CHECK(header.timestamp > 0);
    CHECK(qos.count == 0);
    PositionReport_fini(&back);

    /* The wire envelope carries the data-model GUID. */
    {
        FACE_TSS_MESSAGE raw;
        FACE_TSS_TRANSACTION_ID_TYPE t2 = 0;
        memset(&raw, 0, sizeof(raw));
        txn = 0;
        CHECK_RC(face_tss_typed_send(pub, pid, 5000000000LL, &txn,
                                     "FaceTSS.PositionReport", &out),
                 FACE_TSS_RC_NO_ERROR);
        CHECK_RC(face_tss_receive_message(sub, sid, 5000000000LL, 0, &t2,
                                          &raw, NULL),
                 FACE_TSS_RC_NO_ERROR);
        CHECK(raw.message_guid == POSITIONREPORT_MESSAGE_GUID);
        face_tss_message_fini(&raw);
    }
    face_tss_destroy(pub);
    face_tss_destroy(sub);
    TEST_END();
}

typedef struct { volatile int n; double last_lat; } tcb_state_t;

static void on_typed(FACE_TSS_CONNECTION_ID_TYPE id,
                     FACE_TSS_TRANSACTION_ID_TYPE txn,
                     const char *type_name, const void *typed_msg,
                     const FACE_TSS_HEADER *header,
                     const FACE_TSS_QOS_EVENT *qos,
                     void *user,
                     FACE_TSS_RETURN_CODE *return_code)
{
    const PositionReport *r = (const PositionReport *)typed_msg;
    tcb_state_t *s = (tcb_state_t *)user;
    (void)id;
    (void)txn;
    (void)header;
    (void)qos;
    if (strcmp(type_name, "FaceTSS.PositionReport") != 0) {
        *return_code = FACE_TSS_RC_INVALID_PARAM;
        return;
    }
    s->last_lat = r->latitude_deg;
    s->n++;
    *return_code = FACE_TSS_RC_NO_ERROR;
}

static void t_typed_callback(void)
{
    char a[64];
    FACE_TSS *pub, *sub;
    FACE_TSS_CONNECTION_ID_TYPE pid, sid;
    FACE_TSS_TRANSACTION_ID_TYPE txn;
    PositionReport out;
    tcb_state_t st;
    int waited = 0;
    TEST_BEGIN("typed_callback");
    memset(&st, 0, sizeof(st));
    addr(a);
    make_pair(a, &pub, &sub, &pid, &sid);
    CHECK_RC(face_tss_typed_register_callback(sub, sid,
                                              "FaceTSS.PositionReport",
                                              on_typed, &st),
             FACE_TSS_RC_NO_ERROR);
    msleep(300);
    memset(&out, 0, sizeof(out));
    out.vehicle_id = "UAV-9";
    out.latitude_deg = 1.5;
    out.valid = true;
    txn = 0;
    CHECK_RC(face_tss_typed_send(pub, pid, 5000000000LL, &txn,
                                 "FaceTSS.PositionReport", &out),
             FACE_TSS_RC_NO_ERROR);
    while (!st.n && waited < 50) {
        msleep(100);
        waited++;
    }
    CHECK(st.n == 1);
    CHECK(st.last_lat == 1.5);
    CHECK_RC(face_tss_unregister_callback(sub, sid), FACE_TSS_RC_NO_ERROR);
    face_tss_destroy(pub);
    face_tss_destroy(sub);
    TEST_END();
}

static void t_typed_guid_mismatch(void)
{
    char a[64];
    FACE_TSS *pub, *sub;
    FACE_TSS_CONNECTION_ID_TYPE pid, sid;
    FACE_TSS_TRANSACTION_ID_TYPE txn;
    FACE_TSS_HEADER header;
    PositionReport out, back;
    static const FACE_TSS_TYPE_SUPPORT other = {
        12345, "Other.Type", PositionReport_serialize,
        PositionReport_deserialize, PositionReport_fini,
        sizeof(PositionReport), 65536
    };
    TEST_BEGIN("typed_guid_mismatch");
    addr(a);
    make_pair(a, &pub, &sub, &pid, &sid);
    /* sub registers a *different* type under a different name */
    CHECK_RC(face_tss_typed_register(sub, &other), FACE_TSS_RC_NO_ERROR);
    msleep(400);
    memset(&out, 0, sizeof(out));
    out.vehicle_id = "X";
    txn = 0;
    CHECK_RC(face_tss_typed_send(pub, pid, 5000000000LL, &txn,
                                 "FaceTSS.PositionReport", &out),
             FACE_TSS_RC_NO_ERROR);
    memset(&back, 0, sizeof(back));
    txn = 0;
    /* receiving as the wrong type is rejected */
    CHECK(face_tss_typed_receive(sub, sid, 5000000000LL, 0, &txn,
                                 "Other.Type", &back, &header, NULL) ==
          FACE_TSS_RC_INVALID_PARAM);
    face_tss_destroy(pub);
    face_tss_destroy(sub);
    TEST_END();
}

int main(void)
{
    printf("[typed]\n");
    t_register_errors();
    t_typed_round_trip();
    t_typed_callback();
    t_typed_guid_mismatch();
    return TEST_SUMMARY();
}
