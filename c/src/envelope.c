/* FACE TSS envelope codec over the flatcc runtime. See envelope.h.
 *
 * Encoding uses the flatcc builder API directly (table_add / create_string
 * / create_vector / end_table / finalize_buffer) so no generated code is
 * needed. Decoding uses the raw FlatBuffers object layout with endian-safe
 * memcpy reads plus `flatcc_verify_table_as_root` against a hand-written
 * table verifier that only accepts the six known fields with their exact
 * scalar widths.
 */

#include "face_tss/envelope.h"

#include <stdlib.h>
#include <string.h>

#include <flatcc/flatcc_builder.h>
#include <flatcc/flatcc_verifier.h>

#define F_CONN 0
#define F_TXN 1
#define F_SRC 2
#define F_SEQ 3
#define F_TS 4
#define F_PAYLOAD 5

void face_tss_envelope_init(FACE_TSS_ENVELOPE *env)
{
    if (env)
        memset(env, 0, sizeof(*env));
}

void face_tss_envelope_fini(FACE_TSS_ENVELOPE *env)
{
    if (!env)
        return;
    free(env->payload);
    env->payload = NULL;
    env->payload_len = 0;
}

size_t face_tss_topic_for(
    const char *connection_name, char topic_out[FACE_TSS_MAX_CONNECTION_NAME + 1])
{
    char norm[FACE_TSS_MAX_CONNECTION_NAME];
    size_t n;
    if (!connection_name ||
        face_tss_normalize_name(connection_name, norm) != FACE_TSS_RC_NO_ERROR) {
        topic_out[0] = '\0';
        return 0;
    }
    n = strlen(norm);
    memcpy(topic_out, norm, n);
    topic_out[n] = '\0';
    topic_out[n + 1] = '\0';
    return n + 1; /* includes the NUL separator, not the terminator */
}

FACE_TSS_RETURN_CODE face_tss_envelope_encode(
    const FACE_TSS_ENVELOPE *env, uint8_t **out, size_t *out_len)
{
    flatcc_builder_t builder;
    flatcc_builder_t *B = &builder;
    flatcc_builder_ref_t name_ref = 0, payload_ref = 0, root;
    flatcc_builder_ref_t *pref;
    int64_t *pi64;
    uint64_t *pu64;
    void *buf;
    size_t size;
    char norm[FACE_TSS_MAX_CONNECTION_NAME];

    if (!env || !out || !out_len)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!env->connection_name[0])
        return FACE_TSS_RC_INVALID_PARAM;
    if (face_tss_normalize_name(env->connection_name, norm) != FACE_TSS_RC_NO_ERROR)
        return FACE_TSS_RC_INVALID_PARAM;
    if (env->payload_len > 0 && !env->payload)
        return FACE_TSS_RC_INVALID_PARAM;

    flatcc_builder_init(B);

    /* Create leaf objects first (may be done before/without open table). */
    name_ref = flatcc_builder_create_string_str(B, norm);
    if (!name_ref)
        goto oom;
    payload_ref = flatcc_builder_create_vector(
        B, env->payload, env->payload_len, 1, 1,
        FLATBUFFERS_COUNT_MAX(1));
    if (!payload_ref)
        goto oom;

    if (flatcc_builder_start_table(B, 6))
        goto oom;

    /* string field 0 */
    pref = flatcc_builder_table_add_offset(B, F_CONN);
    if (!pref)
        goto oom_table;
    *pref = name_ref;

    /* int64 fields 1, 2, 4 (added only when nonzero: default-aware reader) */
    if (env->transaction_id != 0) {
        pi64 = (int64_t *)flatcc_builder_table_add(B, F_TXN, 8, 8);
        if (!pi64)
            goto oom_table;
        *pi64 = flatbuffers_int64_to_pe((flatbuffers_int64_t)env->transaction_id);
    }
    if (env->source_id != 0) {
        pi64 = (int64_t *)flatcc_builder_table_add(B, F_SRC, 8, 8);
        if (!pi64)
            goto oom_table;
        *pi64 = flatbuffers_int64_to_pe((flatbuffers_int64_t)env->source_id);
    }
    if (env->sequence_number != 0) {
        pu64 = (uint64_t *)flatcc_builder_table_add(B, F_SEQ, 8, 8);
        if (!pu64)
            goto oom_table;
        *pu64 = flatbuffers_uint64_to_pe((flatbuffers_uint64_t)env->sequence_number);
    }
    if (env->timestamp_ns != 0) {
        pi64 = (int64_t *)flatcc_builder_table_add(B, F_TS, 8, 8);
        if (!pi64)
            goto oom_table;
        *pi64 = flatbuffers_int64_to_pe((flatbuffers_int64_t)env->timestamp_ns);
    }

    /* ubyte vector field 5 */
    pref = flatcc_builder_table_add_offset(B, F_PAYLOAD);
    if (!pref)
        goto oom_table;
    *pref = payload_ref;

    root = flatcc_builder_end_table(B);
    if (!root)
        goto oom_table;
    if (flatcc_builder_start_buffer(B, 0, 0, 0))
        goto oom_table;
    if (!flatcc_builder_end_buffer(B, root)) {
        flatcc_builder_clear(B);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    buf = flatcc_builder_finalize_buffer(B, &size);
    flatcc_builder_clear(B);
    if (!buf)
        return FACE_TSS_RC_NOT_AVAILABLE;
    *out = (uint8_t *)buf; /* flatcc_builder_free-compatible (malloc) */
    *out_len = size;
    return FACE_TSS_RC_NO_ERROR;

oom_table:
    flatcc_builder_clear(B);
    return FACE_TSS_RC_NOT_AVAILABLE;
oom:
    flatcc_builder_clear(B);
    return FACE_TSS_RC_NOT_AVAILABLE;
}

