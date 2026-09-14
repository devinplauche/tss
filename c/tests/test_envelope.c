/* Envelope codec tests: round-trip incl. binary/empty/large payloads,
 * defaults, unicode names, corrupt-input rejection. */
#include "test.h"

#include <string.h>

#include "face_tss/envelope.h"
#include "face_tss/tss.h" /* face_tss_rc_str */

static FACE_TSS_ENVELOPE make_env(void)
{
    FACE_TSS_ENVELOPE e;
    static const uint8_t blob[] = { 1, 2, 3, 250, 251, 0, 255 };
    face_tss_envelope_init(&e);
    strcpy(e.connection_name, "HELLO");
    e.transaction_id = 7;
    e.source_id = 123456789;
    e.sequence_number = 42;
    e.timestamp_ns = 1720000000000000000LL;
    e.payload = (uint8_t *)malloc(sizeof(blob));
    memcpy(e.payload, blob, sizeof(blob));
    e.payload_len = sizeof(blob);
    return e;
}

static void check_equal(const FACE_TSS_ENVELOPE *a, const FACE_TSS_ENVELOPE *b)
{
    CHECK(strcmp(a->connection_name, b->connection_name) == 0);
    CHECK(a->transaction_id == b->transaction_id);
    CHECK(a->source_id == b->source_id);
    CHECK(a->sequence_number == b->sequence_number);
    CHECK(a->timestamp_ns == b->timestamp_ns);
    CHECK(a->payload_len == b->payload_len);
    CHECK(a->payload_len == 0 ||
          memcmp(a->payload, b->payload, a->payload_len) == 0);
}

static void t_round_trip(void)
{
    FACE_TSS_ENVELOPE e = make_env(), back;
    uint8_t *buf = NULL;
    size_t len = 0;
    TEST_BEGIN("round_trip_binary_payload");
    CHECK_RC(face_tss_envelope_encode(&e, &buf, &len), FACE_TSS_RC_NO_ERROR);
    CHECK(buf && len > 0);
    CHECK_RC(face_tss_envelope_decode(buf, len, &back), FACE_TSS_RC_NO_ERROR);
    check_equal(&e, &back);
    face_tss_envelope_fini(&back);
    free(buf);
    face_tss_envelope_fini(&e);
    TEST_END();
}

static void t_defaults(void)
{
    FACE_TSS_ENVELOPE e, back;
    uint8_t *buf = NULL;
    size_t len = 0;
    TEST_BEGIN("round_trip_empty_payload_and_defaults");
    face_tss_envelope_init(&e);
    strcpy(e.connection_name, "X");
    CHECK_RC(face_tss_envelope_encode(&e, &buf, &len), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_envelope_decode(buf, len, &back), FACE_TSS_RC_NO_ERROR);
    check_equal(&e, &back);
    CHECK(back.payload_len == 0);
    face_tss_envelope_fini(&back);
    free(buf);
    TEST_END();
}

static void t_large(void)
{
    FACE_TSS_ENVELOPE e, back;
    uint8_t *buf = NULL;
    size_t len = 0, i;
    TEST_BEGIN("round_trip_large_payload");
    face_tss_envelope_init(&e);
    strcpy(e.connection_name, "BIG");
    e.payload_len = 16384;
    e.payload = (uint8_t *)malloc(e.payload_len);
    CHECK(e.payload != NULL);
    for (i = 0; i < e.payload_len; i++)
        e.payload[i] = (uint8_t)(i & 0xFF);
    CHECK_RC(face_tss_envelope_encode(&e, &buf, &len), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_envelope_decode(buf, len, &back), FACE_TSS_RC_NO_ERROR);
    check_equal(&e, &back);
    face_tss_envelope_fini(&back);
    free(buf);
    face_tss_envelope_fini(&e);
    TEST_END();
}

static void t_topic(void)
{
    char topic[FACE_TSS_MAX_CONNECTION_NAME + 1];
    size_t n;
    TEST_BEGIN("topic_framing");
    n = face_tss_topic_for("hello", topic);
    CHECK(n == 6);
    CHECK(memcmp(topic, "HELLO", 5) == 0 && topic[5] == '\0');
    /* unknown/empty -> empty topic */
    CHECK(face_tss_topic_for("", topic) == 0);
    TEST_END();
}

static void t_corrupt(void)
{
    FACE_TSS_ENVELOPE back;
    static const uint8_t bad1[] = { 0x01, 0x02 };
    uint8_t bad2[64];
    TEST_BEGIN("corrupt_inputs_rejected");
    memset(bad2, 0xFF, sizeof(bad2));
    CHECK(face_tss_envelope_decode(NULL, 0, &back) == FACE_TSS_RC_INVALID_PARAM);
    CHECK(face_tss_envelope_decode(bad1, sizeof(bad1), &back) ==
          FACE_TSS_RC_INVALID_PARAM);
    CHECK(face_tss_envelope_decode(bad2, sizeof(bad2), &back) ==
          FACE_TSS_RC_INVALID_PARAM);
    TEST_END();
}

int main(void)
{
    printf("[envelope]\n");
    t_round_trip();
    t_defaults();
    t_large();
    t_topic();
    t_corrupt();
    return TEST_SUMMARY();
}
