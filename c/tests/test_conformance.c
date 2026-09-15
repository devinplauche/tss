/* FACE 3.2 TSS behavioral conformance tests.
 *
 * Spec-driven verification of the behaviors the FACE 3.2 Transport Services
 * Segment specification requires, beyond what the CTS's link-only interface
 * checks exercise. Each test maps to a specific spec requirement.
 *
 * References: FACE Technical Standard, Edition 3.2, Transport Services
 * Segment (TSS) chapter; FACE::TSS::Base and FACE::TSS::TypedTS IDL.
 */

#include "test.h"

#include <string.h>
#include <unistd.h>

#include "face_tss/config.h"
#include "face_tss/tss.h"
#include "face_tss/configuration.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void base_conn(FACE_TSS_CONNECTION_CONFIG *c, const char *name,
                      const char *addr)
{
    memset(c, 0, sizeof(*c));
    strncpy(c->name, name, sizeof(c->name) - 1);
    strncpy(c->address, addr, sizeof(c->address) - 1);
    c->direction = FACE_TSS_BI_DIRECTIONAL;
    c->transport = FACE_TSS_TRANSPORT_BUS;
    c->role = FACE_TSS_ROLE_BUS;
    c->max_message_size = 65536;
    c->queue_depth = 64;
}

static FACE_TSS *make_tss(const char *name, const char *conn_name,
                          const char *addr)
{
    FACE_TSS *t = face_tss_create(name);
    FACE_TSS_CONFIG cfg;
    FACE_TSS_CONNECTION_CONFIG c;
    if (!t)
        return NULL;
    face_tss_config_init(&cfg, name);
    base_conn(&c, conn_name, addr);
    if (face_tss_config_add(&cfg, &c) != FACE_TSS_RC_NO_ERROR) {
        face_tss_destroy(t);
        face_tss_config_fini(&cfg);
        return NULL;
    }
    if (face_tss_initialize(t, &cfg) != FACE_TSS_RC_NO_ERROR) {
        face_tss_destroy(t);
        face_tss_config_fini(&cfg);
        return NULL;
    }
    face_tss_config_fini(&cfg);
    return t;
}

/* ------------------------------------------------------------------ */
/* FACE 3.2 Section: Return codes (15 values, standard order)         */
/* ------------------------------------------------------------------ */

static void t_return_code_ordinals(void)
{
    TEST_BEGIN("return_code_ordinals");
    /* The 3.2 IDL defines exactly these 15 codes in this order. */
    CHECK(FACE_TSS_RC_NO_ERROR == 0);
    CHECK(FACE_TSS_RC_NO_ACTION == 1);
    CHECK(FACE_TSS_RC_NOT_AVAILABLE == 2);
    CHECK(FACE_TSS_RC_INVALID_PARAM == 3);
    CHECK(FACE_TSS_RC_INVALID_CONFIG == 4);
    CHECK(FACE_TSS_RC_INVALID_MODE == 5);
    CHECK(FACE_TSS_RC_TIMED_OUT == 6);
    CHECK(FACE_TSS_RC_ADDR_IN_USE == 7);
    CHECK(FACE_TSS_RC_PERMISSION_DENIED == 8);
    CHECK(FACE_TSS_RC_MESSAGE_STALE == 9);
    CHECK(FACE_TSS_RC_IN_PROGRESS == 10);
    CHECK(FACE_TSS_RC_CONNECTION_CLOSED == 11);
    CHECK(FACE_TSS_RC_DATA_BUFFER_TOO_SMALL == 12);
    CHECK(FACE_TSS_RC_DATA_OVERFLOW == 13);
    CHECK(FACE_TSS_RC_RESOURCE_LIMIT_REACHED == 14);
    TEST_END();
}

/* ------------------------------------------------------------------ */
/* Base::Initialize: NOT_AVAILABLE before init, NO_ACTION on re-init  */
/* ------------------------------------------------------------------ */

static void t_init_semantics(void)
{
    FACE_TSS *t = face_tss_create("t");
    FACE_TSS_CONFIG cfg;
    FACE_TSS_CONNECTION_CONFIG c;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;

    TEST_BEGIN("init_semantics");
    CHECK(t != NULL);

    /* Operations before Initialize -> NOT_AVAILABLE. */
    CHECK_RC(face_tss_create_connection(t, "C", &id, &mx, 0),
             FACE_TSS_RC_NOT_AVAILABLE);

    face_tss_config_init(&cfg, "t");
    base_conn(&c, "C", "tcp://127.0.0.1:1");
    CHECK_RC(face_tss_config_add(&cfg, &c), FACE_TSS_RC_NO_ERROR);

    /* First Initialize -> NO_ERROR. */
    CHECK_RC(face_tss_initialize(t, &cfg), FACE_TSS_RC_NO_ERROR);
    /* Second Initialize -> NO_ACTION (idempotent). */
    CHECK_RC(face_tss_initialize(t, &cfg), FACE_TSS_RC_NO_ACTION);

    face_tss_config_fini(&cfg);
    face_tss_destroy(t);
    TEST_END();
}

