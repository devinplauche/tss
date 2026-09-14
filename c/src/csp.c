/* FACE 3.2 TSS CSP implementation: in-memory data store. */

#include "face_tss/csp.h"

#include <stdlib.h>
#include <string.h>

#define CSP_MAX_STORES 16
#define CSP_MAX_ENTRIES 256

typedef struct {
    FACE_TSS_CSP_DATA_ID_TYPE data_id;
    uint8_t *data;
    size_t data_len;
    int in_use;
} csp_entry_t;

typedef struct {
    char configuration_name[128];
    FACE_TSS_CSP_DATA_STORE_KIND kind;
    FACE_TSS_UID_TYPE uop_id;
    csp_entry_t entries[CSP_MAX_ENTRIES];
    int in_use;
} csp_store_t;

struct FACE_TSS_CSP {
    char name[64];
    int initialized;
    csp_store_t stores[CSP_MAX_STORES];
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE next_token;
};

FACE_TSS_CSP *face_tss_csp_create(const char *name)
{
    FACE_TSS_CSP *csp = (FACE_TSS_CSP *)calloc(1, sizeof(*csp));
    if (!csp)
        return NULL;
    if (name)
        strncpy(csp->name, name, sizeof(csp->name) - 1);
    csp->next_token = 1;
    return csp;
}

void face_tss_csp_destroy(FACE_TSS_CSP *csp)
{
    int i, j;
    if (!csp)
        return;
    for (i = 0; i < CSP_MAX_STORES; i++) {
        if (!csp->stores[i].in_use)
            continue;
        for (j = 0; j < CSP_MAX_ENTRIES; j++) {
            if (csp->stores[i].entries[j].in_use) {
                free(csp->stores[i].entries[j].data);
            }
        }
    }
    free(csp);
}

FACE_TSS_RETURN_CODE face_tss_csp_initialize(
    FACE_TSS_CSP *csp, const char *configuration_resource)
{
    if (!csp)
        return FACE_TSS_RC_INVALID_PARAM;
    (void)configuration_resource;
    if (csp->initialized)
        return FACE_TSS_RC_NO_ACTION;
    csp->initialized = 1;
    return FACE_TSS_RC_NO_ERROR;
}

static csp_store_t *find_store(FACE_TSS_CSP *csp,
                               FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE token)
{
    int i;
    for (i = 0; i < CSP_MAX_STORES; i++) {
        if (csp->stores[i].in_use &&
            (FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE)(i + 1) == token)
            return &csp->stores[i];
    }
    return NULL;
}

FACE_TSS_RETURN_CODE face_tss_csp_open(
    FACE_TSS_CSP *csp, FACE_TSS_UID_TYPE uop_id,
    const char *configuration_name,
    FACE_TSS_CSP_DATA_STORE_KIND kind,
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE *token_out)
{
    int i;
    if (!csp || !token_out)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!csp->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    if (!configuration_name)
        return FACE_TSS_RC_INVALID_PARAM;

    for (i = 0; i < CSP_MAX_STORES; i++) {
        if (!csp->stores[i].in_use) {
            csp->stores[i].in_use = 1;
            strncpy(csp->stores[i].configuration_name, configuration_name,
                    sizeof(csp->stores[i].configuration_name) - 1);
            csp->stores[i].kind = kind;
            csp->stores[i].uop_id = uop_id;
            *token_out = (FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE)(i + 1);
            return FACE_TSS_RC_NO_ERROR;
        }
    }
    return FACE_TSS_RC_RESOURCE_LIMIT_REACHED;
}

FACE_TSS_RETURN_CODE face_tss_csp_close(
    FACE_TSS_CSP *csp, FACE_TSS_UID_TYPE uop_id,
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE token)
{
    csp_store_t *s;
    int j;
    if (!csp)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!csp->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    s = find_store(csp, token);
    if (!s)
        return FACE_TSS_RC_INVALID_PARAM;
    (void)uop_id;
    for (j = 0; j < CSP_MAX_ENTRIES; j++) {
        if (s->entries[j].in_use) {
            free(s->entries[j].data);
            s->entries[j].in_use = 0;
        }
    }
    s->in_use = 0;
    return FACE_TSS_RC_NO_ERROR;
}

static csp_entry_t *find_entry(csp_store_t *s, FACE_TSS_CSP_DATA_ID_TYPE data_id)
{
    int j;
    for (j = 0; j < CSP_MAX_ENTRIES; j++) {
        if (s->entries[j].in_use && s->entries[j].data_id == data_id)
            return &s->entries[j];
    }
    return NULL;
}

