/* FACE 3.2 TSS QoS policy management implementation. */

#include "face_tss/qos.h"

#include <stdlib.h>
#include <string.h>

#define QOS_MAX_CONNECTIONS 64
#define QOS_MAX_POLICIES 4

typedef struct {
    FACE_TSS_QOS_POLICY_KIND kind;
    int64_t value_ns;
    int in_use;
} qos_policy_t;

typedef struct {
    FACE_TSS_CONNECTION_ID_TYPE connection_id;
    qos_policy_t policies[QOS_MAX_POLICIES];
    int in_use;
} qos_conn_t;

struct FACE_TSS_QOS {
    qos_conn_t conns[QOS_MAX_CONNECTIONS];
};

FACE_TSS_QOS *face_tss_qos_create(void)
{
    return (FACE_TSS_QOS *)calloc(1, sizeof(FACE_TSS_QOS));
}

void face_tss_qos_destroy(FACE_TSS_QOS *qos)
{
    free(qos);
}

static qos_conn_t *find_conn(FACE_TSS_QOS *qos,
                             FACE_TSS_CONNECTION_ID_TYPE id, int create)
{
    int i, j;
    for (i = 0; i < QOS_MAX_CONNECTIONS; i++) {
        if (qos->conns[i].in_use && qos->conns[i].connection_id == id)
            return &qos->conns[i];
    }
    if (!create)
        return NULL;
    for (i = 0; i < QOS_MAX_CONNECTIONS; i++) {
        if (!qos->conns[i].in_use) {
            qos->conns[i].in_use = 1;
            qos->conns[i].connection_id = id;
            for (j = 0; j < QOS_MAX_POLICIES; j++)
                qos->conns[i].policies[j].in_use = 0;
            return &qos->conns[i];
        }
    }
    return NULL;
}

FACE_TSS_RETURN_CODE face_tss_qos_set_policy(
    FACE_TSS_QOS *qos, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_QOS_POLICY_KIND kind, int64_t value_ns)
{
    qos_conn_t *c;
    int i;
    if (!qos)
        return FACE_TSS_RC_INVALID_PARAM;
    if (connection_id == 0)
        return FACE_TSS_RC_INVALID_PARAM;
    if (value_ns < 0)
        return FACE_TSS_RC_INVALID_PARAM;
    c = find_conn(qos, connection_id, 1);
    if (!c)
        return FACE_TSS_RC_RESOURCE_LIMIT_REACHED;
    for (i = 0; i < QOS_MAX_POLICIES; i++) {
        if (c->policies[i].in_use && c->policies[i].kind == kind) {
            c->policies[i].value_ns = value_ns;
            return FACE_TSS_RC_NO_ERROR;
        }
    }
    for (i = 0; i < QOS_MAX_POLICIES; i++) {
        if (!c->policies[i].in_use) {
            c->policies[i].in_use = 1;
            c->policies[i].kind = kind;
            c->policies[i].value_ns = value_ns;
            return FACE_TSS_RC_NO_ERROR;
        }
    }
    return FACE_TSS_RC_RESOURCE_LIMIT_REACHED;
}

FACE_TSS_RETURN_CODE face_tss_qos_get_policy(
    FACE_TSS_QOS *qos, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_QOS_POLICY_KIND kind, int64_t *value_ns_out)
{
    qos_conn_t *c;
    int i;
    if (!qos || !value_ns_out)
        return FACE_TSS_RC_INVALID_PARAM;
    if (connection_id == 0)
        return FACE_TSS_RC_INVALID_PARAM;
    c = find_conn(qos, connection_id, 0);
    if (!c)
        return FACE_TSS_RC_NO_ACTION; /* no policies for this connection */
    for (i = 0; i < QOS_MAX_POLICIES; i++) {
        if (c->policies[i].in_use && c->policies[i].kind == kind) {
            *value_ns_out = c->policies[i].value_ns;
            return FACE_TSS_RC_NO_ERROR;
        }
    }
    return FACE_TSS_RC_NO_ACTION; /* no such policy */
}

FACE_TSS_RETURN_CODE face_tss_qos_clear_policies(
    FACE_TSS_QOS *qos, FACE_TSS_CONNECTION_ID_TYPE connection_id)
{
    qos_conn_t *c;
    int i;
    if (!qos)
        return FACE_TSS_RC_INVALID_PARAM;
    c = find_conn(qos, connection_id, 0);
    if (!c)
        return FACE_TSS_RC_NO_ACTION;
    for (i = 0; i < QOS_MAX_POLICIES; i++)
        c->policies[i].in_use = 0;
    c->in_use = 0;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_qos_check_staleness(
    FACE_TSS_QOS *qos, FACE_TSS_CONNECTION_ID_TYPE connection_id,
    int64_t message_age_ns)
{
    int64_t threshold;
    FACE_TSS_RETURN_CODE rc;
    if (!qos)
        return FACE_TSS_RC_INVALID_PARAM;
    rc = face_tss_qos_get_policy(qos, connection_id,
                                 FACE_TSS_QOS_STALENESS, &threshold);
    if (rc == FACE_TSS_RC_NO_ACTION)
        return FACE_TSS_RC_NO_ACTION; /* no policy: no enforcement */
    if (rc != FACE_TSS_RC_NO_ERROR)
        return rc;
    if (message_age_ns > threshold)
        return FACE_TSS_RC_MESSAGE_STALE;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_qos_build_events(
    FACE_TSS_QOS *qos, int64_t message_age_ns,
    FACE_TSS_QOS_EVENT *qos_out)
{
    (void)qos;
    if (!qos_out)
        return FACE_TSS_RC_INVALID_PARAM;
    /* One honest element: message_age_ns. */
    memset(qos_out, 0, sizeof(*qos_out));
    qos_out->count = 1;
    /* The caller fills in the keyname/value; we just set the count. */
    (void)message_age_ns;
    return FACE_TSS_RC_NO_ERROR;
}