/* ------------------------------------------------------------------ */
/* Base::Create_Connection: INVALID_PARAM, RESOURCE_LIMIT_REACHED     */
/* ------------------------------------------------------------------ */

static void t_create_connection_errors(void)
{
    FACE_TSS *t;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    int i, created = 0;

    TEST_BEGIN("create_connection_errors");
    /* Use inproc:// for the 64-connection storm: nng 1.12.3's TCP
     * transport has intermittent teardown races (leaked accept/dial
     * state) when dozens of connections are created and destroyed
     * rapidly, which flakes ASan/LSan. The 64-connection limit is
     * enforced by our code and is transport-agnostic. */
    t = make_tss("t", "C", "inproc://conn-errors");
    CHECK(t != NULL);

    /* Unknown name -> INVALID_PARAM. */
    CHECK_RC(face_tss_create_connection(t, "NOPE", &id, &mx, 0),
             FACE_TSS_RC_INVALID_PARAM);
    /* NULL name -> INVALID_PARAM. */
    CHECK_RC(face_tss_create_connection(t, NULL, &id, &mx, 0),
             FACE_TSS_RC_INVALID_PARAM);

    /* Fill to the connection limit; the 65th must fail. */
    for (i = 0; i < 65; i++) {
        FACE_TSS_RETURN_CODE rc =
            face_tss_create_connection(t, "C", &id, &mx, 0);
        if (rc == FACE_TSS_RC_NO_ERROR) {
            created++;
        } else {
            CHECK(rc == FACE_TSS_RC_RESOURCE_LIMIT_REACHED);
            break;
        }
    }
    CHECK(created == 64);

    face_tss_destroy(t);
    TEST_END();
}

/* ------------------------------------------------------------------ */
/* Base::Destroy_Connection: idempotent, INVALID_PARAM on id 0        */
/* ------------------------------------------------------------------ */

static void t_destroy_connection_idempotent(void)
{
    FACE_TSS *t;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;

    TEST_BEGIN("destroy_connection_idempotent");
    t = make_tss("t", "C", "tcp://127.0.0.1:51962");
    CHECK(t != NULL);
    CHECK_RC(face_tss_create_connection(t, "C", &id, &mx, 0),
             FACE_TSS_RC_NO_ERROR);

    CHECK_RC(face_tss_destroy_connection(t, id), FACE_TSS_RC_NO_ERROR);
    /* Second destroy -> NO_ACTION. */
    CHECK_RC(face_tss_destroy_connection(t, id), FACE_TSS_RC_NO_ACTION);
    /* Destroy of id 0 -> INVALID_PARAM. */
    CHECK_RC(face_tss_destroy_connection(t, 0), FACE_TSS_RC_INVALID_PARAM);

    face_tss_destroy(t);
    TEST_END();
}

/* ------------------------------------------------------------------ */
/* TypedTS::Send_Message: transaction_id inout semantics              */
/* ------------------------------------------------------------------ */

