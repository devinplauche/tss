#ifndef FACE_TSS_TYPED_H
#define FACE_TSS_TYPED_H

/*
 * Per-data-type FACE TSS interface (C binding).
 *
 * The FACE Technical Standard parameterizes the Transport Services as
 * FACE::TSS::Typed<DATATYPE_TYPE>::TypedTS: one TypedTS interface per
 * application data type, where the message carries a MESSAGE_GUID_TYPE
 * linking it to the data model. This header is the C projection of that
 * template: application (or generated) type support registers a data type
 * with the TSS instance, and the typed Send/Receive/Callback operations move
 * typed values instead of opaque bytes.
 *
 * Type support is normally produced by tools/face_tss_codegen.py from the
 * application's .fbs schema, which generates the serialize/deserialize/fini
 * functions plus the FACE_TSS_TYPE_SUPPORT descriptor. Hand-written type
 * support is equally valid: fill the descriptor with any codec.
 */

#include "face_tss/tss.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Codec + identity for one application data type. */
typedef struct FACE_TSS_TYPE_SUPPORT {
    FACE_TSS_MESSAGE_GUID_TYPE message_guid; /* data-model type GUID */
    char type_name[FACE_TSS_MAX_STRING];     /* e.g. "FaceTSS.PositionReport" */
    /* Serialize a typed value into malloc'd payload bytes. */
    FACE_TSS_RETURN_CODE (*serialize)(
        const void *typed_msg, uint8_t **payload_out, size_t *payload_len_out);
    /* Deserialize payload bytes into a caller-provided typed value. */
    FACE_TSS_RETURN_CODE (*deserialize)(
        const uint8_t *payload, size_t payload_len, void *typed_msg_out);
    /* Release resources owned by a typed value (after deserialize). */
    void (*fini)(void *typed_msg);
    /* sizeof() the typed value struct (for callback scratch storage). */
    size_t value_size;
    /* Largest serialized form accepted; larger sends fail. */
    size_t max_size;
} FACE_TSS_TYPE_SUPPORT;

/* Register a data type with the TSS instance (copies the descriptor).
 * NO_ACTION if type_name is already registered. */
FACE_TSS_RETURN_CODE face_tss_typed_register(
    FACE_TSS *tss, const FACE_TSS_TYPE_SUPPORT *tsupport);

/* Look up a registered type by name. Returns NULL when unknown. */
const FACE_TSS_TYPE_SUPPORT *face_tss_typed_lookup(
    FACE_TSS *tss, const char *type_name);

/* TypedTS::Send_Message for a registered data type. `*transaction_id` is
 * inout (see face_tss_send_message). */
FACE_TSS_RETURN_CODE face_tss_typed_send(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    const char *type_name, const void *typed_msg);

/* TypedTS::Receive_Message for a registered data type. The received payload
 * is deserialized into `typed_msg_out` (caller provides storage); call the
 * type's fini() when done with it. */
FACE_TSS_RETURN_CODE face_tss_typed_receive(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns, size_t min_message_size,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    const char *type_name, void *typed_msg_out,
    FACE_TSS_HEADER *header_out, FACE_TSS_QOS_EVENT *qos_out);

/* Typed Read_Callback::Callback_Handler. */
typedef void (*FACE_TSS_TYPED_CB)(
    FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_TRANSACTION_ID_TYPE transaction_id,
    const char *type_name, const void *typed_msg,
    const FACE_TSS_HEADER *header,
    const FACE_TSS_QOS_EVENT *qos,
    void *user,
    FACE_TSS_RETURN_CODE *return_code);

/* TypedTS::Register_Callback for a registered data type. */
FACE_TSS_RETURN_CODE face_tss_typed_register_callback(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    const char *type_name, FACE_TSS_TYPED_CB cb, void *user);

/* FACE::TSS::Typed::TypedTS::Unregister_Callback (FACE 3.2 location; 3.1 had
 * this operation on the Base interface). Unregisters the connection's
 * callback, whether typed or untyped - per the IDL it takes only the
 * connection ID, so type_name is validated (INVALID_PARAM when unknown)
 * but does not select among multiple callbacks. NO_ACTION when none is
 * registered. */
FACE_TSS_RETURN_CODE face_tss_typed_unregister_callback(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    const char *type_name);

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_TYPED_H */