/* ------------------------------------------------------------------ */
/* Decode: raw layout reads + verifier                                */
/* ------------------------------------------------------------------ */

static uint16_t rd_u16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return flatbuffers_uint16_from_pe(v);
}

static uint32_t rd_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return flatbuffers_uint32_from_pe(v);
}

static int32_t rd_s32(const uint8_t *p)
{
    int32_t v;
    memcpy(&v, p, 4);
    return (int32_t)flatbuffers_uint32_from_pe((uint32_t)v);
}

static int64_t rd_s64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return (int64_t)flatbuffers_uint64_from_pe(v);
}

static uint64_t rd_u64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return flatbuffers_uint64_from_pe(v);
}

/* Locate field `id` in table at `table` (offsets relative to buf start).
 * Returns 1 + sets *field_off (absolute offset of the slot) when present. */
static int find_field(const uint8_t *buf, size_t len, uint32_t table,
                      int id, uint32_t *field_off)
{
    uint32_t vtable, vsize, tsize;
    uint16_t entry;
    if (table + 4 > len)
        return 0;
    vtable = table - (uint32_t)rd_s32(buf + table);
    if (vtable + 4 > len)
        return 0;
    vsize = rd_u16(buf + vtable);
    tsize = rd_u16(buf + vtable + 2);
    if ((uint32_t)(4 + 2 * id) + 2 > vsize)
        return 0;
    if (table + tsize > len)
        return 0;
    entry = rd_u16(buf + vtable + 4 + 2 * id);
    if (entry == 0)
        return 0;
    if ((uint32_t)entry + 8 > tsize)
        return 0; /* scalar width guard for our fixed fields */
    *field_off = table + entry;
    return 1;
}

