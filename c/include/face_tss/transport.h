#ifndef FACE_TSS_TRANSPORT_H
#define FACE_TSS_TRANSPORT_H

/*
 * nng transport layer (C binding).
 *
 * Each FACE connection maps to exactly one nng socket:
 *
 * - pubsub: Pub0/Sub0 fan-out. The publisher listens on the configured
 *   address; every subscriber dials it (non-blocking, so start order never
 *   matters) and subscribes to CONNECTION_NAME + '\0'. Sends prepend that
 *   topic to the FlatBuffers envelope bytes; receives strip it. The NUL
 *   separator keeps HELLO from matching HELLO2.
 * - bus: Bus0 mesh. One process listens per address (see broker), the rest
 *   dial; every peer hears every peer. Frames are raw envelopes.
 *
 * FACE timeouts are int64 nanoseconds; FACE_TSS_TIMEOUT_INFINITE (-1) blocks
 * forever. face_tss_transport_receive maps an expired wait to
 * FACE_TSS_RC_TIMED_OUT.
 */

#include "face_tss/config.h"
#include "face_tss/envelope.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FACE_TSS_TRANSPORT FACE_TSS_TRANSPORT;

/* Open the transport described by `cfg` (listen or dial as appropriate).
 * On success stores a new handle in `*out`. */
FACE_TSS_RETURN_CODE face_tss_transport_open(
    const FACE_TSS_CONNECTION_CONFIG *cfg, FACE_TSS_TRANSPORT **out);

/* Join a bus mesh explicitly as a dialer (for nodes that must not listen).
 * Equivalent to open() for pubsub configs. */
FACE_TSS_RETURN_CODE face_tss_transport_open_dial(
    const FACE_TSS_CONNECTION_CONFIG *cfg, FACE_TSS_TRANSPORT **out);

void face_tss_transport_close(FACE_TSS_TRANSPORT *t);

/* Stop the callback thread and close the socket without freeing the
 * handle. Idempotent. Closing the socket aborts any thread blocked in
 * send/receive, which is how teardown interrupts in-flight I/O without
 * holding higher-level locks. */
void face_tss_transport_shutdown(FACE_TSS_TRANSPORT *t);

/* Send one envelope (framed per transport), waiting at most timeout_ns
 * for the send to complete (infinite when -1, non-blocking when 0). */
FACE_TSS_RETURN_CODE face_tss_transport_send(
    FACE_TSS_TRANSPORT *t, const FACE_TSS_ENVELOPE *env,
    FACE_TIMEOUT_TYPE timeout_ns);

/* Receive one envelope, waiting up to timeout_ns (infinite when -1,
 * poll when 0). TIMED_OUT when the wait expires. */
FACE_TSS_RETURN_CODE face_tss_transport_receive(
    FACE_TSS_TRANSPORT *t, FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_ENVELOPE *out);

/* Callback delivery: invoke `cb` on an internal thread per message until
 * face_tss_transport_callback_stop() is called. Only one callback per
 * transport; a second start returns FACE_TSS_RC_NO_ACTION. */
typedef void (*FACE_TSS_ENVELOPE_CB)(const FACE_TSS_ENVELOPE *env, void *user);
FACE_TSS_RETURN_CODE face_tss_transport_callback_start(
    FACE_TSS_TRANSPORT *t, FACE_TSS_ENVELOPE_CB cb, void *user);
FACE_TSS_RETURN_CODE face_tss_transport_callback_stop(FACE_TSS_TRANSPORT *t);

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_TRANSPORT_H */
