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
    FACE_TSS_TRANSACTION_ID_TYPE txn = FACE_TSS_TRANSACTION_ID_UNSPECIFIED;
    TEST_BEGIN("use_before_init");
    CHECK(t != NULL);
    CHECK(face_tss_create_connection(t, "C", &id, &mx, 0) ==
          FACE_TSS_RC_NOT_AVAILABLE);
    CHECK(face_tss_send_message(t, 1, 0, &txn, (const uint8_t *)"x", 1) ==
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
    FACE_TSS_TRANSACTION_ID_TYPE txn = FACE_TSS_TRANSACTION_ID_UNSPECIFIED;
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
    CHECK(face_tss_receive_message(t, id, 0, 0, &txn, &m, NULL) ==
          FACE_TSS_RC_INVALID_MODE);
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
    FACE_TSS_TRANSACTION_ID_TYPE txn = FACE_TSS_TRANSACTION_ID_UNSPECIFIED;
    TEST_BEGIN("use_after_destroy");
    face_tss_config_init(&cfg, "t");
    base_conn(&c, "C", "tcp://127.0.0.1:49162");
    CHECK_RC(face_tss_config_add(&cfg, &c), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(t, &cfg), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(t, "c", &id, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_destroy_connection(t, id), FACE_TSS_RC_NO_ERROR);
    CHECK(face_tss_send_message(t, id, 0, &txn, (const uint8_t *)"x", 1) ==
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

/* FACE conformance: all 14 return codes have distinct standard names. */
static void t_return_code_names(void)
{
    TEST_BEGIN("return_code_names");
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_NO_ERROR), "NO_ERROR") == 0);
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_NO_ACTION), "NO_ACTION") == 0);
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_NOT_AVAILABLE),
                 "NOT_AVAILABLE") == 0);
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_INVALID_PARAM),
                 "INVALID_PARAM") == 0);
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_INVALID_CONFIG),
                 "INVALID_CONFIG") == 0);
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_INVALID_MODE),
                 "INVALID_MODE") == 0);
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_TIMED_OUT), "TIMED_OUT") == 0);
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_ADDR_IN_USE),
                 "ADDR_IN_USE") == 0);
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_PERMISSION_DENIED),
                 "PERMISSION_DENIED") == 0);
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_MESSAGE_STALE),
                 "MESSAGE_STALE") == 0);
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_IN_PROGRESS),
                 "IN_PROGRESS") == 0);
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_CONNECTION_CLOSED),
                 "CONNECTION_CLOSED") == 0);
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_DATA_BUFFER_TOO_SMALL),
                 "DATA_BUFFER_TOO_SMALL") == 0);
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_DATA_OVERFLOW),
                 "DATA_OVERFLOW") == 0);
    CHECK(strcmp(face_tss_rc_str(FACE_TSS_RC_RESOURCE_LIMIT_REACHED),
                 "RESOURCE_LIMIT_REACHED") == 0);
    /* Values match the FACE standard's enumeration order. */
    CHECK(FACE_TSS_RC_NO_ERROR == 0 && FACE_TSS_RC_NO_ACTION == 1 &&
          FACE_TSS_RC_NOT_AVAILABLE == 2 && FACE_TSS_RC_INVALID_PARAM == 3 &&
          FACE_TSS_RC_INVALID_CONFIG == 4 && FACE_TSS_RC_INVALID_MODE == 5 &&
          FACE_TSS_RC_TIMED_OUT == 6 && FACE_TSS_RC_ADDR_IN_USE == 7 &&
          FACE_TSS_RC_PERMISSION_DENIED == 8 &&
          FACE_TSS_RC_MESSAGE_STALE == 9 && FACE_TSS_RC_IN_PROGRESS == 10 &&
          FACE_TSS_RC_CONNECTION_CLOSED == 11 &&
          FACE_TSS_RC_DATA_BUFFER_TOO_SMALL == 12 &&
          FACE_TSS_RC_DATA_OVERFLOW == 13 &&
          FACE_TSS_RC_RESOURCE_LIMIT_REACHED == 14);
    TEST_END();
}

