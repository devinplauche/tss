/* nng transport: one FACE connection = one nng socket. See transport.h. */

#include "face_tss/transport.h"

#include <stdlib.h>
#include <string.h>

#include <nng/nng.h>
#include <nng/protocol/bus0/bus.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

/* Some nng versions expose these; define fallbacks otherwise. */
#ifndef NNG_OPT_RECVTIMEO
#define NNG_OPT_RECVTIMEO "recv-timeout"
#endif
#ifndef NNG_OPT_SENDTIMEO
#define NNG_OPT_SENDTIMEO "send-timeout"
#endif
#ifndef NNG_OPT_SUB_SUBSCRIBE
#define NNG_OPT_SUB_SUBSCRIBE "sub:subscribe"
#endif
#ifndef NNG_DURATION_INFINITE
#define NNG_DURATION_INFINITE (-1)
#endif
#ifndef NNG_DURATION_ZERO
#define NNG_DURATION_ZERO (0)
#endif

struct FACE_TSS_TRANSPORT {
    FACE_TSS_CONNECTION_CONFIG cfg;
    nng_socket sock;
    char topic[FACE_TSS_MAX_CONNECTION_NAME + 1];
    size_t topic_len;
    /* callback state */
    FACE_TSS_ENVELOPE_CB cb;
    void *cb_user;
    nng_thread *cb_thread;
    volatile int cb_stop;
};

static long long ns_to_ms(FACE_TIMEOUT_TYPE ns)
{
    if (ns == FACE_TSS_TIMEOUT_INFINITE)
        return NNG_DURATION_INFINITE;
    if (ns <= 0)
        return NNG_DURATION_ZERO;
    return (long long)((ns + 999999LL) / 1000000LL);
}

static FACE_TSS_RETURN_CODE open_socket(
    const FACE_TSS_CONNECTION_CONFIG *cfg, int dial_only, nng_socket *sock)
{
    nng_socket s;
    int rv;
    memset(&s, 0, sizeof(s));
    if (cfg->transport == FACE_TSS_TRANSPORT_PUBSUB) {
        if (cfg->role == FACE_TSS_ROLE_PUBLISHER && !dial_only)
            rv = nng_pub0_open(&s);
        else
            rv = nng_sub0_open(&s);
    } else {
        rv = nng_bus0_open(&s);
    }
    if (rv != 0)
        return FACE_TSS_RC_NOT_AVAILABLE;

    if (cfg->transport == FACE_TSS_TRANSPORT_PUBSUB) {
        if (cfg->role == FACE_TSS_ROLE_PUBLISHER && !dial_only) {
            rv = nng_listen(s, cfg->address, NULL, 0);
            if (rv != 0) {
                nng_close(s);
                return (rv == NNG_EADDRINUSE) ? FACE_TSS_RC_ADDR_IN_USE
                                              : FACE_TSS_RC_NOT_AVAILABLE;
            }
        } else {
            char topic[FACE_TSS_MAX_CONNECTION_NAME + 1];
            size_t tlen = face_tss_topic_for(cfg->name, topic);
            rv = nng_dial(s, cfg->address, NULL, 0);
            if (rv != 0 && rv != NNG_ECONNREFUSED) {
                /* async dial: keep going; NNG_ECONNREFUSED just means the
                 * listener is not up yet - the dialer retries. */
            }
            rv = 0;
            if (cfg->role != FACE_TSS_ROLE_PUBLISHER) {
                rv = nng_socket_set(
                    s, NNG_OPT_SUB_SUBSCRIBE, topic, tlen);
            }
            if (rv != 0) {
                nng_close(s);
                return FACE_TSS_RC_NOT_AVAILABLE;
            }
        }
    } else {
        if (dial_only)
            rv = nng_dial(s, cfg->address, NULL, 0);
        else {
            rv = nng_listen(s, cfg->address, NULL, 0);
            if (rv == NNG_EADDRINUSE)
                rv = nng_dial(s, cfg->address, NULL, 0);
        }
        if (rv != 0 && rv != NNG_ECONNREFUSED)
            rv = 0; /* dialers retry in the background */
        if (rv != 0) {
            nng_close(s);
            return FACE_TSS_RC_NOT_AVAILABLE;
        }
    }
    *sock = s;
    return FACE_TSS_RC_NO_ERROR;
}

