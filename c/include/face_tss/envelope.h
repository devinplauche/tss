#ifndef FACE_TSS_ENVELOPE_H
#define FACE_TSS_ENVELOPE_H

/*
 * FlatBuffers TSS envelope codec (flatcc runtime).
 *
 * Wire schema (see schemas/tss_envelope.fbs); field ids are fixed:
 *
 *   0  connection_name : string
 *   1  transaction_id  : long
 *   2  source_id       : long
 *   3  sequence_number : ulong
 *   4  timestamp_ns    : long
 *   5  payload         : [ubyte]  (opaque typed message bytes)
 *   6  message_guid    : long     (FACE MESSAGE_GUID_TYPE; 0 = untyped)
 *   7  instance_uid    : long     (FACE per-message instance UID)
 *
 * The typed payload is produced/consumed by application code; the TSS only
 * frames it. `face_tss_envelope_encode` serializes with the flatcc builder
 * (linked from libflatccrt); `face_tss_envelope_decode` parses with the
 * flatcc verifier + accessors into a caller-owned copy.
 */

#include "face_tss/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FACE_TSS_ENVELOPE {
    char connection_name[FACE_TSS_MAX_CONNECTION_NAME];
    FACE_TSS_TRANSACTION_ID_TYPE transaction_id;
    FACE_TSS_UID_TYPE source_id;
    uint64_t sequence_number;
    FACE_SYSTEM_TIME_TYPE timestamp_ns;
    FACE_TSS_MESSAGE_GUID_TYPE message_guid; /* 0 = untyped payload */
    FACE_TSS_UID_TYPE instance_uid;      /* per-message instance UID */
    uint8_t *payload;      /* owned bytes (malloc), NULL when empty */
    size_t payload_len;
} FACE_TSS_ENVELOPE;

void face_tss_envelope_init(FACE_TSS_ENVELOPE *env);
void face_tss_envelope_fini(FACE_TSS_ENVELOPE *env);

/* Serialize into a malloc'd buffer (`*out`, `*out_len`, caller frees).
 * Returns FACE_TSS_RC_NO_ERROR or FACE_TSS_RC_INVALID_PARAM. */
FACE_TSS_RETURN_CODE face_tss_envelope_encode(
    const FACE_TSS_ENVELOPE *env, uint8_t **out, size_t *out_len);

/* Parse wire bytes into `out` (deep copy of name + payload).
 * Returns NO_ERROR, INVALID_PARAM (truncated/corrupt), or NOT_AVAILABLE
 * (allocation failure). */
FACE_TSS_RETURN_CODE face_tss_envelope_decode(
    const uint8_t *buf, size_t len, FACE_TSS_ENVELOPE *out);

/* Topic framing for Pub/Sub: topic = NAME + '\0'. Writes the topic into
 * `topic_out` (FACE_TSS_MAX_CONNECTION_NAME+1 bytes); returns its length. */
size_t face_tss_topic_for(
    const char *connection_name, char topic_out[FACE_TSS_MAX_CONNECTION_NAME + 1]);

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_ENVELOPE_H */
