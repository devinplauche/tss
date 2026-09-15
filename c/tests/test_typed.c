/* Typed interface tests: register type support, typed send/receive over
 * real loopback sockets, typed callbacks, guid mismatch rejection. */
#include "test.h"

#include <string.h>
#if defined(_WIN32)
#include <windows.h>
static void msleep(long ms) { Sleep((DWORD)ms); }
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
#endif

#include "face_tss/config.h"
#include "face_tss/tss.h"
#include "face_tss/typed.h"
#include "positionreport_typed.h"
#include "telemetry_typed.h"
#include "event_typed.h"

#include <stdlib.h>

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
    CHECK(qos.count == 1);
    CHECK(strcmp(qos.elements[0].keyname, "message_age_ns") == 0);
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

typedef struct {
    int n;
    double last_lat;
#if defined(_WIN32)
    CRITICAL_SECTION mtx;
#else
    pthread_mutex_t mtx;
#endif
} tcb_state_t;

static void tcb_state_init(tcb_state_t *s)
{
#if defined(_WIN32)
    InitializeCriticalSection(&s->mtx);
#else
    pthread_mutex_init(&s->mtx, NULL);
#endif
}

static void tcb_state_fini(tcb_state_t *s)
{
#if defined(_WIN32)
    DeleteCriticalSection(&s->mtx);
#else
    pthread_mutex_destroy(&s->mtx);
#endif
}

static void tcb_state_lock(tcb_state_t *s)
{
#if defined(_WIN32)
    EnterCriticalSection(&s->mtx);
#else
    pthread_mutex_lock(&s->mtx);
#endif
}

static void tcb_state_unlock(tcb_state_t *s)
{
#if defined(_WIN32)
    LeaveCriticalSection(&s->mtx);
#else
    pthread_mutex_unlock(&s->mtx);
#endif
}

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
    tcb_state_lock(s);
    s->last_lat = r->latitude_deg;
    s->n++;
    tcb_state_unlock(s);
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
    tcb_state_init(&st);
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
    while (waited < 50) {
        int n;
        tcb_state_lock(&st);
        n = st.n;
        tcb_state_unlock(&st);
        if (n)
            break;
        msleep(100);
        waited++;
    }
    {
        int n;
        double last_lat;
        tcb_state_lock(&st);
        n = st.n;
        last_lat = st.last_lat;
        tcb_state_unlock(&st);
        CHECK(n == 1);
        CHECK(last_lat == 1.5);
    }
    CHECK_RC(face_tss_unregister_callback(sub, sid), FACE_TSS_RC_NO_ERROR);
    face_tss_destroy(pub);
    face_tss_destroy(sub);
    tcb_state_fini(&st);
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

/* Codegen extensions: nested table, scalar vector, enum - in-process
 * serialize/deserialize round trip, absent optionals, malformed input. */
