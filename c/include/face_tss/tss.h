#ifndef FACE_TSS_H
#define FACE_TSS_H

/*
 * FACE Transport Services Segment over nng + FlatBuffers (C binding).
 *
 * Implements the FACE TS interface shape - Initialize / Create_Connection /
 * Destroy_Connection / Send_Message / Receive_Message / Register_Callback /
 * Unregister_Callback - with data movement provided by nng sockets and
 * framing provided by FlatBuffers envelopes.
 *
 * Connection model (one FACE connection = one nng socket):
 *   SOURCE          may only Send_Message.
 *   DESTINATION     may only Receive_Message.
 *   BI_DIRECTIONAL  may do both.
 *
 * IDs: Create_Connection returns increasing ints starting at 1 (0 is
 * reserved as FACE_TSS_CONNECTION_ID_INVALID). The TSS stamps source_id
 * (random per-instance GUID), per-connection sequence numbers, and a send
 * timestamp on every outgoing envelope; transaction IDs pass through for
 * request/reply correlation.
 *
 * Threading: the handle is thread-safe; one mutex guards the connection
 * table and sequence counters.
 */

#include "face_tss/config.h"
#include "face_tss/envelope.h"
#include "face_tss/transport.h"
#include "face_tss/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FACE_TSS FACE_TSS;

typedef struct FACE_TSS_STATS {
    uint64_t sent;
    uint64_t received;
    uint64_t send_errors;
    uint64_t receive_timeouts;
    uint64_t stale_dropped;
} FACE_TSS_STATS;

typedef struct FACE_TSS_MESSAGE {
    uint8_t *payload;      /* owned bytes (malloc), NULL when empty */
    size_t payload_len;
    FACE_TSS_HEADER header;
} FACE_TSS_MESSAGE;

void face_tss_message_fini(FACE_TSS_MESSAGE *msg);

/* Create/destroy a TSS instance. */
FACE_TSS *face_tss_create(const char *instance_name);
void face_tss_destroy(FACE_TSS *tss);

/* FACE::TS::Initialize - load configuration (deep copy). Idempotent:
 * second call returns FACE_TSS_RC_NO_ACTION. */
FACE_TSS_RETURN_CODE face_tss_initialize(
    FACE_TSS *tss, const FACE_TSS_CONFIG *config);

/* FACE::TS::Create_Connection -> id + max_message_size. */
FACE_TSS_RETURN_CODE face_tss_create_connection(
    FACE_TSS *tss, const char *name,
    FACE_TSS_CONNECTION_ID_TYPE *connection_id,
    FACE_TSS_MESSAGE_SIZE_TYPE *max_message_size,
    FACE_TIMEOUT_TYPE timeout_ns);

/* FACE::TS::Destroy_Connection. Idempotent (NO_ACTION when unknown;
 * INVALID_PARAM for id 0). */
FACE_TSS_RETURN_CODE face_tss_destroy_connection(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id);

/* FACE::TS::Send_Message. Returns INVALID_MODE on receive-only
 * connections, BUFFER_TOO_SMALL when oversize. */
FACE_TSS_RETURN_CODE face_tss_send_message(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    const uint8_t *payload, size_t payload_len,
    FACE_TSS_TRANSACTION_ID_TYPE transaction_id);

/* FACE::TS::Receive_Message - wait up to timeout_ns for one message.
 * TIMED_OUT on expiry, INVALID_MODE on send-only connections,
 * BUFFER_TOO_SMALL when the payload is smaller than min_message_size. */
FACE_TSS_RETURN_CODE face_tss_receive_message(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns, size_t min_message_size,
    FACE_TSS_MESSAGE *out);

/* Non-blocking receive: NO_ERROR with *has_msg=false instead of TIMED_OUT. */
FACE_TSS_RETURN_CODE face_tss_try_receive(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_MESSAGE *out, bool *has_msg);

/* FACE::TS::Register_Callback - deliver messages on a background thread. */
typedef void (*FACE_TSS_MESSAGE_CB)(const FACE_TSS_MESSAGE *msg, void *user);
FACE_TSS_RETURN_CODE face_tss_register_callback(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_MESSAGE_CB cb, void *user);

/* FACE::TS::Unregister_Callback. NO_ACTION when none is registered. */
FACE_TSS_RETURN_CODE face_tss_unregister_callback(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id);

/* Introspection. */
FACE_TSS_RETURN_CODE face_tss_stats(
    FACE_TSS *tss, FACE_TSS_STATS *out);
FACE_TSS_GUID_TYPE face_tss_source_id(FACE_TSS *tss);
const char *face_tss_rc_str(FACE_TSS_RETURN_CODE rc);

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_H */