/* Table verifier: accept only fields 0..5 with exact widths. */
static int envelope_table_verify(flatcc_table_verifier_descriptor_t *td)
{
    static const uint8_t is_offset[6] = { 1, 0, 0, 0, 0, 1 };
    static const uint8_t width[6] = { 4, 8, 8, 8, 8, 4 };
    int i;
    for (i = 0; i < 6; i++) {
        uint16_t e = 0;
        if ((uint32_t)(4 + 2 * i) + 2 <= td->vsize)
            memcpy(&e, (const uint8_t *)td->vtable + 4 + 2 * i, 2);
        e = flatbuffers_uint16_from_pe(e);
        if (e == 0)
            continue;
        if ((uint32_t)e + width[i] > td->tsize)
            return flatcc_verify_error_table_field_out_of_range;
        if (!is_offset[i] && ((td->table + e) & (width[i] - 1)) != 0)
            return flatcc_verify_error_table_field_not_aligned;
        if (is_offset[i]) {
            uint32_t at = td->table + e;
            uint32_t off;
            if (at + 4 > td->end)
                return flatcc_verify_error_table_field_out_of_range;
            memcpy(&off, (const uint8_t *)td->buf + at, 4);
            off = flatbuffers_uint32_from_pe(off);
            if (off > td->end - at)
                return flatcc_verify_error_table_field_out_of_range;
        }
    }
    return flatcc_verify_ok;
}

FACE_TSS_RETURN_CODE face_tss_envelope_decode(
    const uint8_t *buf, size_t len, FACE_TSS_ENVELOPE *out)
{
    uint32_t root, table, at, off, n, start;
    uint32_t foff;
    FACE_TSS_ENVELOPE tmp;

    if (!buf || len < 4 || !out)
        return FACE_TSS_RC_INVALID_PARAM;
    if (len > FLATBUFFERS_UOFFSET_MAX)
        return FACE_TSS_RC_INVALID_PARAM;
    if (flatcc_verify_table_as_root(buf, len, 0, envelope_table_verify) !=
        flatcc_verify_ok)
        return FACE_TSS_RC_INVALID_PARAM;

    root = rd_u32(buf);
    if (root >= len)
        return FACE_TSS_RC_INVALID_PARAM;
    table = root;

    face_tss_envelope_init(&tmp);

    /* field 0: required string */
    if (!find_field(buf, len, table, F_CONN, &foff))
        return FACE_TSS_RC_INVALID_PARAM;
    at = foff;
    if (at + 4 > len)
        return FACE_TSS_RC_INVALID_PARAM;
    off = rd_u32(buf + at);
    at += off;
    if (at + 4 > len)
        return FACE_TSS_RC_INVALID_PARAM;
    n = rd_u32(buf + at);
    start = at + 4;
    if (n >= FACE_TSS_MAX_CONNECTION_NAME || start + n + 1 > len)
        return FACE_TSS_RC_INVALID_PARAM;
    if (buf[start + n] != '\0')
        return FACE_TSS_RC_INVALID_PARAM;
    memcpy(tmp.connection_name, buf + start, n);
    tmp.connection_name[n] = '\0';

    /* scalar fields with defaults */
    if (find_field(buf, len, table, F_TXN, &foff))
        tmp.transaction_id = (FACE_TSS_TRANSACTION_ID_TYPE)rd_s64(buf + foff);
    if (find_field(buf, len, table, F_SRC, &foff))
        tmp.source_id = (FACE_TSS_GUID_TYPE)rd_s64(buf + foff);
    if (find_field(buf, len, table, F_SEQ, &foff))
        tmp.sequence_number = rd_u64(buf + foff);
    if (find_field(buf, len, table, F_TS, &foff))
        tmp.timestamp_ns = (FACE_SYSTEM_TIME_TYPE)rd_s64(buf + foff);

    /* field 5: payload vector (may be absent -> empty) */
    if (find_field(buf, len, table, F_PAYLOAD, &foff)) {
        at = foff;
        if (at + 4 > len)
            return FACE_TSS_RC_INVALID_PARAM;
        off = rd_u32(buf + at);
        at += off;
        if (at + 4 > len)
            return FACE_TSS_RC_INVALID_PARAM;
        n = rd_u32(buf + at);
        start = at + 4;
        if (start + n > len)
            return FACE_TSS_RC_INVALID_PARAM;
        if (n > 0) {
            tmp.payload = (uint8_t *)malloc(n ? n : 1);
            if (!tmp.payload)
                return FACE_TSS_RC_NOT_AVAILABLE;
            memcpy(tmp.payload, buf + start, n);
            tmp.payload_len = n;
        }
    }

    *out = tmp;
    return FACE_TSS_RC_NO_ERROR;
}