/* FACE 3.2: RESOURCE_LIMIT_REACHED when the connection table is full. */
static void t_connection_limit(void)
{
    FACE_TSS *t = face_tss_create("t");
    FACE_TSS_CONFIG cfg;
    FACE_TSS_CONNECTION_CONFIG c;
    FACE_TSS_CONNECTION_ID_TYPE ids[FACE_TSS_MAX_CONNECTIONS];
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    size_t i;
    TEST_BEGIN("connection_limit");
    face_tss_config_init(&cfg, "t");
    base_conn(&c, "C", "tcp://127.0.0.1:49170");
    c.direction = FACE_TSS_DESTINATION;
    c.transport = FACE_TSS_TRANSPORT_PUBSUB;
    c.role = FACE_TSS_ROLE_SUBSCRIBER;
    CHECK_RC(face_tss_config_add(&cfg, &c), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_initialize(t, &cfg), FACE_TSS_RC_NO_ERROR);
    for (i = 0; i < FACE_TSS_MAX_CONNECTIONS; i++)
        CHECK_RC(face_tss_create_connection(t, "c", &ids[i], &mx, 0),
                 FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(t, "c", &id, &mx, 0),
             FACE_TSS_RC_RESOURCE_LIMIT_REACHED);
    /* Freeing one slot lets creation succeed again. */
    CHECK_RC(face_tss_destroy_connection(t, ids[0]), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(t, "c", &id, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_destroy_connection(t, id), FACE_TSS_RC_NO_ERROR);
    for (i = 1; i < FACE_TSS_MAX_CONNECTIONS; i++)
        CHECK_RC(face_tss_destroy_connection(t, ids[i]), FACE_TSS_RC_NO_ERROR);
    face_tss_config_fini(&cfg);
    face_tss_destroy(t);
    TEST_END();
}

/* FACE Configuration interface (issue #3): Set_Reference injects a
 * Configuration provider; Initialize takes a CONFIGURATION_RESOURCE. */
static char seen_resource[256];

static FACE_TSS_RETURN_CODE test_cfg_load(const char *resource,
                                          FACE_TSS_CONFIG *config,
                                          void *user)
{
    FACE_TSS_CONNECTION_CONFIG c;
    if (!resource || !config)
        return FACE_TSS_RC_INVALID_PARAM;
    strncpy(seen_resource, resource, sizeof(seen_resource) - 1);
    seen_resource[sizeof(seen_resource) - 1] = '\0';
    (void)user;
    face_tss_config_init(config, "injected");
    base_conn(&c, "C", "inproc://cfg-test");
    if (face_tss_config_add(config, &c) != FACE_TSS_RC_NO_ERROR) {
        face_tss_config_fini(config);
        return FACE_TSS_RC_INVALID_CONFIG;
    }
    return FACE_TSS_RC_NO_ERROR;
}

static FACE_TSS_RETURN_CODE other_cfg_load(const char *resource,
                                           FACE_TSS_CONFIG *config,
                                           void *user)
{
    (void)resource; (void)config; (void)user;
    return FACE_TSS_RC_NO_ERROR;
}

static void t_configuration_interface(void)
{
    FACE_TSS *t;
    FACE_TSS_CONFIGURATION cfg_iface, other_iface, null_iface;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    static const char *json =
        "{\"instance_name\": \"r\", \"connections\": ["
        " {\"name\": \"C\", \"transport\": \"bus\", \"role\": \"bus\","
        "  \"address\": \"inproc://cfg-json\"}]}";
    char resource[512];
    TEST_BEGIN("configuration_interface");
    t = face_tss_create("t");
    CHECK(t != NULL);

    memset(&cfg_iface, 0, sizeof(cfg_iface));
    cfg_iface.load = test_cfg_load;
    memset(&other_iface, 0, sizeof(other_iface));
    other_iface.load = other_cfg_load;
    memset(&null_iface, 0, sizeof(null_iface)); /* load == NULL */

    /* Injectable validation. */
    CHECK(face_tss_set_reference(NULL, "Configuration", &cfg_iface, 1) ==
          FACE_TSS_RC_INVALID_PARAM);
    CHECK(face_tss_set_reference(t, "Configuration", NULL, 1) ==
          FACE_TSS_RC_INVALID_PARAM);
    CHECK(face_tss_set_reference(t, "Configuration", &null_iface, 1) ==
          FACE_TSS_RC_INVALID_PARAM);
    CHECK(face_tss_set_reference(t, "NotAConfiguration", &cfg_iface, 1) ==
          FACE_TSS_RC_INVALID_PARAM);

    /* First set wins; duplicate is NO_ACTION; different is NOT_AVAILABLE. */
    CHECK_RC(face_tss_set_reference(t, "Configuration", &cfg_iface, 7),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_set_reference(t, "Configuration", &cfg_iface, 7),
             FACE_TSS_RC_NO_ACTION);
    CHECK(face_tss_set_reference(t, "Configuration", &other_iface, 8) ==
          FACE_TSS_RC_NOT_AVAILABLE);

    /* Initialize(CONFIGURATION_RESOURCE) goes through the injected
     * Configuration interface. */
    seen_resource[0] = '\0';
    CHECK_RC(face_tss_initialize_from_resource(t, "service://my-config"),
             FACE_TSS_RC_NO_ERROR);
    CHECK(strcmp(seen_resource, "service://my-config") == 0);
    CHECK_RC(face_tss_create_connection(t, "c", &id, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    /* Steady state: no more Set_Reference; Initialize is idempotent. */
    CHECK(face_tss_set_reference(t, "Configuration", &cfg_iface, 7) ==
          FACE_TSS_RC_INVALID_MODE);
    CHECK_RC(face_tss_initialize_from_resource(t, "service://my-config"),
             FACE_TSS_RC_NO_ACTION);
    face_tss_destroy(t);

    /* Built-in JSON adapter: inline "json:{...}". */
    t = face_tss_create("t2");
    snprintf(resource, sizeof(resource), "json:%s", json);
    CHECK_RC(face_tss_initialize_from_resource(t, resource),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_create_connection(t, "c", &id, &mx, 0),
             FACE_TSS_RC_NO_ERROR);
    face_tss_destroy(t);

    /* Bad resource / oversize resource. */
    t = face_tss_create("t3");
    CHECK(face_tss_initialize_from_resource(t, NULL) ==
          FACE_TSS_RC_INVALID_PARAM);
    CHECK(face_tss_initialize_from_resource(t, "/nonexistent/x.json") ==
          FACE_TSS_RC_INVALID_CONFIG);
    memset(resource, 'r', sizeof(resource) - 1);
    resource[sizeof(resource) - 1] = '\0';
    CHECK(face_tss_initialize_from_resource(t, resource) ==
          FACE_TSS_RC_INVALID_PARAM);
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
    t_return_code_names();
    t_connection_limit();
    t_configuration_interface();
    return TEST_SUMMARY();
}
