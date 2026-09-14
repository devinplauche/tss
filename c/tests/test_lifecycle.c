/* TSS lifecycle tests (loopback binds only). */
#include "test.h"

#include <string.h>

#include "face_tss/config.h"
#include "face_tss/tss.h"

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

static void t_not_initialized(void)
{
    FACE_TSS *t = face_tss_create("t");
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    TEST_BEGIN("use_before_init");
    CHECK(t != NULL);
    CHECK(face_tss_create_connection(t, "C", &id, &mx, 0) ==
          FACE_TSS_RC_NOT_AVAILABLE);
    CHECK(face_tss_send_message(t, 1, (const uint8_t *)"x", 1, 0) ==
          FACE_TSS_RC_NOT_AVAILABLE);
    CHECK(face_tss_destroy_connection(t, 1) == FACE_TSS_RC_NOT_AVAILABLE);
    face_tss_destroy(t);
    TEST_END();
}

static void t_init_idempotent(void)
{
    FACE_TSS *t = face_tss_create("t");
    FACE_TSS_CONFIG cfg;
    FACE_TSS_CONNECTION_CONFIG c;
    TEST_BEGIN("initialize_idempotent");
    face_tss_config_init(&cfg, "t");
    base_conn(&c, "C", "tcp://127.0.0.1:1");
    CHECK_RC(face_tss_config_add(&cfg, &c), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(t, &cfg), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(t, &cfg), FACE_TSS_RC_NO_ACTION);
    face_tss_config_fini(&cfg);
    face_tss_destroy(t);
    TEST_END();
}

static void t_unknown_and_zero(void)
{
    FACE_TSS *t = face_tss_create("t");
    FACE_TSS_CONFIG cfg;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    TEST_BEGIN("unknown_and_zero_ids");
    face_tss_config_init(&cfg, "t");
    CHECK_RC(face_tss_initialize(t, &cfg), FACE_TSS_RC_NO_ERROR);
    CHECK(face_tss_create_connection(t, "NOPE", &id, &mx, 0) ==
          FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_destroy_connection(t, 999), FACE_TSS_RC_NO_ACTION);
    CHECK(face_tss_destroy_connection(t, 0) == FACE_TSS_RC_INVALID_PARAM);
    face_tss_config_fini(&cfg);
    face_tss_destroy(t);
    TEST_END();
}

static void t_direction(void)
{
    FACE_TSS *t = face_tss_create("t");
    FACE_TSS_CONFIG cfg;
    FACE_TSS_CONNECTION_CONFIG c;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    FACE_TSS_MESSAGE m;
    TEST_BEGIN("direction_enforcement");
    face_tss_config_init(&cfg, "t");
    base_conn(&c, "SRC", "tcp://127.0.0.1:49161");
    c.direction = FACE_TSS_SOURCE;
    CHECK_RC(face_tss_config_add(&cfg, &c), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(t, &cfg), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(t, "src", &id, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    memset(&m, 0, sizeof(m));
    CHECK(face_tss_receive_message(t, id, 0, 0, &m) == FACE_TSS_RC_INVALID_MODE);
    CHECK_RC(face_tss_destroy_connection(t, id), FACE_TSS_RC_NO_ERROR);
    face_tss_config_fini(&cfg);
    face_tss_destroy(t);
    TEST_END();
}

static void t_destroy_then_use(void)
{
    FACE_TSS *t = face_tss_create("t");
    FACE_TSS_CONFIG cfg;
    FACE_TSS_CONNECTION_CONFIG c;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    TEST_BEGIN("use_after_destroy");
    face_tss_config_init(&cfg, "t");
    base_conn(&c, "C", "tcp://127.0.0.1:49162");
    CHECK_RC(face_tss_config_add(&cfg, &c), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(t, &cfg), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(t, "c", &id, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_destroy_connection(t, id), FACE_TSS_RC_NO_ERROR);
    CHECK(face_tss_send_message(t, id, (const uint8_t *)"x", 1, 0) ==
          FACE_TSS_RC_CONNECTION_CLOSED);
    CHECK_RC(face_tss_unregister_callback(t, id),
             FACE_TSS_RC_CONNECTION_CLOSED);
    face_tss_config_fini(&cfg);
    face_tss_destroy(t);
    TEST_END();
}

static void t_unregister_none(void)
{
    FACE_TSS *t = face_tss_create("t");
    FACE_TSS_CONFIG cfg;
    FACE_TSS_CONNECTION_CONFIG c;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    TEST_BEGIN("unregister_without_callback");
    face_tss_config_init(&cfg, "t");
    base_conn(&c, "C", "tcp://127.0.0.1:49163");
    c.direction = FACE_TSS_DESTINATION;
    c.transport = FACE_TSS_TRANSPORT_PUBSUB;
    c.role = FACE_TSS_ROLE_SUBSCRIBER;
    CHECK_RC(face_tss_config_add(&cfg, &c), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(t, &cfg), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(t, "c", &id, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_unregister_callback(t, id), FACE_TSS_RC_NO_ACTION);
    CHECK_RC(face_tss_destroy_connection(t, id), FACE_TSS_RC_NO_ERROR);
    face_tss_config_fini(&cfg);
    face_tss_destroy(t);
    TEST_END();
}

int main(void)
{
    printf("[lifecycle]\n");
    t_not_initialized();
    t_init_idempotent();
    t_unknown_and_zero();
    t_direction();
    t_destroy_then_use();
    t_unregister_none();
    return TEST_SUMMARY();
}
