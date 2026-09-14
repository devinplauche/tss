/* FACE 3.2 TSS Primitive Marshalling implementation.
 * Network byte order (big-endian) for multi-byte primitives. */

#include "face_tss/marshalling.h"

#include <stdlib.h>
#include <string.h>

struct FACE_TSS_MARSHALLING {
    int dummy;
};

FACE_TSS_MARSHALLING *face_tss_marshalling_create(void)
{
    return (FACE_TSS_MARSHALLING *)calloc(1, sizeof(FACE_TSS_MARSHALLING));
}

void face_tss_marshalling_destroy(FACE_TSS_MARSHALLING *m)
{
    free(m);
}

/* Big-endian encode/decode helpers. */
static void enc16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}
static void enc32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}
static void enc64(uint8_t *b, uint64_t v)
{
    b[0] = (uint8_t)(v >> 56);
    b[1] = (uint8_t)(v >> 48);
    b[2] = (uint8_t)(v >> 40);
    b[3] = (uint8_t)(v >> 32);
    b[4] = (uint8_t)(v >> 24);
    b[5] = (uint8_t)(v >> 16);
    b[6] = (uint8_t)(v >> 8);
    b[7] = (uint8_t)v;
}
static uint16_t dec16(const uint8_t *b)
{
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}
static uint32_t dec32(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}
static uint64_t dec64(const uint8_t *b)
{
    return ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) |
           ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
           ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
           ((uint64_t)b[6] << 8) | b[7];
}

#define MARSHAL_CHECK(m, buffer, need, out)                          \
    do {                                                             \
        if (!(m) || !(buffer) || !(out))                              \
            return FACE_TSS_RC_INVALID_PARAM;                         \
        if (buffer_len < (need))                                      \
            return FACE_TSS_RC_DATA_BUFFER_TOO_SMALL;                 \
    } while (0)

