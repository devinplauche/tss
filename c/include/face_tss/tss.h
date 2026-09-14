#ifndef FACE_TSS_H
#define FACE_TSS_H

/*
 * FACE Transport Services Segment over nng + FlatBuffers (C binding).
 *
 * Implements the FACE Technical Standard, Edition 3.1, Transport Services
 * interfaces (FACE/TSS/Base.idl, FACE/TSS/Typed.idl):
 *
 *   Base interface  - Initialize / Create_Connection / Destroy_Connection /
 *                     Unregister_Callback
 *   Typed interface - Send_Message / Receive_Message / Register_Callback
 *                     (+ Read_Callback::Callback_Handler)
 *
 * C mapping notes:
 * - IDL `out`/`inout` parameters become pointer out-params; the return code
 *   is the C function return value (equivalent to the IDL's out
 *   RETURN_CODE_TYPE).
 * - FACE::TSS::TypedTS<DATATYPE> is projected two ways: the untyped
 *   Send_Message/Receive_Message below move opaque payload bytes (the
 *   historical shape of this library), while face_tss/typed.h provides the
 *   per-data-type TypedTS projection over registered type support.
 * - The standard's per-receive/per-callback QoS_EVENT_TYPE is plumbed
 *   through; this implementation reports no QoS policies yet, so the event
 *   is always empty (count == 0).
 * - Read_Callback carries a `void *user` context as a C-idiom extension.
 *
 * Connection model (one FACE connection = one nng socket):
 *   SOURCE          may only Send_Message.
 *   DESTINATION     may only Receive_Message.
 *   BI_DIRECTIONAL  may do both.
 *
 * IDs: Create_Connection returns increasing ints starting at 1 (0 is
 * reserved as FACE_TSS_CONNECTION_ID_INVALID). The TSS stamps source_uid
 * (random per-instance UID), per-message instance_uid, and a send timestamp
 * on every outgoing envelope; transaction IDs pass through for request/reply
 * correlation (a sender may pass TRANSACTION_ID_UNSPECIFIED and the TSS
 * assigns one).
 *
 * Threading: the handle is thread-safe; one mutex guards the connection
 * table and UID counters.
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

/* Received message: opaque payload bytes plus the standard FACE header.
 * (C projection of the IDL `inout DATATYPE message, out HEADER_TYPE header`
 * pair; see typed.h for the per-data-type projection.) */
typedef struct FACE_TSS_MESSAGE {
    uint8_t *payload;      /* owned bytes (malloc), NULL when empty */
    size_t payload_len;
    FACE_TSS_MESSAGE_GUID_TYPE message_guid; /* data-model type GUID, 0=untyped */
    FACE_TSS_HEADER header;
} FACE_TSS_MESSAGE;

void face_tss_message_fini(FACE_TSS_MESSAGE *msg);

/* Create/destroy a TSS instance. */
FACE_TSS *face_tss_create(const char *instance_name);
void face_tss_destroy(FACE_TSS *tss);

/* FACE::TSS::Base::Initialize - load configuration (deep copy). Idempotent:
 * second call returns FACE_TSS_RC_NO_ACTION. */
FACE_TSS_RETURN_CODE face_tss_initialize(
    FACE_TSS *tss, const FACE_TSS_CONFIG *config);

/* FACE::TSS::Base::Create_Connection. `timeout_ns` bounds the blocking time
 * of the call itself. */
FACE_TSS_RETURN_CODE face_tss_create_connection(
    FACE_TSS *tss, const char *name,
    FACE_TSS_CONNECTION_ID_TYPE *connection_id,
    FACE_TSS_MESSAGE_SIZE_TYPE *max_message_size,
    FACE_TIMEOUT_TYPE timeout_ns);

/* FACE::TSS::Base::Destroy_Connection. Idempotent (NO_ACTION when unknown;
 * INVALID_PARAM for id 0). */