static FACE_TSS_TRANSPORT *alloc_t(const FACE_TSS_CONNECTION_CONFIG *cfg)
{
    FACE_TSS_TRANSPORT *t =
        (FACE_TSS_TRANSPORT *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->cfg = *cfg;
    t->topic_len = face_tss_topic_for(cfg->name, t->topic);
    return t;
}

FACE_TSS_RETURN_CODE face_tss_transport_open(
    const FACE_TSS_CONNECTION_CONFIG *cfg, FACE_TSS_TRANSPORT **out)
{
    FACE_TSS_TRANSPORT *t;
    FACE_TSS_RETURN_CODE rc;
    if (!cfg || !out)
        return FACE_TSS_RC_INVALID_PARAM;
    t = alloc_t(cfg);
    if (!t)
        return FACE_TSS_RC_NOT_AVAILABLE;
    rc = open_socket(cfg, 0, &t->sock);
    if (rc != FACE_TSS_RC_NO_ERROR) {
        free(t);
        return rc;
    }
    *out = t;
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_transport_open_dial(
    const FACE_TSS_CONNECTION_CONFIG *cfg, FACE_TSS_TRANSPORT **out)
{
    FACE_TSS_TRANSPORT *t;
    FACE_TSS_RETURN_CODE rc;
    if (!cfg || !out)
        return FACE_TSS_RC_INVALID_PARAM;
    t = alloc_t(cfg);
    if (!t)
        return FACE_TSS_RC_NOT_AVAILABLE;
    rc = open_socket(cfg, 1, &t->sock);
    if (rc != FACE_TSS_RC_NO_ERROR) {
        free(t);
        return rc;
    }
    *out = t;
    return FACE_TSS_RC_NO_ERROR;
}

void face_tss_transport_close(FACE_TSS_TRANSPORT *t)
{
    if (!t)
        return;
    face_tss_transport_callback_stop(t);
    nng_close(t->sock);
    free(t);
}

FACE_TSS_RETURN_CODE face_tss_transport_send(
    FACE_TSS_TRANSPORT *t, const FACE_TSS_ENVELOPE *env,
    FACE_TIMEOUT_TYPE timeout_ns)
{
    uint8_t *body = NULL;
    size_t body_len = 0;
    nng_msg *msg = NULL;
    int rv;
    FACE_TSS_RETURN_CODE rc;
    if (!t || !env)
        return FACE_TSS_RC_INVALID_PARAM;
    rc = face_tss_envelope_encode(env, &body, &body_len);
    if (rc != FACE_TSS_RC_NO_ERROR)
        return rc;
    rv = nng_msg_alloc(&msg, 0);
    if (rv != 0) {
        flatcc_builder_free(body);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    if (t->cfg.transport == FACE_TSS_TRANSPORT_PUBSUB) {
        rv = nng_msg_append(msg, t->topic, t->topic_len);
        if (rv != 0)
            goto fail;
    }
    rv = nng_msg_append(msg, body, body_len);
    flatcc_builder_free(body);
    body = NULL;
    if (rv != 0)
        goto fail;
    /* Bound the blocking time per the FACE Send_Message timeout. */
    rv = nng_socket_set_ms(
        t->sock, NNG_OPT_SENDTIMEO, (nng_duration)ns_to_ms(timeout_ns));
    if (rv != 0)
        goto fail;
    rv = nng_sendmsg(t->sock, msg, 0);
    /* Restore non-blocking sends for the callback/poll paths. */
    nng_socket_set_ms(t->sock, NNG_OPT_SENDTIMEO, NNG_DURATION_ZERO);
    if (rv == NNG_ETIMEDOUT) {
        nng_msg_free(msg);
        return FACE_TSS_RC_TIMED_OUT;
    }
    if (rv != 0) {
        nng_msg_free(msg);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    return FACE_TSS_RC_NO_ERROR;
fail:
    if (body)
        flatcc_builder_free(body);
    nng_msg_free(msg);
    return FACE_TSS_RC_NOT_AVAILABLE;
}

FACE_TSS_RETURN_CODE face_tss_transport_receive(
    FACE_TSS_TRANSPORT *t, FACE_TIMEOUT_TYPE timeout_ns,
    FACE_TSS_ENVELOPE *out)
{
    nng_msg *msg = NULL;
    int rv;
    const uint8_t *data;
    size_t len;
    FACE_TSS_RETURN_CODE rc;
    if (!t || !out)
        return FACE_TSS_RC_INVALID_PARAM;
    rv = nng_socket_set_ms(
        t->sock, NNG_OPT_RECVTIMEO, (nng_duration)ns_to_ms(timeout_ns));
    if (rv != 0)
        return FACE_TSS_RC_NOT_AVAILABLE;
    rv = nng_recvmsg(t->sock, &msg, 0);
    if (rv == NNG_ETIMEDOUT)
        return FACE_TSS_RC_TIMED_OUT;
    if (rv != 0)
        return FACE_TSS_RC_NOT_AVAILABLE;
    data = (const uint8_t *)nng_msg_body(msg);
    len = nng_msg_len(msg);
    if (t->cfg.transport == FACE_TSS_TRANSPORT_PUBSUB) {
        if (len < t->topic_len ||
            memcmp(data, t->topic, t->topic_len) != 0) {
            nng_msg_free(msg);
            return FACE_TSS_RC_NOT_AVAILABLE; /* topic mismatch */
        }
        data += t->topic_len;
        len -= t->topic_len;
    }
    rc = face_tss_envelope_decode(data, len, out);
    nng_msg_free(msg);
    return rc;
}

/* Callback loop: 100 ms poll slices so stop() is responsive. */
static void cb_loop(void *arg)
{
    FACE_TSS_TRANSPORT *t = (FACE_TSS_TRANSPORT *)arg;
    while (!t->cb_stop) {
        FACE_TSS_ENVELOPE env;
        FACE_TSS_RETURN_CODE rc =
            face_tss_transport_receive(t, 100 * 1000000LL, &env);
        if (rc == FACE_TSS_RC_TIMED_OUT)
            continue;
        if (rc != FACE_TSS_RC_NO_ERROR)
            continue;
        if (!t->cb_stop && t->cb)
            t->cb(&env, t->cb_user);
        face_tss_envelope_fini(&env);
        if (t->cb_stop)
            break;
    }
}

FACE_TSS_RETURN_CODE face_tss_transport_callback_start(
    FACE_TSS_TRANSPORT *t, FACE_TSS_ENVELOPE_CB cb, void *user)
{
    int rv;
    if (!t || !cb)
        return FACE_TSS_RC_INVALID_PARAM;
    if (t->cb)
        return FACE_TSS_RC_NO_ACTION;
    t->cb = cb;
    t->cb_user = user;
    t->cb_stop = 0;
    rv = nng_thread_create(&t->cb_thread, cb_loop, t);
    if (rv != 0) {
        t->cb = NULL;
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_transport_callback_stop(FACE_TSS_TRANSPORT *t)
{
    if (!t)
        return FACE_TSS_RC_INVALID_PARAM;
    if (!t->cb)
        return FACE_TSS_RC_NO_ACTION;
    t->cb_stop = 1;
    if (t->cb_thread) {
        nng_thread_destroy(t->cb_thread);
        t->cb_thread = NULL;
    }
    t->cb = NULL;
    t->cb_user = NULL;
    return FACE_TSS_RC_NO_ERROR;
}
