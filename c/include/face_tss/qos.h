/* FACE 3.2 TSS QoS (Quality of Service) policy management.
 *
 * Manages QoS policies for connections: staleness thresholds, message
 * age reporting, and policy enforcement.
 */

#ifndef FACE_TSS_QOS_H
#define FACE_TSS_QOS_H

#include <stddef.h>
#include <stdint.h>

#include "face_tss/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* QoS policy kinds. */
typedef enum FACE_TSS_QOS_POLICY_KIND {
    FACE_TSS_QOS_STALENESS = 0,      /* max message age before MESSAGE_STALE */
    FACE_TSS_QOS_MAX_AGE = 1,        /* alias for staleness threshold */
    FACE_TSS_QOS_PRIORITY = 2,       /* message priority hint */
    FACE_TSS_QOS_RELIABILITY = 3    /* reliability level */
} FACE_TSS_QOS_POLICY_KIND;

/* Opaque QoS policy manager. */
typedef struct FACE_TSS_QOS FACE_TSS_QOS;

FACE_TSS_QOS *face_tss_qos_create(void);
void face_tss_qos_destroy(FACE_TSS_QOS *qos);

/* Set a QoS policy for a connection. */
FACE_TSS_RETURN_CODE face_tss_qos_set_policy(
    FACE_TSS_QOS *qos, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_QOS_POLICY_KIND kind, int64_t value_ns);

/* Get a QoS policy for a connection. */
FACE_TSS_RETURN_CODE face_tss_qos_get_policy(
    FACE_TSS_QOS *qos, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_QOS_POLICY_KIND kind, int64_t *value_ns_out);

/* Clear all policies for a connection. */
FACE_TSS_RETURN_CODE face_tss_qos_clear_policies(
    FACE_TSS_QOS *qos, FACE_TSS_CONNECTION_ID_TYPE connection_id);

/* Check if a message (with the given age) violates the staleness policy.
 * Returns MESSAGE_STALE if the age exceeds the threshold, NO_ERROR otherwise.
 * NO_ACTION if no staleness policy is set. */
FACE_TSS_RETURN_CODE face_tss_qos_check_staleness(
    FACE_TSS_QOS *qos, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    int64_t message_age_ns);

/* Build the QoS event list for a received message (message_age_ns). */
FACE_TSS_RETURN_CODE face_tss_qos_build_events(
    FACE_TSS_QOS *qos, int64_t message_age_ns,
    FACE_TSS_QOS_EVENT *qos_out);

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_QOS_H */
