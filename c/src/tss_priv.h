/* Private TSS internals shared between tss.c and typed.c. Not installed. */

#ifndef FACE_TSS_PRIV_H
#define FACE_TSS_PRIV_H

#include "face_tss/typed.h"

/* Type-support registry (implemented in tss.c; locks internally). */
FACE_TSS_RETURN_CODE face_tss_priv_typed_register(
    FACE_TSS *tss, const FACE_TSS_TYPE_SUPPORT *tsupport);
const FACE_TSS_TYPE_SUPPORT *face_tss_priv_typed_lookup(
    FACE_TSS *tss, const char *type_name);

/* Send with an explicit message GUID on the envelope (implemented in tss.c;
 * face_tss_send_message is this with guid == 0). */
FACE_TSS_RETURN_CODE face_tss_priv_send_guid(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    FACE_TSS_MESSAGE_GUID_TYPE message_guid,
    const uint8_t *payload, size_t payload_len);

/* Register a callback with a cleanup for the user context (implemented in
 * tss.c; face_tss_register_callback is this with user_fini == NULL). The
 * cleanup runs on unregister/destroy. */
FACE_TSS_RETURN_CODE face_tss_priv_register_callback_ex(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_MESSAGE_CB cb, void *user, void (*user_fini)(void *));

#endif /* FACE_TSS_PRIV_H */