FACE_TSS_RETURN_CODE face_tss_marshal_short(FACE_TSS_MARSHALLING *m,
                                            int16_t data, uint8_t *buffer,
                                            size_t buffer_len,
                                            size_t *bytes_consumed_out)
{
    MARSHAL_CHECK(m, buffer, 2, bytes_consumed_out);
    enc16(buffer, (uint16_t)data);
    *bytes_consumed_out = 2;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_marshal_long(FACE_TSS_MARSHALLING *m,
                                           int32_t data, uint8_t *buffer,
                                           size_t buffer_len,
                                           size_t *bytes_consumed_out)
{
    MARSHAL_CHECK(m, buffer, 4, bytes_consumed_out);
    enc32(buffer, (uint32_t)data);
    *bytes_consumed_out = 4;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_marshal_long_long(FACE_TSS_MARSHALLING *m,
                                                int64_t data, uint8_t *buffer,
                                                size_t buffer_len,
                                                size_t *bytes_consumed_out)
{
    MARSHAL_CHECK(m, buffer, 8, bytes_consumed_out);
    enc64(buffer, (uint64_t)data);
    *bytes_consumed_out = 8;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_marshal_unsigned_short(FACE_TSS_MARSHALLING *m,
                                                     uint16_t data,
                                                     uint8_t *buffer,
                                                     size_t buffer_len,
                                                     size_t *bytes_consumed_out)
{
    MARSHAL_CHECK(m, buffer, 2, bytes_consumed_out);
    enc16(buffer, data);
    *bytes_consumed_out = 2;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_marshal_unsigned_long(FACE_TSS_MARSHALLING *m,
                                                    uint32_t data,
                                                    uint8_t *buffer,
                                                    size_t buffer_len,
                                                    size_t *bytes_consumed_out)
{
    MARSHAL_CHECK(m, buffer, 4, bytes_consumed_out);
    enc32(buffer, data);
    *bytes_consumed_out = 4;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_marshal_unsigned_long_long(FACE_TSS_MARSHALLING *m,
                                                         uint64_t data,
                                                         uint8_t *buffer,
                                                         size_t buffer_len,
                                                         size_t *bytes_consumed_out)
{
    MARSHAL_CHECK(m, buffer, 8, bytes_consumed_out);
    enc64(buffer, data);
    *bytes_consumed_out = 8;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_marshal_float(FACE_TSS_MARSHALLING *m,
                                            float data, uint8_t *buffer,
                                            size_t buffer_len,
                                            size_t *bytes_consumed_out)
{
    uint32_t u;
    MARSHAL_CHECK(m, buffer, 4, bytes_consumed_out);
    memcpy(&u, &data, 4);
    enc32(buffer, u);
    *bytes_consumed_out = 4;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_marshal_double(FACE_TSS_MARSHALLING *m,
                                             double data, uint8_t *buffer,
                                             size_t buffer_len,
                                             size_t *bytes_consumed_out)
{
    uint64_t u;
    MARSHAL_CHECK(m, buffer, 8, bytes_consumed_out);
    memcpy(&u, &data, 8);
    enc64(buffer, u);
    *bytes_consumed_out = 8;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_marshal_long_double(FACE_TSS_MARSHALLING *m,
                                                  long double data,
                                                  uint8_t *buffer,
                                                  size_t buffer_len,
                                                  size_t *bytes_consumed_out)
{
    /* long double is 16 bytes on x86-64; marshal as raw bytes. */
    MARSHAL_CHECK(m, buffer, sizeof(long double), bytes_consumed_out);
    memcpy(buffer, &data, sizeof(long double));
    *bytes_consumed_out = sizeof(long double);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_marshal_char(FACE_TSS_MARSHALLING *m,
                                           char data, uint8_t *buffer,
                                           size_t buffer_len,
                                           size_t *bytes_consumed_out)
{
    MARSHAL_CHECK(m, buffer, 1, bytes_consumed_out);
    buffer[0] = (uint8_t)data;
    *bytes_consumed_out = 1;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_marshal_octet(FACE_TSS_MARSHALLING *m,
                                            uint8_t data, uint8_t *buffer,
                                            size_t buffer_len,
                                            size_t *bytes_consumed_out)
{
    MARSHAL_CHECK(m, buffer, 1, bytes_consumed_out);
    buffer[0] = data;
    *bytes_consumed_out = 1;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_marshal_boolean(FACE_TSS_MARSHALLING *m,
                                              int data, uint8_t *buffer,
                                              size_t buffer_len,
                                              size_t *bytes_consumed_out)
{
    MARSHAL_CHECK(m, buffer, 1, bytes_consumed_out);
    buffer[0] = data ? 1 : 0;
    *bytes_consumed_out = 1;
    return FACE_TSS_RC_NO_ERROR;
}

#define UNMARSHAL_CHECK(m, buffer, need, out)                        \
    do {                                                             \
        if (!(m) || !(buffer) || !(out))                              \
            return FACE_TSS_RC_INVALID_PARAM;                         \
        if (buffer_len < (need))                                      \
            return FACE_TSS_RC_DATA_BUFFER_TOO_SMALL;                 \
    } while (0)

FACE_TSS_RETURN_CODE face_tss_unmarshal_short(FACE_TSS_MARSHALLING *m,
                                              const uint8_t *buffer,
                                              size_t buffer_len,
                                              int16_t *data_out,
                                              size_t *bytes_consumed_out)
{
    UNMARSHAL_CHECK(m, buffer, 2, bytes_consumed_out);
    if (!data_out)
        return FACE_TSS_RC_INVALID_PARAM;
    *data_out = (int16_t)dec16(buffer);
    *bytes_consumed_out = 2;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_unmarshal_long(FACE_TSS_MARSHALLING *m,
                                             const uint8_t *buffer,
                                             size_t buffer_len,
                                             int32_t *data_out,
                                             size_t *bytes_consumed_out)
{
    UNMARSHAL_CHECK(m, buffer, 4, bytes_consumed_out);
    if (!data_out)
        return FACE_TSS_RC_INVALID_PARAM;
    *data_out = (int32_t)dec32(buffer);
    *bytes_consumed_out = 4;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_unmarshal_long_long(FACE_TSS_MARSHALLING *m,
                                                  const uint8_t *buffer,
                                                  size_t buffer_len,
                                                  int64_t *data_out,
                                                  size_t *bytes_consumed_out)
{
    UNMARSHAL_CHECK(m, buffer, 8, bytes_consumed_out);
    if (!data_out)
        return FACE_TSS_RC_INVALID_PARAM;
    *data_out = (int64_t)dec64(buffer);
    *bytes_consumed_out = 8;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_unmarshal_unsigned_short(FACE_TSS_MARSHALLING *m,
                                                       const uint8_t *buffer,
                                                       size_t buffer_len,
                                                       uint16_t *data_out,
                                                       size_t *bytes_consumed_out)
{
    UNMARSHAL_CHECK(m, buffer, 2, bytes_consumed_out);
    if (!data_out)
        return FACE_TSS_RC_INVALID_PARAM;
    *data_out = dec16(buffer);
    *bytes_consumed_out = 2;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_unmarshal_unsigned_long(FACE_TSS_MARSHALLING *m,
                                                      const uint8_t *buffer,
                                                      size_t buffer_len,
                                                      uint32_t *data_out,
                                                      size_t *bytes_consumed_out)
{
    UNMARSHAL_CHECK(m, buffer, 4, bytes_consumed_out);
    if (!data_out)
        return FACE_TSS_RC_INVALID_PARAM;
    *data_out = dec32(buffer);
    *bytes_consumed_out = 4;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_unmarshal_unsigned_long_long(FACE_TSS_MARSHALLING *m,
                                                           const uint8_t *buffer,
                                                           size_t buffer_len,
                                                           uint64_t *data_out,
                                                           size_t *bytes_consumed_out)
{
    UNMARSHAL_CHECK(m, buffer, 8, bytes_consumed_out);
    if (!data_out)
        return FACE_TSS_RC_INVALID_PARAM;
    *data_out = dec64(buffer);
    *bytes_consumed_out = 8;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_unmarshal_float(FACE_TSS_MARSHALLING *m,
                                              const uint8_t *buffer,
                                              size_t buffer_len,
                                              float *data_out,
                                              size_t *bytes_consumed_out)
{
    uint32_t u;
    UNMARSHAL_CHECK(m, buffer, 4, bytes_consumed_out);
    if (!data_out)
        return FACE_TSS_RC_INVALID_PARAM;
    u = dec32(buffer);
    memcpy(data_out, &u, 4);
    *bytes_consumed_out = 4;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_unmarshal_double(FACE_TSS_MARSHALLING *m,
                                               const uint8_t *buffer,
                                               size_t buffer_len,
                                               double *data_out,
                                               size_t *bytes_consumed_out)
{
    uint64_t u;
    UNMARSHAL_CHECK(m, buffer, 8, bytes_consumed_out);
    if (!data_out)
        return FACE_TSS_RC_INVALID_PARAM;
    u = dec64(buffer);
    memcpy(data_out, &u, 8);
    *bytes_consumed_out = 8;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_unmarshal_long_double(FACE_TSS_MARSHALLING *m,
                                                    const uint8_t *buffer,
                                                    size_t buffer_len,
                                                    long double *data_out,
                                                    size_t *bytes_consumed_out)
{
    UNMARSHAL_CHECK(m, buffer, sizeof(long double), bytes_consumed_out);
    if (!data_out)
        return FACE_TSS_RC_INVALID_PARAM;
    memcpy(data_out, buffer, sizeof(long double));
    *bytes_consumed_out = sizeof(long double);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_unmarshal_char(FACE_TSS_MARSHALLING *m,
                                             const uint8_t *buffer,
                                             size_t buffer_len,
                                             char *data_out,
                                             size_t *bytes_consumed_out)
{
    UNMARSHAL_CHECK(m, buffer, 1, bytes_consumed_out);
    if (!data_out)
        return FACE_TSS_RC_INVALID_PARAM;
    *data_out = (char)buffer[0];
    *bytes_consumed_out = 1;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_unmarshal_octet(FACE_TSS_MARSHALLING *m,
                                              const uint8_t *buffer,
                                              size_t buffer_len,
                                              uint8_t *data_out,
                                              size_t *bytes_consumed_out)
{
    UNMARSHAL_CHECK(m, buffer, 1, bytes_consumed_out);
    if (!data_out)
        return FACE_TSS_RC_INVALID_PARAM;
    *data_out = buffer[0];
    *bytes_consumed_out = 1;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_unmarshal_boolean(FACE_TSS_MARSHALLING *m,
                                                const uint8_t *buffer,
                                                size_t buffer_len,
                                                int *data_out,
                                                size_t *bytes_consumed_out)
{
    UNMARSHAL_CHECK(m, buffer, 1, bytes_consumed_out);
    if (!data_out)
        return FACE_TSS_RC_INVALID_PARAM;
    *data_out = buffer[0] ? 1 : 0;
    *bytes_consumed_out = 1;
    return FACE_TSS_RC_NO_ERROR;
}
