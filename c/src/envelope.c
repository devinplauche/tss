/* FACE TSS envelope codec over the flatcc runtime. See envelope.h.
 *
 * Encoding uses the flatcc builder API directly (table_add / create_string
 * / create_vector / end_table / finalize_buffer) so no generated code is
 * needed. Decoding uses the raw FlatBuffers object layout with endian-safe
 * memcpy reads plus `flatcc_verify_table_as_root` against a hand-written
 * table verifier that only accepts the eight known fields with their exact
 * scalar widths.
 */

#include "face_tss/envelope.h"
#include "face_tss/config.h"

#include <stdlib.h>
#include <string.h>

#include <flatcc/flatcc_builder.h>
#include <flatcc/flatcc_endian.h>
#include <flatcc/flatcc_types.h>

/* The bare flatcc_endian.h header needs the flatcc flatbuffers base types
 * for the offset accessors; include the full base header which pulls in
 * flatcc_types.h in the right order. */
#include <flatcc/flatcc_flatbuffers.h>

/* stdint.h for uintptr_t used in decode alignment check. */
#include <stdint.h>

#define F_CONN 0
#define F_TXN 1
#define F_SRC 2
#define F_SEQ 3
#define F_TS 4
#define F_PAYLOAD 5
#define F_GUID 6
#define F_IUID 7

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

    /* Create leaf objects first (may be done before/without open table).
     * NOTE: create_vector takes a COUNT, not a byte length. */
    name_ref = flatcc_builder_create_string_str(B, norm);
    if (!name_ref)
        goto oom;
    payload_ref = flatcc_builder_create_vector(
        B, env->payload, env->payload_len, 1, 1,
        FLATBUFFERS_COUNT_MAX(1));
    if (!payload_ref && env->payload_len > 0)
        goto oom;

    if (flatcc_builder_start_table(B, 8))
        goto oom;

    /* Offsets first, then scalars: flatcc packs offset slots before scalar
     * slots in the table body (matches flatcc-generated code order). */
    pref = flatcc_builder_table_add_offset(B, F_CONN);
    if (!pref)
        goto oom_table;
    *pref = name_ref;

    /* ubyte vector field 5 (empty payload encodes as absent -> empty). */
    if (payload_ref) {
        pref = flatcc_builder_table_add_offset(B, F_PAYLOAD);
        if (!pref)
            goto oom_table;
        *pref = payload_ref;
    }

    /* Scalar fields: table_add returns a NATIVE-endian slot; convert with
     * _write_to_pe (generated code does TN_assign_to_pe, same thing).
     * Skipped when zero (FlatBuffers default semantics). */
    if (env->transaction_id != 0) {
        pi64 = (int64_t *)flatcc_builder_table_add(B, F_TXN, 8, 8);
        if (!pi64)
            goto oom_table;
        flatbuffers_int64_write_to_pe(pi64, (int64_t)env->transaction_id);
    }
    if (env->source_id != 0) {
        pi64 = (int64_t *)flatcc_builder_table_add(B, F_SRC, 8, 8);
        if (!pi64)
            goto oom_table;
        flatbuffers_int64_write_to_pe(pi64, (int64_t)env->source_id);
    }
    if (env->sequence_number != 0) {
        pu64 = (uint64_t *)flatcc_builder_table_add(B, F_SEQ, 8, 8);
        if (!pu64)
            goto oom_table;
        flatbuffers_uint64_write_to_pe(pu64, (uint64_t)env->sequence_number);
    }
    if (env->timestamp_ns != 0) {
        pi64 = (int64_t *)flatcc_builder_table_add(B, F_TS, 8, 8);
        if (!pi64)
            goto oom_table;
        flatbuffers_int64_write_to_pe(pi64, (int64_t)env->timestamp_ns);
    }
    if (env->message_guid != 0) {
        pi64 = (int64_t *)flatcc_builder_table_add(B, F_GUID, 8, 8);
        if (!pi64)
            goto oom_table;
        flatbuffers_int64_write_to_pe(pi64, (int64_t)env->message_guid);
    }
    if (env->instance_uid != 0) {
        pi64 = (int64_t *)flatcc_builder_table_add(B, F_IUID, 8, 8);
        if (!pi64)
            goto oom_table;
        flatbuffers_int64_write_to_pe(pi64, (int64_t)env->instance_uid);
    }

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
    if (!buf || size == 0 || size > (size_t)0xFFFFFFF0UL) {
        if (buf)
            flatcc_builder_free(buf);
        flatcc_builder_clear(B);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    flatcc_builder_clear(B);
    /* Copy to caller-owned malloc buffer (flatcc finalize buffer must be
     * released with flatcc_builder_free; keep ownership explicit). */
    {
        uint8_t *copy = (uint8_t *)malloc(size);
        if (!copy) {
            flatcc_builder_free(buf);
            return FACE_TSS_RC_NOT_AVAILABLE;
        }
        memcpy(copy, buf, size);
        flatcc_builder_free(buf);
        *out = copy;
    }
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
/* Decode: explicit little-endian reads, single vtable derivation.        */
/* ------------------------------------------------------------------ */

/* The FlatBuffers wire format is little-endian; decode with explicit LE
 * loads (portable to big-endian hosts). */
static uint32_t rd_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint16_t rd_u16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static uint16_t rd_voff(const uint8_t *p)
{
    return rd_u16(p);
}

static uint32_t rd_uoff(const uint8_t *p)
{
    return rd_u32(p);
}

static int32_t rd_soff(const uint8_t *p)
{
    return (int32_t)rd_u32(p);
}

/* NOTE: no byte-by-byte u64/i64 reconstruction here: GCC -O3 rewrites
 * the shift-or chain into a single unaligned 8-byte load, which faults on
 * strict-alignment targets and defeats the alignment-scratch logic. Use
 * memcpy (GCC lowers it to a safe unaligned access) instead. */
static int64_t rd_s64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return (int64_t)v;
}