static void t_codegen_extensions(void)
{
    Telemetry out, back;
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    float samples[] = { 1.5f, -2.25f, 3.75f };
    uint8_t flags[] = { 0x01, 0x02 };
    TEST_BEGIN("codegen_extensions");
    memset(&out, 0, sizeof(out));
    out.device = "sensor-1";
    out.fix = FixType_Rtk;
    out.pos = (struct GeoPoint *)calloc(1, sizeof(*out.pos));
    out.pos->lat = 42.5;
    out.pos->lon = -83.7;
    out.samples = samples;
    out.samples_count = 3;
    out.flags = flags;
    out.flags_count = 2;
    CHECK_RC(Telemetry_serialize(&out, &payload, &payload_len),
             FACE_TSS_RC_NO_ERROR);
    CHECK(payload != NULL && payload_len > 0);

    memset(&back, 0, sizeof(back));
    CHECK_RC(Telemetry_deserialize(payload, payload_len, &back),
             FACE_TSS_RC_NO_ERROR);
    CHECK(strcmp(back.device, "sensor-1") == 0);
    CHECK(back.fix == FixType_Rtk);
    CHECK(back.pos != NULL);
    CHECK(back.pos->lat == 42.5 && back.pos->lon == -83.7);
    CHECK(back.samples_count == 3);
    CHECK(back.samples[0] == 1.5f && back.samples[1] == -2.25f &&
          back.samples[2] == 3.75f);
    CHECK(back.flags_count == 2);
    CHECK(back.flags[0] == 0x01 && back.flags[1] == 0x02);
    Telemetry_fini(&back);
    free(payload);

    /* Optional nested table / vectors absent: still round-trips. */
    {
        Telemetry sparse, sparse_back;
        uint8_t *p2 = NULL;
        size_t l2 = 0;
        memset(&sparse, 0, sizeof(sparse));
        sparse.device = "bare";
        CHECK_RC(Telemetry_serialize(&sparse, &p2, &l2),
                 FACE_TSS_RC_NO_ERROR);
        memset(&sparse_back, 0, sizeof(sparse_back));
        CHECK_RC(Telemetry_deserialize(p2, l2, &sparse_back),
                 FACE_TSS_RC_NO_ERROR);
        CHECK(strcmp(sparse_back.device, "bare") == 0);
        CHECK(sparse_back.fix == FixType_None);
        CHECK(sparse_back.pos == NULL);
        CHECK(sparse_back.samples == NULL && sparse_back.samples_count == 0);
        CHECK(sparse_back.flags == NULL && sparse_back.flags_count == 0);
        Telemetry_fini(&sparse_back);
        free(p2);
    }

    /* Malformed input is rejected, not crashed on. */
    {
        uint8_t junk[16];
        Telemetry bad;
        memset(junk, 0xFF, sizeof(junk));
        memset(&bad, 0, sizeof(bad));
        CHECK(Telemetry_deserialize(junk, sizeof(junk), &bad) ==
              FACE_TSS_RC_INVALID_PARAM);
        Telemetry_fini(&bad);
    }

    free(out.pos);
    TEST_END();
}

/* Codegen extensions: unions, vectors of tables/strings, nested vectors,
 * explicit field ids. In-process serialize/deserialize round trip. */
static uint32_t ks_rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint16_t ks_rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

/* Vtable slot offset for field idx in the root table, 0 when absent. */
static uint16_t ks_vslot(const uint8_t *buf, size_t len, int idx)
{
    uint32_t root, table, vtable;
    uint16_t vsize;
    int32_t soff;
    if (len < 8)
        return 0;
    root = ks_rd32(buf);
    if (root < 4 || root > (uint32_t)(len - 4))
        return 0;
    table = root;
    soff = (int32_t)ks_rd32(buf + table);
    vtable = table - (uint32_t)soff;
    if (vtable > (uint32_t)(len - 4))
        return 0;
    vsize = ks_rd16(buf + vtable);
    if (vsize < (uint16_t)(4 + 2 * idx + 2))
        return 0;
    return ks_rd16(buf + vtable + 4 + 2 * idx);
}

static char *ks_dup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *c = (char *)malloc(n);
    if (c)
        memcpy(c, s, n);
    return c;
}