FACE_TSS_RETURN_CODE face_tss_destroy_connection(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id);

/* FACE::TSS::TypedTS::Send_Message. `timeout_ns` bounds the blocking time of
 * the send. `*transaction_id` is inout: pass
 * FACE_TSS_TRANSACTION_ID_UNSPECIFIED to have the TSS assign one; the
 * assigned value is written back.
 * Returns INVALID_MODE on receive-only connections,
 * DATA_BUFFER_TOO_SMALL when oversize. */
FACE_TSS_RETURN_CODE face_tss_send_message(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    const uint8_t *payload, size_t payload_len);

/* FACE::TSS::TypedTS::Receive_Message - wait up to timeout_ns for one
 * message. `*transaction_id` is inout: the received transaction id is
 * written back. `qos` receives the QoS events for this message (currently
 * always empty; may be NULL).
 * TIMED_OUT on expiry, INVALID_MODE on send-only connections,
 * DATA_BUFFER_TOO_SMALL when the payload is smaller than min_message_size. */
FACE_TSS_RETURN_CODE face_tss_receive_message(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns, size_t min_message_size,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    FACE_TSS_MESSAGE *msg_out,
    FACE_TSS_QOS_EVENT *qos_out);

/* FACE::TSS::TypedTS::Receive_Message with a caller-owned data buffer.
 * The payload is copied into `buffer` (capacity `buffer_capacity`) instead
 * of being allocated. On success `*payload_len_out` is the payload size and
 * the header/guid/qos outputs are filled (may be NULL).
 * DATA_BUFFER_TOO_SMALL when the payload does not fit: `*payload_len_out`
 * is set to the required size and the message is discarded.
 * TIMED_OUT on expiry, INVALID_MODE on send-only connections,
 * DATA_BUFFER_TOO_SMALL when the payload is smaller than min_message_size. */
FACE_TSS_RETURN_CODE face_tss_receive_message_into(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns, size_t min_message_size,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    uint8_t *buffer, size_t buffer_capacity, size_t *payload_len_out,
    FACE_TSS_MESSAGE_GUID_TYPE *message_guid_out,
    FACE_TSS_HEADER *header_out,
    FACE_TSS_QOS_EVENT *qos_out);

/* Non-blocking receive: NO_ERROR with *has_msg=false instead of TIMED_OUT.
 * (Convenience extension; not in the FACE IDL.) */
FACE_TSS_RETURN_CODE face_tss_try_receive(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    FACE_TSS_MESSAGE *msg_out, FACE_TSS_QOS_EVENT *qos_out,
    bool *has_msg);

/* FACE::TSS::TypedTS::Read_Callback::Callback_Handler, C projection. The
 * callback sets *return_code (NO_ERROR when the message was consumed). */
typedef void (*FACE_TSS_MESSAGE_CB)(
    FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_TRANSACTION_ID_TYPE transaction_id,
    FACE_TSS_MESSAGE_GUID_TYPE message_guid,
    const uint8_t *payload, size_t payload_len,
    const FACE_TSS_HEADER *header,
    const FACE_TSS_QOS_EVENT *qos,
    void *user,
    FACE_TSS_RETURN_CODE *return_code);

/* FACE::TSS::TypedTS::Register_Callback - deliver messages on a background
 * thread. */
FACE_TSS_RETURN_CODE face_tss_register_callback(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_MESSAGE_CB cb, void *user);

/* FACE::TSS::Base::Unregister_Callback. NO_ACTION when none is registered. */
FACE_TSS_RETURN_CODE face_tss_unregister_callback(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id);

/* Introspection (extensions; not in the FACE IDL). */
FACE_TSS_RETURN_CODE face_tss_stats(
    FACE_TSS *tss, FACE_TSS_STATS *out);
FACE_TSS_UID_TYPE face_tss_source_id(FACE_TSS *tss);
const char *face_tss_rc_str(FACE_TSS_RETURN_CODE rc);

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_H */