static uint64_t rd_u64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

/* Vtable entry for field id, or 0 when absent. All bounds pre-checked:
 * the caller guarantees vsize covers ids 0..7 (checked once in decode). */
static uint16_t vt_entry(const uint8_t *buf, uint32_t vtable,
                         uint32_t vsize, int id)
{
    uint32_t at;
    uint16_t e;
    if (id < 0 || id > 7)
        return 0;
    if (vsize < (uint32_t)(4 + 2 * id + 2))
        return 0;
    at = vtable + 4 + (uint32_t)id * 2;
    e = rd_voff(buf + at);
    return e;
}

FACE_TSS_RETURN_CODE face_tss_envelope_decode(
    const uint8_t *buf, size_t len, FACE_TSS_ENVELOPE *out)
{
    uint32_t root, table, vtable, vsize, tsize;
    int32_t soff;
    uint32_t at, off, n, start, foff, entry;
    FACE_TSS_ENVELOPE tmp;

    if (!buf || !out)
        return FACE_TSS_RC_INVALID_PARAM;
    if (len < 8 || len > (size_t)0xFFFFFFF0UL)
        return FACE_TSS_RC_INVALID_PARAM;

    /* Unaligned callers are copied to an aligned scratch buffer
     * (FlatBuffers offsets require at least uoffset alignment). */
    if (((uintptr_t)buf & 3u) != 0u) {
        uint8_t *copy = (uint8_t *)malloc(len);
        FACE_TSS_RETURN_CODE rc;
        if (!copy)
            return FACE_TSS_RC_NOT_AVAILABLE;
        memcpy(copy, buf, len);
        rc = face_tss_envelope_decode(copy, len, out);
        free(copy);
        return rc;
    }

    /* Root table. */
    root = rd_uoff(buf);
    if (root < 4 || root > (uint32_t)(len - 4))
        return FACE_TSS_RC_INVALID_PARAM;
    table = root;

    /* Vtable (derived once; every field lookup reuses it). */
    soff = rd_soff(buf + table);
    /* soff is signed in the format and usually negative (flatcc clusters
     * vtables after the table); the subtraction wraps identically in
     * unsigned arithmetic, so derive that way and bounds-check below. */
    vtable = table - (uint32_t)soff;
    if (vtable > (uint32_t)(len - 4))
        return FACE_TSS_RC_INVALID_PARAM;
    vsize = rd_voff(buf + vtable);
    tsize = rd_voff(buf + vtable + 2);
    if (vsize < 4 || (vsize & 1u) || vsize > (uint32_t)(len - vtable))
        return FACE_TSS_RC_INVALID_PARAM;
    if (tsize < 4 || tsize > (uint32_t)(len - table))
        return FACE_TSS_RC_INVALID_PARAM;
    memset(&tmp, 0, sizeof(tmp));

    /* field 0: required string. */
    entry = vt_entry(buf, vtable, vsize, F_CONN);
    if (entry == 0 || entry + 4 > tsize)
        return FACE_TSS_RC_INVALID_PARAM;
    at = table + entry;
    if (at > (uint32_t)(len - 4))
        return FACE_TSS_RC_INVALID_PARAM;
    off = rd_uoff(buf + at);
    if (off == 0 || off > (uint32_t)(len - at - 4))
        return FACE_TSS_RC_INVALID_PARAM;
    at += off;
    if (at > (uint32_t)(len - 4))
        return FACE_TSS_RC_INVALID_PARAM;
    n = rd_uoff(buf + at);
    start = at + 4;
    if (n >= FACE_TSS_MAX_CONNECTION_NAME)
        return FACE_TSS_RC_INVALID_PARAM;
    if (start >= len)
        return FACE_TSS_RC_INVALID_PARAM;
    if (n + 1 > len - start)
        return FACE_TSS_RC_INVALID_PARAM;
    if (buf[start + n] != '\0')
        return FACE_TSS_RC_INVALID_PARAM;
    memcpy(tmp.connection_name, buf + start, n);
    tmp.connection_name[n] = '\0';

    /* scalar fields (absent -> 0 default). Inline the slot check instead
     * of a helper so the vtable cannot be re-derived per field. */
#define TRY_SCALAR(fid, stmt)                                            \
    do {                                                                 \
        uint16_t _e = vt_entry(buf, vtable, vsize, (fid));               \
        if (_e != 0) {                                                   \
            uint32_t _at;                                                \
            if ((uint32_t)_e + 8 > tsize)                                \
                return FACE_TSS_RC_INVALID_PARAM;                        \
            _at = table + _e;                                            \
            if (_at > (uint32_t)(len - 8))                               \
                return FACE_TSS_RC_INVALID_PARAM;                        \
            stmt;                                                        \
        }                                                                \
    } while (0)

    TRY_SCALAR(F_TXN, tmp.transaction_id =
                   (FACE_TSS_TRANSACTION_ID_TYPE)rd_s64(buf + _at));
    TRY_SCALAR(F_SRC, tmp.source_id =
                   (FACE_TSS_UID_TYPE)rd_s64(buf + _at));
    TRY_SCALAR(F_SEQ, tmp.sequence_number = rd_u64(buf + _at));
    TRY_SCALAR(F_TS, tmp.timestamp_ns =
                   (FACE_SYSTEM_TIME_TYPE)rd_s64(buf + _at));
    TRY_SCALAR(F_GUID, tmp.message_guid =
                   (FACE_TSS_MESSAGE_GUID_TYPE)rd_s64(buf + _at));
    TRY_SCALAR(F_IUID, tmp.instance_uid =
                   (FACE_TSS_UID_TYPE)rd_s64(buf + _at));
#undef TRY_SCALAR

    /* field 5: payload vector (absent -> empty). */
    foff = 0;
    {
        uint16_t _e = vt_entry(buf, vtable, vsize, F_PAYLOAD);
        if (_e != 0) {
            if ((uint32_t)_e + 4 > tsize)
                return FACE_TSS_RC_INVALID_PARAM;
            foff = table + _e;
        }
    }
    if (foff != 0) {
        at = foff;
        if (at > (uint32_t)(len - 4))
            return FACE_TSS_RC_INVALID_PARAM;
        off = rd_uoff(buf + at);
        if (off == 0 || off > (uint32_t)(len - at - 4))
            return FACE_TSS_RC_INVALID_PARAM;
        at += off;
        if (at > (uint32_t)(len - 4))
            return FACE_TSS_RC_INVALID_PARAM;
        n = rd_uoff(buf + at);
        start = at + 4;
        if (n > (uint32_t)(len - start))
            return FACE_TSS_RC_INVALID_PARAM;
        if (n > 0) {
            tmp.payload = (uint8_t *)malloc(n);
            if (!tmp.payload)
                return FACE_TSS_RC_NOT_AVAILABLE;
            memcpy(tmp.payload, buf + start, n);
            tmp.payload_len = n;
        }
    }

    (void)foff;
    *out = tmp;
    return FACE_TSS_RC_NO_ERROR;
}