FACE_TSS_RETURN_CODE face_tss_csp_create_entry(
    FACE_TSS_CSP *csp, FACE_TSS_UID_TYPE uop_id,
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE token,
    FACE_TSS_CSP_DATA_ID_TYPE data_id,
    const uint8_t *data, size_t data_len)
{
    csp_store_t *s;
    int j;
    if (!csp)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!csp->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    s = find_store(csp, token);
    if (!s)
        return FACE_TSS_RC_INVALID_PARAM;
    (void)uop_id;
    if (find_entry(s, data_id))
        return FACE_TSS_RC_INVALID_PARAM; /* already exists */

    for (j = 0; j < CSP_MAX_ENTRIES; j++) {
        if (!s->entries[j].in_use) {
            s->entries[j].in_use = 1;
            s->entries[j].data_id = data_id;
            if (data_len > 0) {
                s->entries[j].data = (uint8_t *)malloc(data_len);
                if (!s->entries[j].data) {
                    s->entries[j].in_use = 0;
                    return FACE_TSS_RC_RESOURCE_LIMIT_REACHED;
                }
                memcpy(s->entries[j].data, data, data_len);
            } else {
                s->entries[j].data = NULL;
            }
            s->entries[j].data_len = data_len;
            return FACE_TSS_RC_NO_ERROR;
        }
    }
    return FACE_TSS_RC_RESOURCE_LIMIT_REACHED;
}

FACE_TSS_RETURN_CODE face_tss_csp_read(
    FACE_TSS_CSP *csp, FACE_TSS_UID_TYPE uop_id,
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE token,
    FACE_TSS_CSP_DATA_ID_TYPE data_id,
    uint8_t *data_out, size_t *data_len_inout)
{
    csp_store_t *s;
    csp_entry_t *e;
    if (!csp || !data_out || !data_len_inout)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!csp->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    s = find_store(csp, token);
    if (!s)
        return FACE_TSS_RC_INVALID_PARAM;
    (void)uop_id;
    e = find_entry(s, data_id);
    if (!e)
        return FACE_TSS_RC_INVALID_PARAM;
    if (*data_len_inout < e->data_len) {
        *data_len_inout = e->data_len;
        return FACE_TSS_RC_DATA_BUFFER_TOO_SMALL;
    }
    if (e->data_len > 0)
        memcpy(data_out, e->data, e->data_len);
    *data_len_inout = e->data_len;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_csp_update(
    FACE_TSS_CSP *csp, FACE_TSS_UID_TYPE uop_id,
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE token,
    FACE_TSS_CSP_DATA_ID_TYPE data_id,
    const uint8_t *data, size_t data_len)
{
    csp_store_t *s;
    csp_entry_t *e;
    uint8_t *nd;
    if (!csp)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!csp->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    s = find_store(csp, token);
    if (!s)
        return FACE_TSS_RC_INVALID_PARAM;
    (void)uop_id;
    e = find_entry(s, data_id);
    if (!e)
        return FACE_TSS_RC_INVALID_PARAM;
    if (data_len > 0) {
        nd = (uint8_t *)malloc(data_len);
        if (!nd)
            return FACE_TSS_RC_RESOURCE_LIMIT_REACHED;
        memcpy(nd, data, data_len);
        free(e->data);
        e->data = nd;
    } else {
        free(e->data);
        e->data = NULL;
    }
    e->data_len = data_len;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_csp_delete(
    FACE_TSS_CSP *csp, FACE_TSS_UID_TYPE uop_id,
    FACE_TSS_CSP_DATA_STORE_TOKEN_TYPE token,
    FACE_TSS_CSP_DATA_ID_TYPE data_id)
{
    csp_store_t *s;
    csp_entry_t *e;
    if (!csp)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!csp->initialized)
        return FACE_TSS_RC_NOT_AVAILABLE;
    s = find_store(csp, token);
    if (!s)
        return FACE_TSS_RC_INVALID_PARAM;
    (void)uop_id;
    e = find_entry(s, data_id);
    if (!e)
        return FACE_TSS_RC_INVALID_PARAM;
    free(e->data);
    e->data = NULL;
    e->data_len = 0;
    e->in_use = 0;
    return FACE_TSS_RC_NO_ERROR;
}
