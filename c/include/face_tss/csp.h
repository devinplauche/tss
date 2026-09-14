/* FACE 3.2 TSS CSP (Checkpoint Service Provider) interface.
 *
 * The CSP provides a data store for checkpoint and private data, accessed
 * via tokens. This is the FACE::TSS::CSP::CSP IDL interface (FACE 3.2).
 */

#ifndef FACE_TSS_CSP_H
#define FACE_TSS_CSP_H

#include <stddef.h>
#include <stdint.h>

#include "face_tss/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FACE::TSS::CSP::DATA_STORE_KIND_TYPE */
typedef enum FACE_TSS_CSP_DATA_STORE_KIND {
    FACE_TSS_CSP_PRIVATE_DATA_STORE = 0,
    FACE_TSS_CSP_CHECKPOINT_DATA_STORE = 1
} FACE_TSS_CSP_DATA_STORE_KIND;

/* FACE::TSS::CSP::DATA_STORE_TOKEN_TYPE */
typedef int64_t FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE;
#define FACE_TSS_CSP_DATA_STORE_TOKEN_INVALID ((FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE)0)

/* FACE::TSS::CSP::DATA_ID_TYPE */
typedef int64_t FACE_TSS_CSP_DATA_ID_TYPE;

/* Opaque CSP instance. */
typedef struct FACE_TSS_CSP FACE_TSS_CSP;

/* Create/destroy a CSP instance. */
FACE_TSS_CSP *face_tss_csp_create(const char *name);
void face_tss_csp_destroy(FACE_TSS_CSP *csp);

/* FACE::TSS::CSP::CSP::Initialize. */
FACE_TSS_RETURN_CODE face_tss_csp_initialize(
    FACE_TSS_CSP *csp, const char *configuration_resource);

/* FACE::TSS::CSP::CSP::Open. Opens a data store; returns a token. */
FACE_TSS_RETURN_CODE face_tss_csp_open(
    FACE_TSS_CSP *csp, FACE_TSS_UID_TYPE uop_id,
    const char *configuration_name,
    FACE_TSS_CSP_DATA_STORE_KIND kind,
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE *token_out);

/* FACE::TSS::CSP::CSP::Close. */
FACE_TSS_RETURN_CODE face_tss_csp_close(
    FACE_TSS_CSP *csp, FACE_TSS_UID_TYPE uop_id,
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE token);

/* FACE::TSS::CSP::CSP::Create. Creates a data store entry. */
FACE_TSS_RETURN_CODE face_tss_csp_create_entry(
    FACE_TSS_CSP *csp, FACE_TSS_UID_TYPE uop_id,
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE token,
    FACE_TSS_CSP_DATA_ID_TYPE data_id,
    const uint8_t *data, size_t data_len);

/* FACE::TSS::CSP::CSP::Read. Reads a data store entry. */
FACE_TSS_RETURN_CODE face_tss_csp_read(
    FACE_TSS_CSP *csp, FACE_TSS_UID_TYPE uop_id,
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE token,
    FACE_TSS_CSP_DATA_ID_TYPE data_id,
    uint8_t *data_out, size_t *data_len_inout);

/* FACE::TSS::CSP::CSP::Update. Updates a data store entry. */
FACE_TSS_RETURN_CODE face_tss_csp_update(
    FACE_TSS_CSP *csp, FACE_TSS_UID_TYPE uop_id,
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE token,
    FACE_TSS_CSP_DATA_ID_TYPE data_id,
    const uint8_t *data, size_t data_len);

/* FACE::TSS::CSP::CSP::Delete. Deletes a data store entry. */
FACE_TSS_RETURN_CODE face_tss_csp_delete(
    FACE_TSS_CSP *csp, FACE_TSS_UID_TYPE uop_id,
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE token,
    FACE_TSS_CSP_DATA_ID_TYPE data_id);

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_CSP_H */
