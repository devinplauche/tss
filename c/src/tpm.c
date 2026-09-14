/* FACE 3.2 TSS TPM implementation: channel-based transport. */

#include "face_tss/tpm.h"

#include <stdlib.h>
#include <string.h>

#define TPM_MAX_CHANNELS 16
#define TPM_MAX_MSG 65536

typedef struct {
    char endpoint_name[128];
    int in_use;
    int open;
    /* Simple message queue for the channel. */
    uint8_t *pending_msg;
    size_t pending_len;
    FACE_TSS_TRANSACTION_ID_TYPE pending_txn;
    /* Callbacks. */
    FACE_TSS_TPM_DATA_CB data_cb;
    FACE_TSS_TPM_EVENT_CB event_cb;
    void *cb_user;
    FACE_TSS_TPM_CALLBACK_KIND cb_kind;
    int cb_registered;
} tpm_channel_t;

struct FACE_TSS_TPM {
    char name[64];
    int initialized;
    FACE_TSS_TPM_STATE_TYPE state;
    FACE_TSS_TPM_EVENT_TYPE status;
    tpm_channel_t channels[TPM_MAX_CHANNELS];
    FACE_TSS_TPM_CHANNEL_ID_TYPE next_channel_id;
};

FACE_TSS_TPM *face_tss_tpm_create(const char *name)
{
    FACE_TSS_TPM *tpm = (FACE_TSS_TPM *)calloc(1, sizeof(*tpm));
    if (!tpm)
        return NULL;
    if (name)
        strncpy(tpm->name, name, sizeof(tpm->name) - 1);
    tpm->state = FACE_TSS_TPM_STATE_NORMAL;
    tpm->status = FACE_TSS_TPM_INIT_COMPLETE;
    tpm->next_channel_id = 1;
    return tpm;
}

void face_tss_tpm_destroy(FACE_TSS_TPM *tpm)
{
    int i;
    if (!tpm)
        return;
    for (i = 0; i < TPM_MAX_CHANNELS; i++) {
        free(tpm->channels[i].pending_msg);
    }
    free(tpm);
}

FACE_TSS_RETURN_CODE face_tss_tpm_initialize(
    FACE_TSS_TPM *tpm, const char *configuration_resource)
{
    if (!tpm)
        return FACE_TSS_RC_INVALID_PARAM;
    (void)configuration_resource;
    if (tpm->initialized)
        return FACE_TSS_RC_NO_ACTION;
    tpm->initialized = 1;
    tpm->status = FACE_TSS_TPM_INIT_COMPLETE;
    return FACE_TSS_RC_NO_ERROR;
}

static tpm_channel_t *find_channel(FACE_TSS_TPM *tpm,
                                   FACE_TSS_TPM_CHANNEL_ID_TYPE id)
{
    int i;
    for (i = 0; i < TPM_MAX_CHANNELS; i++) {
        if (tpm->channels[i].in_use &&
            (FACE_TSS_TPM_CHANNEL_ID_TYPE)(i + 1) == id)
            return &tpm->channels[i];
    }
    return NULL;
}

FACE_TSS_RETURN_CODE face_tss_tpm_open_channel(
    FACE_TSS_TPM *tpm, const char *endpoint_name,
    const uint8_t *transport_config, size_t transport_config_len,
    const uint8_t *security_config, size_t security_config_len,
    FACE_TSS_TPM_CHANNEL_ID_TYPE *channel_id_out)
{
    int i;
    if (!tpm || !channel_id_out)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    if (!endpoint_name)
        return FACE_TSS_RC_INVALID_PARAM;
    (void)transport_config;
    (void)transport_config_len;
    (void)security_config;
    (void)security_config_len;

    if (tpm->state == FACE_TSS_TPM_STATE_SHUTDOWN)
        return FACE_TSS_RC_INVALID_MODE;

    for (i = 0; i < TPM_MAX_CHANNELS; i++) {
        if (!tpm->channels[i].in_use) {
            tpm->channels[i].in_use = 1;
            tpm->channels[i].open = 1;
            strncpy(tpm->channels[i].endpoint_name, endpoint_name,
                    sizeof(tpm->channels[i].endpoint_name) - 1);
            *channel_id_out = (FACE_TSS_TPM_CHANNEL_ID_TYPE)(i + 1);
            return FACE_TSS_RC_NO_ERROR;
        }
    }
    return FACE_TSS_RC_RESOURCE_LIMIT_REACHED;
}

