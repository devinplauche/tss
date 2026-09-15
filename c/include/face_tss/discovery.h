/* FACE TSS UDP-broadcast peer discovery.
 *
 * Lets TSS instances find each other on a LAN without pre-configured
 * peer addresses ("TSS distribution / multi-instance discovery beyond
 * static config").
 *
 * Wire protocol (IPv4 only):
 *   - UDP datagrams on port FACE_TSS_DISCOVERY_PORT (51970).
 *   - Datagram layout: magic "FTSSD1" (6 bytes) | name (NUL-terminated,
 *     max FACE_TSS_DISCOVERY_NAME_MAX incl. NUL) | address
 *     (NUL-terminated, max FACE_TSS_DISCOVERY_ADDRESS_MAX incl. NUL) |
 *     interval_ms (8 bytes, little-endian, host order converted).
 *   - The protocol is versioned by the magic; receivers ignore datagrams
 *     with a wrong magic or malformed (unterminated/truncated) fields.
 *
 * Behavior:
 *   - One announcement per discovery object: announce() starts a
 *     background broadcaster thread; stop_announce() stops it.
 *   - A background listener thread maintains a table of recently seen
 *     peers. Entries expire after 3x the sender's interval_ms without a
 *     refresh, so departed peers vanish automatically.
 *   - The broadcaster uses connect()+send() on its UDP socket rather than
 *     sendto(); both reach the broadcast destination, and connect()+send()
 *     works in sandboxed environments where sendto() is restricted.
 *
 * Sandbox note: some sandboxed environments block UDP broadcast
 * (sendto/connect to 255.255.255.255 fails with EPERM). The default
 * destination is the limited broadcast address; when that is
 * unavailable, point the destination at 127.0.0.1 via
 * face_tss_discovery_set_destination() to exercise the full
 * announce/listen path over loopback. The wire format is identical.
 *
 * Thread-safety: all functions are safe to call concurrently. destroy()
 * stops the background threads and joins them before freeing.
 */

#ifndef FACE_TSS_DISCOVERY_H
#define FACE_TSS_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>

#include "face_tss/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UDP port for discovery datagrams. */
#define FACE_TSS_DISCOVERY_PORT 51970

/* Magic prefix of every discovery datagram ("FTSSD1"). */
#define FACE_TSS_DISCOVERY_MAGIC "FTSSD1"
#define FACE_TSS_DISCOVERY_MAGIC_LEN 6

/* Max bytes for a peer name / address, including the NUL terminator. */
#define FACE_TSS_DISCOVERY_NAME_MAX 64
#define FACE_TSS_DISCOVERY_ADDRESS_MAX 256

/* Announce interval bounds (ms). */
#define FACE_TSS_DISCOVERY_INTERVAL_MIN_MS 100
#define FACE_TSS_DISCOVERY_INTERVAL_MAX_MS 60000

/* A peer is forgotten after this many missed announce intervals. */
#define FACE_TSS_DISCOVERY_EXPIRE_FACTOR 3

/* Opaque discovery instance. */
typedef struct FACE_TSS_DISCOVERY FACE_TSS_DISCOVERY;

FACE_TSS_DISCOVERY *face_tss_discovery_create(void);
void face_tss_discovery_destroy(FACE_TSS_DISCOVERY *d);

/* Override the broadcast destination (dotted IPv4, e.g. "255.255.255.255"
 * or "127.0.0.1" for loopback testing). Must be called before announce().
 * - NO_ERROR: destination stored.
 * - INVALID_PARAM: NULL d/destination, bad IPv4 literal, or already
 *   announcing.
 * - NOT_AVAILABLE: the destination is unreachable (connect failed). */
FACE_TSS_RETURN_CODE face_tss_discovery_set_destination(
    FACE_TSS_DISCOVERY *d, const char *destination);

/* Start broadcasting our presence every interval_ms.
 * - NO_ERROR: broadcaster started.
 * - NO_ACTION: already announcing this exact name+address.
 * - NOT_AVAILABLE: already announcing a different name or address
 *   (one announcement per discovery object).
 * - INVALID_PARAM: NULL d/name/address, empty name/address, name or
 *   address too long, or interval_ms outside [100, 60000]. */
FACE_TSS_RETURN_CODE face_tss_discovery_announce(FACE_TSS_DISCOVERY *d,
                                                 const char *name,
                                                 const char *address,
                                                 uint64_t interval_ms);

/* Stop broadcasting. NO_ACTION if not announcing. */
FACE_TSS_RETURN_CODE face_tss_discovery_stop_announce(FACE_TSS_DISCOVERY *d);

/* Block up to timeout_ms for a peer called `name`.
 * - NO_ERROR: peer found; its address copied to address_out (always
 *   NUL-terminated, truncated if address_len is short).
 * - TIMED_OUT: no such peer within timeout_ms (timeout_ms == 0 polls
 *   the current table once).
 * - INVALID_PARAM: NULL d/name/address_out, or address_len == 0. */
FACE_TSS_RETURN_CODE face_tss_discovery_lookup(FACE_TSS_DISCOVERY *d,
                                               const char *name,
                                               uint64_t timeout_ms,
                                               char *address_out,
                                               size_t address_len);

/* Snapshot of currently known (non-expired) peers.
 * Copies up to `max` entries into names[][64] / addresses[][256] and
 * sets *count to the number copied.
 * - NO_ERROR: snapshot taken.
 * - INVALID_PARAM: NULL d/count, or max > 0 with NULL names/addresses. */
FACE_TSS_RETURN_CODE face_tss_discovery_list(FACE_TSS_DISCOVERY *d,
                                             char names[][FACE_TSS_DISCOVERY_NAME_MAX],
                                             char addresses[][FACE_TSS_DISCOVERY_ADDRESS_MAX],
                                             size_t *count, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_DISCOVERY_H */
