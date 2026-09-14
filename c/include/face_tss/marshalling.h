/* FACE 3.2 TSS Primitive Marshalling interface.
 *
 * Marshal/unmarshal primitive types to/from a byte buffer.
 * This is the FACE::TSS::Primitive_Marshalling IDL interface (FACE 3.2).
 * Uses network byte order (big-endian) for multi-byte primitives.
 */

#ifndef FACE_TSS_MARSHALLING_H
#define FACE_TSS_MARSHALLING_H

#include <stddef.h>
#include <stdint.h>

#include "face_tss/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque marshalling instance. */
typedef struct FACE_TSS_MARSHALLING FACE_TSS_MARSHALLING;

FACE_TSS_MARSHALLING *face_tss_marshalling_create(void);
void face_tss_marshalling_destroy(FACE_TSS_MARSHALLING *m);

/* Marshal primitives: write to buffer, report bytes consumed. */
FACE_TSS_RETURN_CODE face_tss_marshal_short(FACE_TSS_MARSHALLING *m,
                                            int16_t data, uint8_t *buffer,
                                            size_t buffer_len,
                                            size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_marshal_long(FACE_TSS_MARSHALLING *m,
                                           int32_t data, uint8_t *buffer,
                                           size_t buffer_len,
                                           size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_marshal_long_long(FACE_TSS_MARSHALLING *m,
                                                int64_t data, uint8_t *buffer,
                                                size_t buffer_len,
                                                size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_marshal_unsigned_short(FACE_TSS_MARSHALLING *m,
                                                     uint16_t data,
                                                     uint8_t *buffer,
                                                     size_t buffer_len,
                                                     size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_marshal_unsigned_long(FACE_TSS_MARSHALLING *m,
                                                    uint32_t data,
                                                    uint8_t *buffer,
                                                    size_t buffer_len,
                                                    size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_marshal_unsigned_long_long(FACE_TSS_MARSHALLING *m,
                                                         uint64_t data,
                                                         uint8_t *buffer,
                                                         size_t buffer_len,
                                                         size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_marshal_float(FACE_TSS_MARSHALLING *m,
                                            float data, uint8_t *buffer,
                                            size_t buffer_len,
                                            size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_marshal_double(FACE_TSS_MARSHALLING *m,
                                             double data, uint8_t *buffer,
                                             size_t buffer_len,
                                             size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_marshal_long_double(FACE_TSS_MARSHALLING *m,
                                                  long double data,
                                                  uint8_t *buffer,
                                                  size_t buffer_len,
                                                  size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_marshal_char(FACE_TSS_MARSHALLING *m,
                                           char data, uint8_t *buffer,
                                           size_t buffer_len,
                                           size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_marshal_octet(FACE_TSS_MARSHALLING *m,
                                            uint8_t data, uint8_t *buffer,
                                            size_t buffer_len,
                                            size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_marshal_boolean(FACE_TSS_MARSHALLING *m,
                                              int data, uint8_t *buffer,
                                              size_t buffer_len,
                                              size_t *bytes_consumed_out);

/* Unmarshal primitives: read from buffer, report bytes consumed. */
FACE_TSS_RETURN_CODE face_tss_unmarshal_short(FACE_TSS_MARSHALLING *m,
                                              const uint8_t *buffer,
                                              size_t buffer_len,
                                              int16_t *data_out,
                                              size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_unmarshal_long(FACE_TSS_MARSHALLING *m,
                                             const uint8_t *buffer,
                                             size_t buffer_len,
                                             int32_t *data_out,
                                             size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_unmarshal_long_long(FACE_TSS_MARSHALLING *m,
                                                  const uint8_t *buffer,
                                                  size_t buffer_len,
                                                  int64_t *data_out,
                                                  size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_unmarshal_unsigned_short(FACE_TSS_MARSHALLING *m,
                                                       const uint8_t *buffer,
                                                       size_t buffer_len,
                                                       uint16_t *data_out,
                                                       size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_unmarshal_unsigned_long(FACE_TSS_MARSHALLING *m,
                                                      const uint8_t *buffer,
                                                      size_t buffer_len,
                                                      uint32_t *data_out,
                                                      size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_unmarshal_unsigned_long_long(FACE_TSS_MARSHALLING *m,
                                                           const uint8_t *buffer,
                                                           size_t buffer_len,
                                                           uint64_t *data_out,
                                                           size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_unmarshal_float(FACE_TSS_MARSHALLING *m,
                                              const uint8_t *buffer,
                                              size_t buffer_len,
                                              float *data_out,
                                              size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_unmarshal_double(FACE_TSS_MARSHALLING *m,
                                               const uint8_t *buffer,
                                               size_t buffer_len,
                                               double *data_out,
                                               size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_unmarshal_long_double(FACE_TSS_MARSHALLING *m,
                                                    const uint8_t *buffer,
                                                    size_t buffer_len,
                                                    long double *data_out,
                                                    size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_unmarshal_char(FACE_TSS_MARSHALLING *m,
                                             const uint8_t *buffer,
                                             size_t buffer_len,
                                             char *data_out,
                                             size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_unmarshal_octet(FACE_TSS_MARSHALLING *m,
                                              const uint8_t *buffer,
                                              size_t buffer_len,
                                              uint8_t *data_out,
                                              size_t *bytes_consumed_out);
FACE_TSS_RETURN_CODE face_tss_unmarshal_boolean(FACE_TSS_MARSHALLING *m,
                                                const uint8_t *buffer,
                                                size_t buffer_len,
                                                int *data_out,
                                                size_t *bytes_consumed_out);

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_MARSHALLING_H */