static void t_codegen_advanced(void)
{
    Event out, back;
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    struct Alarm *al;
    struct SensorReading *sr0, *sr1, *sr2;
    struct Summary *sum;
    float *row0, *row2;
    TEST_BEGIN("codegen_advanced");

    memset(&out, 0, sizeof(out));
    out.name = ks_dup("evt-1");

    /* payload: union holding an Alarm (id 2 -> slots 2,3). */
    al = (struct Alarm *)calloc(1, sizeof(*al));
    al->code = 7;
    al->message = ks_dup("boom");
    out.payload.type = EventPayload_Alarm;
    out.payload.value = al;

    /* tags: [string] (id 7). */
    out.tags_count = 3;
    out.tags = (char **)malloc(3 * sizeof(char *));
    out.tags[0] = ks_dup("a");
    out.tags[1] = ks_dup("bb");
    out.tags[2] = ks_dup("ccc");

    /* readings: [SensorReading] (id 0). */
    out.readings_count = 2;
    out.readings =
        (struct SensorReading **)malloc(2 * sizeof(struct SensorReading *));
    sr0 = (struct SensorReading *)calloc(1, sizeof(*sr0));
    sr0->value = 1.5;
    sr0->unit = ks_dup("m");
    sr1 = (struct SensorReading *)calloc(1, sizeof(*sr1));
    sr1->value = 2.5;
    sr1->unit = ks_dup("s");
    out.readings[0] = sr0;
    out.readings[1] = sr1;

    /* matrix: [[float]] with an empty middle row (id 4). */
    out.matrix_count = 3;
    out.matrix = (float **)malloc(3 * sizeof(float *));
    out.matrix_counts = (size_t *)malloc(3 * sizeof(size_t));
    row0 = (float *)malloc(2 * sizeof(float));
    row0[0] = 1.0f;
    row0[1] = 2.0f;
    row2 = (float *)malloc(sizeof(float));
    row2[0] = 3.0f;
    out.matrix[0] = row0;
    out.matrix_counts[0] = 2;
    out.matrix[1] = NULL;
    out.matrix_counts[1] = 0;
    out.matrix[2] = row2;
    out.matrix_counts[2] = 1;

    /* summary: nested table, auto-assigned ids, union + table vector. */
    sum = (struct Summary *)calloc(1, sizeof(*sum));
    sum->title = ks_dup("s");
    sum->items_count = 1;
    sum->items = (struct Alarm **)malloc(sizeof(struct Alarm *));
    sum->items[0] = (struct Alarm *)calloc(1, sizeof(struct Alarm));
    sum->items[0]->code = 1;
    sum->items[0]->message = ks_dup("x");
    sr2 = (struct SensorReading *)calloc(1, sizeof(*sr2));
    sr2->value = 9.9;
    sr2->unit = ks_dup("km");
    sum->last.type = EventPayload_SensorReading;
    sum->last.value = sr2;
    out.summary = sum;

    CHECK_RC(Event_serialize(&out, &payload, &payload_len),
             FACE_TSS_RC_NO_ERROR);
    CHECK(payload != NULL && payload_len > 0);

    memset(&back, 0, sizeof(back));
    CHECK_RC(Event_deserialize(payload, payload_len, &back),
             FACE_TSS_RC_NO_ERROR);
    CHECK(strcmp(back.name, "evt-1") == 0);
    /* union */
    CHECK(back.payload.type == EventPayload_Alarm);
    CHECK(back.payload.value != NULL);
    CHECK(((struct Alarm *)back.payload.value)->code == 7);
    CHECK(strcmp(((struct Alarm *)back.payload.value)->message, "boom") == 0);
    /* [string] */
    CHECK(back.tags_count == 3);
    CHECK(strcmp(back.tags[0], "a") == 0);
    CHECK(strcmp(back.tags[1], "bb") == 0);
    CHECK(strcmp(back.tags[2], "ccc") == 0);
    /* [table] */
    CHECK(back.readings_count == 2);
    CHECK(back.readings[0]->value == 1.5);
    CHECK(strcmp(back.readings[0]->unit, "m") == 0);
    CHECK(back.readings[1]->value == 2.5);
    CHECK(strcmp(back.readings[1]->unit, "s") == 0);
    /* [[float]] */
    CHECK(back.matrix_count == 3);
    CHECK(back.matrix_counts[0] == 2);
    CHECK(back.matrix[0][0] == 1.0f && back.matrix[0][1] == 2.0f);
    CHECK(back.matrix[1] == NULL && back.matrix_counts[1] == 0);
    CHECK(back.matrix_counts[2] == 1 && back.matrix[2][0] == 3.0f);
    /* nested summary, auto ids */
    CHECK(back.summary != NULL);
    CHECK(strcmp(back.summary->title, "s") == 0);
    CHECK(back.summary->items_count == 1);
    CHECK(back.summary->items[0]->code == 1);
    CHECK(strcmp(back.summary->items[0]->message, "x") == 0);
    CHECK(back.summary->last.type == EventPayload_SensorReading);
    CHECK(((struct SensorReading *)back.summary->last.value)->value == 9.9);
    CHECK(strcmp(((struct SensorReading *)back.summary->last.value)->unit,
                 "km") == 0);
    Event_fini(&back);
    Event_fini(&out);
    free(payload);
    payload = NULL;

    /* Explicit ids land in the right vtable slots: only name (id 5) set. */
    {
        Event sparse, sparse_back;
        uint8_t *p2 = NULL;
        size_t l2 = 0;
        int i;
        memset(&sparse, 0, sizeof(sparse));
        sparse.name = ks_dup("only-name");
        CHECK_RC(Event_serialize(&sparse, &p2, &l2), FACE_TSS_RC_NO_ERROR);
        for (i = 0; i < 10; i++) {
            if (i == 5)
                CHECK(ks_vslot(p2, l2, i) != 0);
            else
                CHECK(ks_vslot(p2, l2, i) == 0);
        }
        memset(&sparse_back, 0, sizeof(sparse_back));
        CHECK_RC(Event_deserialize(p2, l2, &sparse_back),
                 FACE_TSS_RC_NO_ERROR);
        CHECK(strcmp(sparse_back.name, "only-name") == 0);
        CHECK(sparse_back.payload.type == EventPayload_NONE);
        CHECK(sparse_back.payload.value == NULL);
        CHECK(sparse_back.tags == NULL && sparse_back.tags_count == 0);
        CHECK(sparse_back.readings == NULL &&
              sparse_back.readings_count == 0);
        CHECK(sparse_back.matrix == NULL && sparse_back.matrix_count == 0);
        CHECK(sparse_back.summary == NULL);
        Event_fini(&sparse_back);
        Event_fini(&sparse);
        free(p2);
    }

    /* Corrupt the union discriminator -> INVALID_PARAM, no crash. */
    {
        Event with_union, uback;
        uint8_t *p3 = NULL;
        size_t l3 = 0;
        uint32_t root, table;
        uint16_t tslot;
        int32_t soff;
        struct Alarm *al2 = (struct Alarm *)calloc(1, sizeof(*al2));
        memset(&with_union, 0, sizeof(with_union));
        al2->code = 3;
        al2->message = ks_dup("z");
        with_union.payload.type = EventPayload_Alarm;
        with_union.payload.value = al2;
        CHECK_RC(Event_serialize(&with_union, &p3, &l3),
                 FACE_TSS_RC_NO_ERROR);
        root = ks_rd32(p3);
        table = root;
        soff = (int32_t)ks_rd32(p3 + table);
        tslot = ks_vslot(p3, l3, 2); /* discriminator slot (id 2) */
        CHECK(tslot != 0);
        p3[table + tslot] = 99; /* unknown discriminator */
        memset(&uback, 0, sizeof(uback));
        CHECK(Event_deserialize(p3, l3, &uback) ==
              FACE_TSS_RC_INVALID_PARAM);
        Event_fini(&uback);
        Event_fini(&with_union);
        free(p3);
        (void)soff;
    }

    TEST_END();
}

