/* Tests for CSP, TPM, marshalling, and QoS policy interfaces. */

#include "test.h"

#include <string.h>

#include "face_tss/tss.h"
#include "face_tss/csp.h"
#include "face_tss/tpm.h"
#include "face_tss/marshalling.h"
#include "face_tss/qos.h"

/* ------------------------------------------------------------------ */
/* CSP                                                                */
/* ------------------------------------------------------------------ */

static void t_csp_lifecycle(void)
{
    FACE_TSS_CSP *csp = face_tss_csp_create("test");
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE token = 0;

    TEST_BEGIN("csp_lifecycle");
    CHECK(csp != NULL);

    /* Init idempotent. */
    CHECK_RC(face_tss_csp_initialize(csp, ""), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_csp_initialize(csp, ""), FACE_TSS_RC_NO_ACTION);

    /* Open a store. */
    CHECK_RC(face_tss_csp_open(csp, 12345, "cfg1",
                               FACE_TSS_CSP_PRIVATE_DATA_STORE, &token),
             FACE_TSS_RC_NO_ERROR);
    CHECK(token != FACE_TSS_CSP_DATA_STORE_TOKEN_INVALID);

    /* Close it. */
    CHECK_RC(face_tss_csp_close(csp, 12345, token), FACE_TSS_RC_NO_ERROR);
    /* Close of bad token -> INVALID_PARAM. */
    CHECK_RC(face_tss_csp_close(csp, 12345, 99999), FACE_TSS_RC_INVALID_PARAM);

    face_tss_csp_destroy(csp);
    TEST_END();
}

static void t_csp_crud(void)
{
    FACE_TSS_CSP *csp = face_tss_csp_create("test");
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE token = 0;
    static const uint8_t data1[] = "hello";
    static const uint8_t data2[] = "world!";
    uint8_t buf[64];
    size_t len;

    TEST_BEGIN("csp_crud");
    CHECK(csp != NULL);
    CHECK_RC(face_tss_csp_initialize(csp, ""), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_csp_open(csp, 1, "cfg",
                               FACE_TSS_CSP_CHECKPOINT_DATA_STORE, &token),
             FACE_TSS_RC_NO_ERROR);

    /* Create. */
    CHECK_RC(face_tss_csp_create_entry(csp, 1, token, 42, data1,
                                       sizeof(data1)),
             FACE_TSS_RC_NO_ERROR);
    /* Duplicate create -> INVALID_PARAM. */
    CHECK_RC(face_tss_csp_create_entry(csp, 1, token, 42, data1,
                                       sizeof(data1)),
             FACE_TSS_RC_INVALID_PARAM);

    /* Read. */
    len = sizeof(buf);
    CHECK_RC(face_tss_csp_read(csp, 1, token, 42, buf, &len),
             FACE_TSS_RC_NO_ERROR);
    CHECK(len == sizeof(data1));
    CHECK(memcmp(buf, data1, len) == 0);

    /* Read with small buffer -> DATA_BUFFER_TOO_SMALL, len updated. */
    len = 2;
    CHECK_RC(face_tss_csp_read(csp, 1, token, 42, buf, &len),
             FACE_TSS_RC_DATA_BUFFER_TOO_SMALL);
    CHECK(len == sizeof(data1));

    /* Update. */
    CHECK_RC(face_tss_csp_update(csp, 1, token, 42, data2, sizeof(data2)),
             FACE_TSS_RC_NO_ERROR);
    len = sizeof(buf);
    CHECK_RC(face_tss_csp_read(csp, 1, token, 42, buf, &len),
             FACE_TSS_RC_NO_ERROR);
    CHECK(len == sizeof(data2));
    CHECK(memcmp(buf, data2, len) == 0);

    /* Delete. */
    CHECK_RC(face_tss_csp_delete(csp, 1, token, 42), FACE_TSS_RC_NO_ERROR);
    /* Read after delete -> INVALID_PARAM. */
    len = sizeof(buf);
    CHECK_RC(face_tss_csp_read(csp, 1, token, 42, buf, &len),
             FACE_TSS_RC_INVALID_PARAM);

    face_tss_csp_destroy(csp);
    TEST_END();
}

