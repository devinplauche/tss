#ifndef FACE_TSS_CONFIG_H
#define FACE_TSS_CONFIG_H

/*
 * FACE TSS connection configuration (C binding).
 *
 * A configuration names every connection the TSS instance may create and
 * describes how each one moves data:
 *
 * - direction: SOURCE / DESTINATION / BI_DIRECTIONAL (FACE semantics).
 * - transport: which nng pattern carries it (pubsub fan-out or bus mesh),
 *   over the `address` URL (e.g. "tcp://127.0.0.1:5555").
 * - role: which end of the socket this instance owns. For pubsub one side
 *   must be publisher and the other subscriber; for bus both sides use bus.
 * - max_message_size: largest typed payload accepted; larger sends fail
 *   with FACE_TSS_RC_BUFFER_TOO_SMALL.
 *
 * Configs are plain text (see configs/*.json) parsed with a small built-in
 * JSON reader, or built programmatically with face_tss_config_add().
 */

#include "face_tss/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FACE_TSS_CONNECTION_CONFIG {
    char name[FACE_TSS_MAX_CONNECTION_NAME];
    FACE_TSS_DIRECTION direction;
    FACE_TSS_TRANSPORT_KIND transport;
    FACE_TSS_ROLE role;
    char address[FACE_TSS_MAX_ADDRESS];
    FACE_TSS_MESSAGE_SIZE_TYPE max_message_size;
    int queue_depth;
} FACE_TSS_CONNECTION_CONFIG;

typedef struct FACE_TSS_CONFIG {
    char instance_name[FACE_TSS_MAX_STRING];
    FACE_TSS_CONNECTION_CONFIG *connections;
    size_t count;
    size_t capacity;
} FACE_TSS_CONFIG;

/* Uppercase `name` into `out` (size FACE_TSS_MAX_CONNECTION_NAME).
 * Returns FACE_TSS_RC_NO_ERROR or FACE_TSS_RC_INVALID_PARAM. */
FACE_TSS_RETURN_CODE face_tss_normalize_name(
    const char *name, char out[FACE_TSS_MAX_CONNECTION_NAME]);

/* Lifecycle. `config_init` zeroes; `config_reserve/add` grow the table. */
void face_tss_config_init(FACE_TSS_CONFIG *cfg, const char *instance_name);
void face_tss_config_fini(FACE_TSS_CONFIG *cfg);
FACE_TSS_RETURN_CODE face_tss_config_reserve(FACE_TSS_CONFIG *cfg, size_t n);
FACE_TSS_RETURN_CODE face_tss_config_add(
    FACE_TSS_CONFIG *cfg, const FACE_TSS_CONNECTION_CONFIG *conn);

/* Case-insensitive lookup. Returns NULL when absent. */
const FACE_TSS_CONNECTION_CONFIG *face_tss_config_lookup(
    const FACE_TSS_CONFIG *cfg, const char *name);

/* Parse {"instance_name": ..., "connections": [...]} from memory / file.
 * Unknown fields are ignored; missing fields take documented defaults
 * (direction BI_DIRECTIONAL, transport pubsub, role subscriber,
 * max_message_size 65536, queue_depth 64). */
FACE_TSS_RETURN_CODE face_tss_config_from_json(
    const char *json, size_t len, FACE_TSS_CONFIG *out);
FACE_TSS_RETURN_CODE face_tss_config_from_file(
    const char *path, FACE_TSS_CONFIG *out);

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_CONFIG_H */