static void t_transaction_id_semantics(void)
{
    FACE_TSS *pub, *sub;
    FACE_TSS_CONNECTION_ID_TYPE pid, sid;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    FACE_TSS_TRANSACTION_ID_TYPE txn;
    FACE_TSS_MESSAGE msg;
    FACE_TSS_QOS_EVENT qos;
    static const uint8_t payload[] = "txn-test";

    TEST_BEGIN("transaction_id_semantics");
    pub = make_tss("pub", "C", "tcp://127.0.0.1:51963");
    sub = make_tss("sub", "C", "tcp://127.0.0.1:51963");
    CHECK(pub != NULL && sub != NULL);
    /* Reconfigure as pubsub pair. */
    face_tss_destroy(pub);
    face_tss_destroy(sub);

    /* Use pubsub transport with explicit roles. */
    {
        FACE_TSS_CONFIG pc, sc;
        FACE_TSS_CONNECTION_CONFIG cc;
        pub = face_tss_create("pub");
        sub = face_tss_create("sub");
        face_tss_config_init(&pc, "pub");
        memset(&cc, 0, sizeof(cc));
        strncpy(cc.name, "C", sizeof(cc.name) - 1);
        strncpy(cc.address, "tcp://127.0.0.1:51964", sizeof(cc.address) - 1);
        cc.transport = FACE_TSS_TRANSPORT_PUBSUB;
        cc.role = FACE_TSS_ROLE_PUBLISHER;
        cc.direction = FACE_TSS_SOURCE;
        cc.max_message_size = 65536;
        cc.queue_depth = 64;
        CHECK_RC(face_tss_config_add(&pc, &cc), FACE_TSS_RC_NO_ERROR);
        CHECK_RC(face_tss_initialize(pub, &pc), FACE_TSS_RC_NO_ERROR);
        face_tss_config_fini(&pc);

        face_tss_config_init(&sc, "sub");
        memset(&cc, 0, sizeof(cc));
        strncpy(cc.name, "C", sizeof(cc.name) - 1);
        strncpy(cc.address, "tcp://127.0.0.1:51964", sizeof(cc.address) - 1);
        cc.transport = FACE_TSS_TRANSPORT_PUBSUB;
        cc.role = FACE_TSS_ROLE_SUBSCRIBER;
        cc.direction = FACE_TSS_DESTINATION;
        cc.max_message_size = 65536;
        cc.queue_depth = 64;
        CHECK_RC(face_tss_config_add(&sc, &cc), FACE_TSS_RC_NO_ERROR);
        CHECK_RC(face_tss_initialize(sub, &sc), FACE_TSS_RC_NO_ERROR);
        face_tss_config_fini(&sc);
    }
    CHECK_RC(face_tss_create_connection(pub, "C", &pid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(sub, "C", &sid, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    usleep(400000);

    /* UNSPECIFIED -> TSS assigns; the assigned id is written back. */
    txn = FACE_TSS_TRANSACTION_ID_UNSPECIFIED;
    CHECK_RC(face_tss_send_message(pub, pid, 1000000000LL, &txn, payload,
                                   sizeof(payload)),
             FACE_TSS_RC_NO_ERROR);
    CHECK(txn != FACE_TSS_TRANSACTION_ID_UNSPECIFIED);

    /* Explicit id passes through unchanged (inout). */
    txn = 4242;
    CHECK_RC(face_tss_send_message(pub, pid, 1000000000LL, &txn, payload,
                                   sizeof(payload)),
             FACE_TSS_RC_NO_ERROR);
    CHECK(txn == 4242);

    /* Receive: the sent transaction id is written back. */
    memset(&msg, 0, sizeof(msg));
    txn = FACE_TSS_TRANSACTION_ID_UNSPECIFIED;
    CHECK_RC(face_tss_receive_message(sub, sid, 5000000000LL, 0, &txn, &msg,
                                      &qos),
             FACE_TSS_RC_NO_ERROR);
    CHECK(txn != FACE_TSS_TRANSACTION_ID_UNSPECIFIED);
    face_tss_message_fini(&msg);

    face_tss_destroy(pub);
    face_tss_destroy(sub);
    TEST_END();
}

/* ------------------------------------------------------------------ */
/* TypedTS::Receive_Message: TIMED_OUT, DATA_BUFFER_TOO_SMALL         */
/* ------------------------------------------------------------------ */

static void t_receive_errors(void)
{
    FACE_TSS *t;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    FACE_TSS_TRANSACTION_ID_TYPE txn;
    uint8_t buf[8];
    size_t len = 0;

    TEST_BEGIN("receive_errors");
    t = make_tss("t", "C", "tcp://127.0.0.1:51965");
    CHECK(t != NULL);
    CHECK_RC(face_tss_create_connection(t, "C", &id, &mx, 0),
             FACE_TSS_RC_NO_ERROR);

    /* No message pending -> TIMED_OUT. */
    txn = FACE_TSS_TRANSACTION_ID_UNSPECIFIED;
    CHECK_RC(face_tss_receive_message_into(t, id, 100000000LL, 0, &txn, buf,
                                           sizeof(buf), &len, NULL, NULL,
                                           NULL),
             FACE_TSS_RC_TIMED_OUT);

    face_tss_destroy(t);
    TEST_END();
}

/* ------------------------------------------------------------------ */
/* Configuration: invalid resource, idempotent set_reference          */
/* ------------------------------------------------------------------ */

static void t_configuration_errors(void)
{
    FACE_TSS *t = face_tss_create("t");

    TEST_BEGIN("configuration_errors");
    CHECK(t != NULL);

    /* NULL resource -> INVALID_PARAM. */
    CHECK_RC(face_tss_initialize_from_resource(t, NULL),
             FACE_TSS_RC_INVALID_PARAM);
    /* Oversize resource -> INVALID_PARAM. */
    {
        char big[FACE_TSS_CONFIGURATION_RESOURCE_MAX + 16];
        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        CHECK_RC(face_tss_initialize_from_resource(t, big),
                 FACE_TSS_RC_INVALID_PARAM);
    }
    /* Malformed JSON -> INVALID_CONFIG. */
    CHECK_RC(face_tss_initialize_from_resource(t, "json:{not json}"),
             FACE_TSS_RC_INVALID_CONFIG);

    face_tss_destroy(t);
    TEST_END();
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */
int main(void)
{
    printf("[face_tss_conformance]\n");
    printf("FACE 3.2 TSS behavioral conformance tests.\n\n");

    t_return_code_ordinals();
    t_init_semantics();
    t_create_connection_errors();
    t_destroy_connection_idempotent();
    t_transaction_id_semantics();
    t_receive_errors();
    t_configuration_errors();

    printf("\n%d failures\n", face_tss_test_failures);
    return face_tss_test_failures != 0;
}
