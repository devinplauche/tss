/* Config tests: parsing, validation, case-insensitive lookup. */
#include "test.h"

#include <string.h>

#include "face_tss/config.h"
#include "face_tss/tss.h" /* face_tss_rc_str */

static void t_normalize(void)
{
    char out[FACE_TSS_MAX_CONNECTION_NAME];
    TEST_BEGIN("normalize");
    CHECK_RC(face_tss_normalize_name("hello", out), FACE_TSS_RC_NO_ERROR);
    CHECK(strcmp(out, "HELLO") == 0);
    CHECK(face_tss_normalize_name("", out) == FACE_TSS_RC_INVALID_PARAM);
    CHECK(face_tss_normalize_name(NULL, out) == FACE_TSS_RC_INVALID_PARAM);
    {
        char big[FACE_TSS_MAX_CONNECTION_NAME + 8];
        memset(big, 'X', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        CHECK(face_tss_normalize_name(big, out) == FACE_TSS_RC_INVALID_PARAM);
    }
    TEST_END();
}

static FACE_TSS_CONNECTION_CONFIG conn_tpl(const char *name, const char *addr)
{
    FACE_TSS_CONNECTION_CONFIG c;
    memset(&c, 0, sizeof(c));
    strncpy(c.name, name, sizeof(c.name) - 1);
    strncpy(c.address, addr, sizeof(c.address) - 1);
    c.direction = FACE_TSS_BI_DIRECTIONAL;
    c.transport = FACE_TSS_TRANSPORT_PUBSUB;
    c.role = FACE_TSS_ROLE_SUBSCRIBER;
    c.max_message_size = 65536;
    c.queue_depth = 64;
    return c;
}

static void t_lookup(void)
{
    FACE_TSS_CONFIG cfg;
    FACE_TSS_CONNECTION_CONFIG c;
    TEST_BEGIN("lookup_case_insensitive");
    face_tss_config_init(&cfg, "t");
    c = conn_tpl("Position", "tcp://127.0.0.1:1");
    CHECK_RC(face_tss_config_add(&cfg, &c), FACE_TSS_RC_NO_ERROR);
    CHECK(face_tss_config_lookup(&cfg, "position") != NULL);
    CHECK(face_tss_config_lookup(&cfg, "POSITION") != NULL);
    CHECK(face_tss_config_lookup(&cfg, "nope") == NULL);
    /* duplicate (case-insensitive) rejected */
    c = conn_tpl("position", "tcp://127.0.0.1:2");
    CHECK(face_tss_config_add(&cfg, &c) == FACE_TSS_RC_INVALID_CONFIG);
    face_tss_config_fini(&cfg);
    TEST_END();
}

static void t_roles(void)
{
    FACE_TSS_CONFIG cfg;
    FACE_TSS_CONNECTION_CONFIG c;
    TEST_BEGIN("roles_and_directions");
    face_tss_config_init(&cfg, "t");
    /* pubsub + bus role rejected */
    c = conn_tpl("A", "tcp://127.0.0.1:1");
    c.role = FACE_TSS_ROLE_BUS;
    CHECK(face_tss_config_add(&cfg, &c) == FACE_TSS_RC_INVALID_CONFIG);
    /* bus coerces any role to bus */
    c = conn_tpl("B", "tcp://127.0.0.1:1");
    c.transport = FACE_TSS_TRANSPORT_BUS;
    c.role = FACE_TSS_ROLE_SUBSCRIBER;
    CHECK_RC(face_tss_config_add(&cfg, &c), FACE_TSS_RC_NO_ERROR);
    CHECK(face_tss_config_lookup(&cfg, "b")->role == FACE_TSS_ROLE_BUS);
    /* unknown transport rejected */
    c = conn_tpl("C", "tcp://127.0.0.1:1");
    c.transport = (FACE_TSS_TRANSPORT_KIND)99;
    CHECK(face_tss_config_add(&cfg, &c) == FACE_TSS_RC_INVALID_CONFIG);
    /* directions */
    c = conn_tpl("S", "tcp://127.0.0.1:1");
    c.direction = FACE_TSS_SOURCE;
    c.role = FACE_TSS_ROLE_PUBLISHER;
    CHECK_RC(face_tss_config_add(&cfg, &c), FACE_TSS_RC_NO_ERROR);
    CHECK(face_tss_config_lookup(&cfg, "s")->direction == FACE_TSS_SOURCE);
    face_tss_config_fini(&cfg);
    TEST_END();
}

static void t_json(void)
{
    FACE_TSS_CONFIG cfg;
    static const char *json =
        "{\"instance_name\": \"j\", \"connections\": ["
        " {\"name\": \"X\", \"direction\": \"SOURCE\", \"transport\": \"pubsub\","
        "  \"role\": \"publisher\", \"address\": \"tcp://127.0.0.1:9\"},"
        " {\"name\": \"Y\", \"transport\": \"bus\", \"address\": \"tcp://127.0.0.1:10\"}"
        "]}";
    const FACE_TSS_CONNECTION_CONFIG *x, *y;
    TEST_BEGIN("json_parse");
    face_tss_config_init(&cfg, "t");
    CHECK_RC(face_tss_config_from_json(json, strlen(json), &cfg),
             FACE_TSS_RC_NO_ERROR);
    CHECK(strcmp(cfg.instance_name, "j") == 0);
    CHECK(cfg.count == 2);
    x = face_tss_config_lookup(&cfg, "x");
    CHECK(x && x->direction == FACE_TSS_SOURCE);
    CHECK(x->role == FACE_TSS_ROLE_PUBLISHER);
    y = face_tss_config_lookup(&cfg, "y");
    CHECK(y && y->transport == FACE_TSS_TRANSPORT_BUS);
    CHECK(y->role == FACE_TSS_ROLE_BUS); /* coerced */
    CHECK(y->max_message_size == 65536); /* defaulted */
    /* malformed */
    CHECK(face_tss_config_from_json("{nope", 5, &cfg) ==
          FACE_TSS_RC_INVALID_CONFIG);
    CHECK(face_tss_config_from_json("{\"connections\":[{\"direction\":1}]}",
                                    30, &cfg) == FACE_TSS_RC_INVALID_CONFIG);
    face_tss_config_fini(&cfg);
    TEST_END();
}

int main(void)
{
    printf("[config]\n");
    t_normalize();
    t_lookup();
    t_roles();
    t_json();
    return TEST_SUMMARY();
}