/* ------------------------------------------------------------------ */
/* TPM                                                                */
/* ------------------------------------------------------------------ */

static void t_tpm_lifecycle(void)
{
    FACE_TSS_TPM *tpm = face_tss_tpm_create("test");
    FACE_TSS_TPM_CHANNEL_ID_TYPE ch = 0;
    FACE_TSS_TPM_EVENT_TYPE status;

    TEST_BEGIN("tpm_lifecycle");
    CHECK(tpm != NULL);

    CHECK_RC(face_tss_tpm_initialize(tpm, ""), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_tpm_initialize(tpm, ""), FACE_TSS_RC_NO_ACTION);

    CHECK_RC(face_tss_tpm_get_status(tpm, &status), FACE_TSS_RC_NO_ERROR);
    CHECK(status == FACE_TSS_TPM_INIT_COMPLETE);

    /* Open/close channel. */
    CHECK_RC(face_tss_tpm_open_channel(tpm, "ep1", NULL, 0, NULL, 0, &ch),
             FACE_TSS_RC_NO_ERROR);
    CHECK(ch != FACE_TSS_TPM_CHANNEL_ID_INVALID);
    CHECK_RC(face_tss_tpm_close_channel(tpm, ch), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_tpm_close_channel(tpm, 99999), FACE_TSS_RC_INVALID_PARAM);

    /* State changes. */
    CHECK_RC(face_tss_tpm_request_state_change(
                 tpm, FACE_TSS_TPM_STATE_PAUSE, NULL, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_tpm_request_state_change(
                 tpm, FACE_TSS_TPM_STATE_NORMAL, NULL, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_tpm_request_state_change(
                 tpm, (FACE_TSS_TPM_STATE_TYPE)99, NULL, 0),
             FACE_TSS_RC_INVALID_PARAM);

    face_tss_tpm_destroy(tpm);
    TEST_END();
}

static void t_tpm_data(void)
{
    FACE_TSS_TPM *tpm = face_tss_tpm_create("test");
    FACE_TSS_TPM_CHANNEL_ID_TYPE ch = 0;
    static const uint8_t msg[] = "tpm-payload";
    uint8_t buf[64];
    size_t len;
    FACE_TSS_TRANSACTION_ID_TYPE txn;

    TEST_BEGIN("tpm_data");
    CHECK(tpm != NULL);
    CHECK_RC(face_tss_tpm_initialize(tpm, ""), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_tpm_open_channel(tpm, "ep1", NULL, 0, NULL, 0, &ch),
             FACE_TSS_RC_NO_ERROR);

    /* Write then read (loopback). */
    CHECK_RC(face_tss_tpm_write_to_transport(tpm, ch, 0, msg, sizeof(msg),
                                             777),
             FACE_TSS_RC_NO_ERROR);
    len = sizeof(buf);
    txn = 0;
    CHECK_RC(face_tss_tpm_read_from_transport(tpm, ch, 0, &txn, buf, &len),
             FACE_TSS_RC_NO_ERROR);
    CHECK(len == sizeof(msg));
    CHECK(memcmp(buf, msg, len) == 0);
    CHECK(txn == 777);

    /* Read with no data -> TIMED_OUT. */
    len = sizeof(buf);
    CHECK_RC(face_tss_tpm_read_from_transport(tpm, ch, 0, &txn, buf, &len),
             FACE_TSS_RC_TIMED_OUT);

    face_tss_tpm_destroy(tpm);
    TEST_END();
}

/* ------------------------------------------------------------------ */
/* Marshalling                                                        */
/* ------------------------------------------------------------------ */

static void t_marshalling_roundtrip(void)
{
    FACE_TSS_MARSHALLING *m = face_tss_marshalling_create();
    uint8_t buf[32];
    size_t n;

    TEST_BEGIN("marshalling_roundtrip");
    CHECK(m != NULL);

    /* short */
    {
        int16_t v = -12345, out = 0;
        CHECK_RC(face_tss_marshal_short(m, v, buf, sizeof(buf), &n),
                 FACE_TSS_RC_NO_ERROR);
        CHECK(n == 2);
        CHECK_RC(face_tss_unmarshal_short(m, buf, n, &out, &n),
                 FACE_TSS_RC_NO_ERROR);
        CHECK(out == v);
    }
    /* long */
    {
        int32_t v = -123456789, out = 0;
        CHECK_RC(face_tss_marshal_long(m, v, buf, sizeof(buf), &n),
                 FACE_TSS_RC_NO_ERROR);
        CHECK(n == 4);
        CHECK_RC(face_tss_unmarshal_long(m, buf, n, &out, &n),
                 FACE_TSS_RC_NO_ERROR);
        CHECK(out == v);
    }
    /* double */
    {
        double v = 3.14159265358979, out = 0;
        CHECK_RC(face_tss_marshal_double(m, v, buf, sizeof(buf), &n),
                 FACE_TSS_RC_NO_ERROR);
        CHECK(n == 8);
        CHECK_RC(face_tss_unmarshal_double(m, buf, n, &out, &n),
                 FACE_TSS_RC_NO_ERROR);
        CHECK(out == v);
    }
    /* boolean */
    {
        int out = 0;
        CHECK_RC(face_tss_marshal_boolean(m, 1, buf, sizeof(buf), &n),
                 FACE_TSS_RC_NO_ERROR);
        CHECK(n == 1);
        CHECK_RC(face_tss_unmarshal_boolean(m, buf, n, &out, &n),
                 FACE_TSS_RC_NO_ERROR);
        CHECK(out == 1);
    }
    /* buffer too small */
    {
        int16_t v = 1;
        CHECK_RC(face_tss_marshal_short(m, v, buf, 1, &n),
                 FACE_TSS_RC_DATA_BUFFER_TOO_SMALL);
    }

    face_tss_marshalling_destroy(m);
    TEST_END();
}

/* ------------------------------------------------------------------ */
/* QoS                                                                */
/* ------------------------------------------------------------------ */

static void t_qos_policies(void)
{
    FACE_TSS_QOS *qos = face_tss_qos_create();
    int64_t val;

    TEST_BEGIN("qos_policies");
    CHECK(qos != NULL);

    /* Set and get. */
    CHECK_RC(face_tss_qos_set_policy(qos, 1, FACE_TSS_QOS_STALENESS,
                                     1000000000LL),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_qos_get_policy(qos, 1, FACE_TSS_QOS_STALENESS, &val),
             FACE_TSS_RC_NO_ERROR);
    CHECK(val == 1000000000LL);

    /* Staleness check: fresh -> NO_ERROR, stale -> MESSAGE_STALE. */
    CHECK_RC(face_tss_qos_check_staleness(qos, 1, 500000000LL),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_qos_check_staleness(qos, 1, 2000000000LL),
             FACE_TSS_RC_MESSAGE_STALE);

    /* No policy -> NO_ACTION. */
    CHECK_RC(face_tss_qos_check_staleness(qos, 2, 2000000000LL),
             FACE_TSS_RC_NO_ACTION);

    /* Clear. */
    CHECK_RC(face_tss_qos_clear_policies(qos, 1), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_qos_get_policy(qos, 1, FACE_TSS_QOS_STALENESS, &val),
             FACE_TSS_RC_NO_ACTION);

    face_tss_qos_destroy(qos);
    TEST_END();
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("[face_tss_extended]\n");
    printf("CSP, TPM, marshalling, and QoS policy tests.\n\n");

    t_csp_lifecycle();
    t_csp_crud();
    t_tpm_lifecycle();
    t_tpm_data();
    t_marshalling_roundtrip();
    t_qos_policies();

    printf("\n%d failures\n", face_tss_test_failures);
    return face_tss_test_failures != 0;
}