/* FACE 3.2: Unregister_Callback lives on TypedTS. */
static void t_typed_unregister(void)
{
    char a[64];
    FACE_TSS *pub, *sub;
    FACE_TSS_CONNECTION_ID_TYPE pid, sid;
    tcb_state_t st;
    TEST_BEGIN("typed_unregister");
    memset(&st, 0, sizeof(st));
    tcb_state_init(&st);
    addr(a);
    make_pair(a, &pub, &sub, &pid, &sid);
    CHECK_RC(face_tss_typed_unregister_callback(sub, sid, "Nope.Type"),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_typed_register_callback(sub, sid,
                                              "FaceTSS.PositionReport",
                                              on_typed, &st),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_typed_unregister_callback(sub, sid,
                                                "FaceTSS.PositionReport"),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_typed_unregister_callback(sub, sid,
                                                "FaceTSS.PositionReport"),
             FACE_TSS_RC_NO_ACTION);
    /* The old Base spelling still works as a compatibility alias. */
    CHECK_RC(face_tss_typed_register_callback(sub, sid,
                                              "FaceTSS.PositionReport",
                                              on_typed, &st),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_unregister_callback(sub, sid), FACE_TSS_RC_NO_ERROR);
    face_tss_destroy(pub);
    face_tss_destroy(sub);
    tcb_state_fini(&st);
    TEST_END();
}

int main(void)
{
    printf("[typed]\n");
    t_register_errors();
    t_typed_round_trip();
    t_typed_callback();
    t_typed_unregister();
    t_typed_guid_mismatch();
    t_codegen_extensions();
    t_codegen_advanced();
    return TEST_SUMMARY();
}
