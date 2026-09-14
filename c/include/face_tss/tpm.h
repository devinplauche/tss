/* FACE 3.2 TSS TPM (Transport Protocol Module) interface.
 *
 * The TPM provides a lower-level channel-based transport interface.
 * This is the FACE::TSS::TPM::TPMTS IDL interface (FACE 3.2).
 */

#ifndef FACE_TSS_TPM_H
#define FACE_TSS_TPM_H

#include <stddef.h>
#include <stdint.h>

#include "face_tss/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FACE::TSS::TPM::CHANNEL_ID_TYPE */
typedef FACE_TSS_UID_TYPE FACE_TSS_TPM_CHANNEL_ID_TYPE;
#define FACE_TSS_TPM_CHANNEL_ID_INVALID ((FACE_TSS_TPM_CHANNEL_ID_TYPE)0)

/* FACE::TSS::TPM::EVENT_TYPE */
typedef enum FACE_TSS_TPM_EVENT_TYPE {
    FACE_TSS_TPM_INIT_COMPLETE = 0,
    FACE_TSS_TPM_XPORT_DEGRADED = 1,
    FACE_TSS_TPM_CBIT_FAIL = 2,
    FACE_TSS_TPM_IBIT_FAIL = 3,
    FACE_TSS_TPM_CHANNEL_FAIL = 4,
    FACE_TSS_TPM_LOST_LINK = 5,
    FACE_TSS_TPM_TRANSMIT_COMPLETE = 6
} FACE_TSS_TPM_EVENT_TYPE;

/* FACE::TSS::TPM::TPMTS::TPM_STATE_TYPE */
typedef enum FACE_TSS_TPM_STATE_TYPE {
    FACE_TSS_TPM_STATE_NORMAL = 0,
    FACE_TSS_TPM_STATE_TEST = 1,
    FACE_TSS_TPM_STATE_RESUME = 2,
    FACE_TSS_TPM_STATE_PAUSE = 3,
    FACE_TSS_TPM_STATE_SHUTDOWN = 4,
    FACE_TSS_TPM_STATE_SECURE = 5
} FACE_TSS_TPM_STATE_TYPE;

/* FACE::TSS::TPM::TPMTS::LEVEL_OF_TEST_TYPE */
typedef enum FACE_TSS_TPM_LEVEL_OF_TEST_TYPE {
    FACE_TSS_TPM_CBIT = 0,
    FACE_TSS_TPM_IBIT = 1,
    FACE_TSS_TPM_PBIT = 2
} FACE_TSS_TPM_LEVEL_OF_TEST_TYPE;

/* FACE::TSS::TPM::TPM_Callback::CALLBACK_KIND_TYPE */
typedef enum FACE_TSS_TPM_CALLBACK_KIND {
    FACE_TSS_TPM_CALLBACK_DATA = 0,
    FACE_TSS_TPM_CALLBACK_EVENT = 1,
    FACE_TSS_TPM_CALLBACK_BOTH = 2
} FACE_TSS_TPM_CALLBACK_KIND;

/* Callback handlers. */
typedef void (*FACE_TSS_TPM_DATA_CB)(
    FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id,
    FACE_TSS_TRANSACTION_ID_TYPE transaction_id,
    const uint8_t *message, size_t message_len,
    void *user,
    FACE_TSS_RETURN_CODE *return_code);

typedef void (*FACE_TSS_TPM_EVENT_CB)(
    FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id,
    FACE_TSS_TRANSACTION_ID_TYPE transaction_id,
    FACE_TSS_TPM_EVENT_TYPE event,
    int32_t event_code,
    const char *diagnostic_msg,
    void *user,
    FACE_TSS_RETURN_CODE *return_code);

/* Opaque TPM instance. */
typedef struct FACE_TSS_TPM FACE_TSS_TPM;

FACE_TSS_TPM *face_tss_tpm_create(const char *name);
void face_tss_tpm_destroy(FACE_TSS_TPM *tpm);

/* FACE::TSS::TPM::TPMTS::Initialize. */
FACE_TSS_RETURN_CODE face_tss_tpm_initialize(
    FACE_TSS_TPM *tpm, const char *configuration_resource);

/* FACE::TSS::TPM::TPMTS::Open_Channel. */
FACE_TSS_RETURN_CODE face_tss_tpm_open_channel(
    FACE_TSS_TPM *tpm, const char *endpoint_name,
    const uint8_t *transport_config, size_t transport_config_len,
    const uint8_t *security_config, size_t security_config_len,
    FACE_TSS_TPM_CHANNEL_ID_TYPE *channel_id_out);

/* FACE::TSS::TPM::TPMTS::Close_Channel. */
FACE_TSS_RETURN_CODE face_tss_tpm_close_channel(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id);

/* FACE::TSS::TPM::TPMTS::Request_TPM_State_Change. */
FACE_TSS_RETURN_CODE face_tss_tpm_request_state_change(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_STATE_TYPE new_state,
    const void *data, size_t data_len);

/* FACE::TSS::TPM::TPMTS::Is_Data_Available. */
FACE_TSS_RETURN_CODE face_tss_tpm_is_data_available(
    FACE_TSS_TPM *tpm,
    const FACE_TSS_TPM_CHANNEL_ID_TYPE *channel_ids, size_t channel_count,
    FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_TPM_CHANNEL_ID_TYPE *available_out, size_t *available_count_inout);

/* FACE::TSS::TPM::TPMTS::Get_TPM_Status. */
FACE_TSS_RETURN_CODE face_tss_tpm_get_status(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_EVENT_TYPE *status_out);

/* FACE::TSS::TPM::TPMTS::Read_From_Transport. */
FACE_TSS_RETURN_CODE face_tss_tpm_read_from_transport(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id,
    FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_TRANSACTION_ID_TYPE *transaction_id_out,
    uint8_t *message_out, size_t *message_len_inout);

/* FACE::TSS::TPM::TPMTS::Write_To_Transport. */
FACE_TSS_RETURN_CODE face_tss_tpm_write_to_transport(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id,
    FACE_TIMEOUT_TYPE max_delay_ns,
    const uint8_t *message, size_t message_len,
    FACE_TSS_TRANSACTION_ID_TYPE transaction_id);

/* FACE::TSS::TPM::TPMTS::Register_TPM_Callback. */
FACE_TSS_RETURN_CODE face_tss_tpm_register_callback(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id,
    FACE_TSS_TPM_CALLBACK_KIND kind,
    FACE_TSS_TPM_DATA_CB data_cb, FACE_TSS_TPM_EVENT_CB event_cb,
    void *user);

/* FACE::TSS::TPM::TPMTS::Unregister_TPM_Callback. */
FACE_TSS_RETURN_CODE face_tss_tpm_unregister_callback(
    FACE_TSS_TPM *tpm, FACE_TSS_TPM_CHANNEL_ID_TYPE channel_id,
    FACE_TSS_TPM_CALLBACK_KIND kind);

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_TPM_H */