FACE_TSS_RETURN_CODE face_tss_tpm_close_channel(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id)
{
    tpm_channel_t *ch;
    if (!tpm)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    ch = find_channel(tpm, channel_id);
    if (!ch)
        return FACE_TSS_RC_INVALID_PARAM;
    free(ch->pending_msg);
    ch->pending_msg = NULL;
    ch->pending_len = 0;
    ch->in_use = 0;
    ch->open = 0;
    ch->cb_registered = 0;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_tpm_request_state_change(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_STATE_TYPE new_state,
    const void *data, size_t data_len)
{
    if (!tpm)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    (void)data;
    (void)data_len;
    /* Validate state transition. */
    switch (new_state) {
    case FACE_TSS_TPM_STATE_NORMAL:
    case FACE_TSS_TPM_STATE_TEST:
    case FACE_TSS_TPM_STATE_RESUME:
    case FACE_TSS_TPM_STATE_PAUSE:
    case FACE_TSS_TPM_STATE_SHUTDOWN:
    case FACE_TSS_TPM_STATE_SECURE:
        break;
    default:
        return FACE_TSS_RC_INVALID_PARAM;
    }
    tpm->state = new_state;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_tpm_is_data_available(
    FACE_TSS_TPM *tpm,
    const FACE_TSS_TPM_CHANNEL_ID_TYPE *channel_ids, size_t channel_count,
    FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_TPM_CHANNEL_ID_TYPE *available_out, size_t *available_count_inout)
{
    size_t n = 0;
    size_t i;
    (void)timeout_ns; /* non-blocking check in this implementation */
    if (!tpm || !available_out || !available_count_inout)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    for (i = 0; i < channel_count && n < *available_count_inout; i++) {
        tpm_channel_t *ch = find_channel(tpm, channel_ids[i]);
        if (ch && ch->pending_msg) {
            available_out[n++] = channel_ids[i];
        }
    }
    *available_count_inout = n;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_tpm_get_status(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_EVENT_TYPE *status_out)
{
    if (!tpm || !status_out)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    *status_out = tpm->status;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_tpm_read_from_transport(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id,
    FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id_out,
    uint8_t *message_out, size_t *message_len_inout)
{
    tpm_channel_t *ch;
    if (!tpm || !transaction_id_out || !message_out || !message_len_inout)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    ch = find_channel(tpm, channel_id);
    if (!ch)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!ch->open)
        return FACE_TSS_RC_CONNECTION_CLOSED;
    (void)timeout_ns;
    if (!ch->pending_msg)
        return FACE_TSS_RC_TIMED_OUT;
    if (*message_len_inout < ch->pending_len) {
        *message_len_inout = ch->pending_len;
        return FACE_TSS_RC_DATA_BUFFER_TOO_SMALL;
    }
    memcpy(message_out, ch->pending_msg, ch->pending_len);
    *message_len_inout = ch->pending_len;
    *transaction_id_out = ch->pending_txn;
    free(ch->pending_msg);
    ch->pending_msg = NULL;
    ch->pending_len = 0;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_tpm_write_to_transport(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id,
    FACE_TIMEOUT_TYPE max_delay_ns,
    const uint8_t *message, size_t message_len,
    FACE_TSS_TRANSACTION_ID_TYPE transaction_id)
{
    tpm_channel_t *ch;
    uint8_t *buf;
    if (!tpm)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    ch = find_channel(tpm, channel_id);
    if (!ch)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!ch->open)
        return FACE_TSS_RC_CONNECTION_CLOSED;
    (void)max_delay_ns;
    if (message_len > TPM_MAX_MSG)
        return FACE_TSS_RC_DATA_OVERFLOW;
    if (message_len > 0 && !message)
        return FACE_TSS_RC_INVALID_PARAM;
    /* Loopback: deliver to the channel's pending queue (for testing). */
    buf = (uint8_t *)malloc(message_len ? message_len : 1);
    if (!buf)
        return FACE_TSS_RC_RESOURCE_LIMIT_REACHED;
    if (message_len > 0)
        memcpy(buf, message, message_len);
    free(ch->pending_msg);
    ch->pending_msg = buf;
    ch->pending_len = message_len;
    ch->pending_txn = transaction_id;
    /* Fire data callback if registered. */
    if (ch->cb_registered && ch->data_cb &&
        (ch->cb_kind == FACE_TSS_TPM_CALLBACK_DATA ||
         ch->cb_kind == FACE_TSS_TPM_CALLBACK_BOTH)) {
        FACE_TSS_RETURN_CODE rc = FACE_TSS_RC_NO_ERROR;
        ch->data_cb(channel_id, transaction_id, message, message_len,
                    ch->cb_user, &rc);
    }
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_tpm_register_callback(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id,
    FACE_TSS_TPM_CALLBACK_KIND kind,
    FACE_TSS_TPM_DATA_CB data_cb, FACE_TSS_TPM_EVENT_CB event_cb,
    void *user)
{
    tpm_channel_t *ch;
    if (!tpm)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    ch = find_channel(tpm, channel_id);
    if (!ch)
        return FACE_TSS_RC_INVALID_PARAM;
    if (ch->cb_registered)
        return FACE_TSS_RC_NO_ACTION;
    ch->data_cb = data_cb;
    ch->event_cb = event_cb;
    ch->cb_user = user;
    ch->cb_kind = kind;
    ch->cb_registered = 1;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_tpm_unregister_callback(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id,
    FACE_TSS_TPM_CALLBACK_KIND kind)
{
    tpm_channel_t *ch;
    if (!tpm)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!tpm->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    ch = find_channel(tpm, channel_id);
    if (!ch)
        return FACE_TSS_RC_INVALID_PARAM;
    (void)kind;
    if (!ch->cb_registered)
        return FACE_TSS_RC_NO_ACTION;
    ch->cb_registered = 0;
    ch->data_cb = NULL;
    ch->event_cb = NULL;
    return FACE_TSS_RC_NO_ERROR;
}
