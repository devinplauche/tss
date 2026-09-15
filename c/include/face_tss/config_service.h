#ifndef FACE_TSS_CONFIG_SERVICE_H
#define FACE_TSS_CONFIG_SERVICE_H

/* FACE::Configuration service (FACE 3.2 IDL, FACE/Configuration.idl).
 *
 * Session-based access to configuration containers: Initialize the
 * service, Open a named container to get an int64 session handle, then
 * Get_Size / Read / Seek within the session and Close when done.
 *
 * Built-in container backends (selected by container-name prefix):
 * - "memory:<name>": an in-memory container private to the session.
 *   Sets are populated with face_tss_config_service_write() (an
 *   implementation extension; the IDL has no write operation).
 * - "file:<dir>": exposes regular files directly under <dir> as sets;
 *   the set name is the file name. Read streams file content with full
 *   Seek support.
 * A container name with any other prefix yields INVALID_CONFIG.
 *
 * Thread safety: all entry points are safe to call concurrently; a
 * single mutex guards the session table. The lock is held across file
 * I/O, which is acceptable for a configuration service.
 */

#include <stddef.h>
#include <stdint.h>

#include "face_tss/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque service instance. */
typedef struct FACE_TSS_CONFIG_SERVICE FACE_TSS_CONFIG_SERVICE;

/* FACE::Configuration::HANDLE_TYPE (IDL `long`; 64-bit here). */
typedef int64_t FACE_TSS_CONFIG_HANDLE;

/* FACE::Configuration::WHENCE_TYPE. Values match the IDL order. */
typedef enum FACE_TSS_CONFIG_WHENCE {
    FACE_TSS_CONFIG_SEEK_FROM_START = 0,
    FACE_TSS_CONFIG_SEEK_FROM_CURRENT = 1,
    FACE_TSS_CONFIG_SEEK_FROM_END = 2
} FACE_TSS_CONFIG_WHENCE;

/* Create/destroy a Configuration service instance. create() returns
 * NULL on allocation failure. */
FACE_TSS_CONFIG_SERVICE *face_tss_config_service_create(void);
void face_tss_config_service_destroy(FACE_TSS_CONFIG_SERVICE *svc);

/* FACE::Configuration::Initialize. initialization_information is
 * implementation-specific; it is accepted and ignored (may not be NULL).
 * - NO_ERROR: initialized (idempotent).
 * - INVALID_PARAM: svc or initialization_information is NULL. */
FACE_TSS_RETURN_CODE face_tss_config_service_initialize(
    FACE_TSS_CONFIG_SERVICE *svc, const char *initialization_information);

/* FACE::Configuration::Open. container_name selects the backend:
 * "memory:<name>" or "file:<dir>".
 * - NO_ERROR and *handle set on success.
 * - INVALID_CONFIG: unknown backend prefix, or the file directory does
 *   not exist / is not a directory.
 * - INVALID_PARAM: svc, container_name, or handle is NULL. */
FACE_TSS_RETURN_CODE face_tss_config_service_open(
    FACE_TSS_CONFIG_SERVICE *svc, const char *container_name,
    FACE_TSS_CONFIG_HANDLE *handle);

/* FACE::Configuration::Get_Size.
 * - NO_ERROR and *size set on success.
 * - INVALID_CONFIG: bad handle, or no such set in the container.
 * - INVALID_PARAM: svc, set_name, or size is NULL. */
FACE_TSS_RETURN_CODE face_tss_config_service_get_size(
    FACE_TSS_CONFIG_SERVICE *svc, FACE_TSS_CONFIG_HANDLE handle,
    const char *set_name, long *size);

/* FACE::Configuration::Read. Reads up to buffer_size bytes from set_name
 * starting at the session's current position; the position advances by
 * the number of bytes read.
 * - NO_ERROR and *bytes_read set on success (bytes_read may be 0 for a
 *   zero-length read or an empty set).
 * - INVALID_CONFIG: bad handle, or no such set in the container.
 * - INVALID_PARAM: svc, set_name, buffer, or bytes_read is NULL, or
 *   buffer_size is negative.
 * - NOT_AVAILABLE: the session position is at or past the end of the
 *   set (the entire stream has been read). */
FACE_TSS_RETURN_CODE face_tss_config_service_read(
    FACE_TSS_CONFIG_SERVICE *svc, FACE_TSS_CONFIG_HANDLE handle,
    const char *set_name, void *buffer, long buffer_size, long *bytes_read);

/* FACE::Configuration::Seek. Moves the session's position indicator.
 * SEEK_FROM_START requires offset >= 0; SEEK_FROM_END requires
 * offset <= 0 and a set selected by a prior Read/Get_Size (its size is
 * the reference); SEEK_FROM_CURRENT allows any offset. The resulting
 * position must be >= 0; seeking past the end is allowed (a later Read
 * reports NOT_AVAILABLE).
 * - NO_ERROR on success.
 * - INVALID_CONFIG: bad handle, or SEEK_FROM_END with no set selected
 *   yet / the selected set no longer exists.
 * - INVALID_PARAM: svc is NULL, whence is not a valid
 *   FACE_TSS_CONFIG_WHENCE, or the offset is invalid for whence / would
 *   place the position before the start. */
FACE_TSS_RETURN_CODE face_tss_config_service_seek(
    FACE_TSS_CONFIG_SERVICE *svc, FACE_TSS_CONFIG_HANDLE handle,
    FACE_TSS_CONFIG_WHENCE whence, long offset);

/* FACE::Configuration::Close.
 * - NO_ERROR on success.
 * - INVALID_CONFIG: bad (unknown or already-closed) handle.
 * - INVALID_PARAM: svc is NULL. */
FACE_TSS_RETURN_CODE face_tss_config_service_close(
    FACE_TSS_CONFIG_SERVICE *svc, FACE_TSS_CONFIG_HANDLE handle);

/* Implementation extension (not in the IDL): store `len` bytes from
 * `data` as `set_name` in a "memory:" container, replacing any existing
 * set of that name. Only valid on memory-container sessions.
 * - NO_ERROR on success.
 * - INVALID_CONFIG: bad handle, or the session is not a memory
 *   container.
 * - INVALID_PARAM: svc, set_name, or data is NULL (data may be NULL
 *   only when len is 0), or len is negative. */
FACE_TSS_RETURN_CODE face_tss_config_service_write(
    FACE_TSS_CONFIG_SERVICE *svc, FACE_TSS_CONFIG_HANDLE handle,
    const char *set_name, const void *data, long len);

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_CONFIG_SERVICE_H */
