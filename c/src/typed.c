/* Per-data-type FACE TSS interface. See typed.h for the contract. */

#include "face_tss/typed.h"
#include "tss_priv.h"

#include <stdlib.h>
#include <string.h>

FACE_TSS_RETURN_CODE face_tss_typed_register(
    FACE_TSS *tss, const FACE_TSS_TYPE_SUPPORT *tsupport)
{
    if (!tss || !tsupport)
        return FACE_TSS_RC_INVALID_PARAM;
    return face_tss_priv_typed_register(tss, tsupport);
}

const FACE_TSS_TYPE_SUPPORT *face_tss_typed_lookup(
    FACE_TSS *tss, const char *type_name)
{
    return face_tss_priv_typed_lookup(tss, type_name);
}

FACE_TSS_RETURN_CODE face_tss_typed_send(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    const char *type_name, const void *typed_msg)
{
    const FACE_TSS_TYPE_SUPPORT *ts;
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    FACE_TSS_RETURN_CODE rc;
    if (!tss || !type_name || !typed_msg)
        return FACE_TSS_RC_INVALID_PARAM;
    ts = face_tss_priv_typed_lookup(tss, type_name);
    if (!ts)
        return FACE_TSS_RC_INVALID_PARAM;
    rc = ts->serialize(typed_msg, &payload, &payload_len);
    if (rc != FACE_TSS_RC_NO_ERROR)
        return rc;
    if (payload_len > ts->max_size) {
        free(payload);
        return FACE_TSS_RC_DATA_BUFFER_TOO_SMALL;
    }
    rc = face_tss_priv_send_guid(tss, connection_id, timeout_ns,
                                 transaction_id, ts->message_guid,
                                 payload, payload_len);
    free(payload);
    return rc;
}

FACE_TSS_RETURN_CODE face_tss_typed_receive(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TIMEOUT_TYPE timeout_ns, size_t min_message_size,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id,
    const char *type_name, void *typed_msg_out,
    FACE_TSS_HEADER *header_out, FACE_TSS_QOS_EVENT *qos_out)
{
    const FACE_TSS_TYPE_SUPPORT *ts;
    FACE_TSS_MESSAGE msg;
    FACE_TSS_RETURN_CODE rc;
    if (!tss || !type_name || !typed_msg_out || !header_out)
        return FACE_TSS_RC_INVALID_PARAM;
    ts = face_tss_priv_typed_lookup(tss, type_name);
    if (!ts)
        return FACE_TSS_RC_INVALID_PARAM;
    memset(&msg, 0, sizeof(msg));
    rc = face_tss_receive_message(tss, connection_id, timeout_ns,
                                  min_message_size, transaction_id,
                                  &msg, qos_out);
    if (rc != FACE_TSS_RC_NO_ERROR)
        return rc;
    /* A typed sender stamps the data-model GUID; a mismatch means the peer
     * sent a different data type on this connection. */
    if (msg.message_guid != FACE_TSS_MESSAGE_GUID_UNSPECIFIED &&
        msg.message_guid != ts->message_guid) {
        face_tss_message_fini(&msg);
        return FACE_TSS_RC_INVALID_PARAM;
    }
    rc = ts->deserialize(msg.payload, msg.payload_len, typed_msg_out);
    *header_out = msg.header;
    face_tss_message_fini(&msg);
    return rc;
}

/* Bridge: untyped callback -> typed callback. */
typedef struct TYPED_CB_CTX {
    FACE_TSS_TYPE_SUPPORT ts; /* descriptor copy (pointer-stable) */
    FACE_TSS_TYPED_CB cb;
    void *user;
} TYPED_CB_CTX;

static void typed_cb_dispatch(
    FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_TRANSACTION_ID_TYPE transaction_id,
    FACE_TSS_MESSAGE_GUID_TYPE message_guid,
    const uint8_t *payload, size_t payload_len,
    const FACE_TSS_HEADER *header,
    const FACE_TSS_QOS_EVENT *qos,
    void *user,
    FACE_TSS_RETURN_CODE *return_code)
{
    TYPED_CB_CTX *ctx = (TYPED_CB_CTX *)user;
    uint8_t *value;
    FACE_TSS_RETURN_CODE rc = FACE_TSS_RC_NO_ERROR;
    if (message_guid != FACE_TSS_MESSAGE_GUID_UNSPECIFIED &&
        message_guid != ctx->ts.message_guid) {
        *return_code = FACE_TSS_RC_INVALID_PARAM;
        return;
    }
    value = (uint8_t *)calloc(1, ctx->ts.value_size);
    if (!value) {
        *return_code = FACE_TSS_RC_NOT_AVAILABLE;
        return;
    }
    rc = ctx->ts.deserialize(payload, payload_len, value);
    if (rc == FACE_TSS_RC_NO_ERROR)
        ctx->cb(connection_id, transaction_id, ctx->ts.type_name, value,
                header, qos, ctx->user, return_code);
    else
        *return_code = rc;
    ctx->ts.fini(value);
    free(value);
}

FACE_TSS_RETURN_CODE face_tss_typed_register_callback(
    FACE_TSS *tss, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    const char *type_name, FACE_TSS_TYPED_CB cb, void *user)
{
    const FACE_TSS_TYPE_SUPPORT *ts;
    TYPED_CB_CTX *ctx;
    FACE_TSS_RETURN_CODE rc;
    if (!tss || !type_name || !cb)
        return FACE_TSS_RC_INVALID_PARAM;
    ts = face_tss_priv_typed_lookup(tss, type_name);
    if (!ts)
        return FACE_TSS_RC_INVALID_PARAM;
    ctx = (TYPED_CB_CTX *)malloc(sizeof(*ctx));
    if (!ctx)
        return FACE_TSS_RC_NOT_AVAILABLE;
    ctx->ts = *ts;
    ctx->cb = cb;
    ctx->user = user;
    rc = face_tss_priv_register_callback_ex(tss, connection_id,
                                              typed_cb_dispatch, ctx, free);
    if (rc != FACE_TSS_RC_NO_ERROR)
        free(ctx);
    return rc;
}
