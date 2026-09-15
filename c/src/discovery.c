/* UDP-broadcast peer discovery (face_tss/discovery.h).
 *
 * Design notes:
 * - The listener thread binds 0.0.0.0:FACE_TSS_DISCOVERY_PORT and runs
 *   for the lifetime of the object (peers are discoverable even when we
 *   are not announcing).
 * - The broadcaster thread is started/stopped by announce()/stop_announce().
 *   Its socket is created and connect()ed synchronously in announce() so a
 *   bad destination fails fast with NOT_AVAILABLE. connect()+send() is
 *   used instead of sendto(); both reach a broadcast destination, and
 *   connect()+send() also works where sendto() is restricted.
 * - Peer expiry is lazy: lookup()/list() skip entries older than
 *   3x their interval; the listener also reaps while holding the lock.
 * - Times are CLOCK_MONOTONIC milliseconds.
 */

#include "face_tss/discovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DISC_DGRAM_MAX \
    (FACE_TSS_DISCOVERY_MAGIC_LEN + FACE_TSS_DISCOVERY_NAME_MAX + \
     FACE_TSS_DISCOVERY_ADDRESS_MAX + 8)

typedef struct disc_peer {
    char name[FACE_TSS_DISCOVERY_NAME_MAX];
    char address[FACE_TSS_DISCOVERY_ADDRESS_MAX];
    uint64_t interval_ms;
    uint64_t last_seen_ms;
    struct disc_peer *next;
} disc_peer_t;

struct FACE_TSS_DISCOVERY {
    pthread_mutex_t lock;
    pthread_cond_t changed; /* signaled on peer-table change / shutdown */
    pthread_cond_t listener_ready; /* signaled when bind attempted */
    int listener_bound; /* -1 pending, 0 failed, 1 ok */
    int shutdown;

    pthread_t listener_thread;
    int listener_running;
    int listener_sock; /* -1 when not bound */

    pthread_t bcast_thread;
    int bcast_running;
    int bcast_sock; /* -1 when not announcing; connected UDP socket */
    int bcast_stop; /* set to stop the broadcaster thread */
    char bcast_name[FACE_TSS_DISCOVERY_NAME_MAX];
    char bcast_address[FACE_TSS_DISCOVERY_ADDRESS_MAX];
    uint64_t bcast_interval_ms;
    int announcing;

    char destination[16]; /* dotted IPv4; default 255.255.255.255 */

    disc_peer_t *peers;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Little-endian 64-bit store/load (wire format). */
static void put_u64le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v & 0xffu);
        v >>= 8;
    }
}

static uint64_t get_u64le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

/* Build a datagram. Returns length, or 0 if name/address are bad. */
static size_t build_datagram(uint8_t *out, const char *name,
                             const char *address, uint64_t interval_ms)
{
    size_t nl = strlen(name) + 1;
    size_t al = strlen(address) + 1;

    if (nl > FACE_TSS_DISCOVERY_NAME_MAX ||
        al > FACE_TSS_DISCOVERY_ADDRESS_MAX) {
        return 0;
    }
    memcpy(out, FACE_TSS_DISCOVERY_MAGIC, FACE_TSS_DISCOVERY_MAGIC_LEN);
    memcpy(out + FACE_TSS_DISCOVERY_MAGIC_LEN, name, nl);
    memcpy(out + FACE_TSS_DISCOVERY_MAGIC_LEN + nl, address, al);
    put_u64le(out + FACE_TSS_DISCOVERY_MAGIC_LEN + nl + al, interval_ms);
    return FACE_TSS_DISCOVERY_MAGIC_LEN + nl + al + 8;
}

/* Parse a datagram. Returns 1 on success. */
static int parse_datagram(const uint8_t *buf, size_t len, char *name_out,
                          char *address_out, uint64_t *interval_out)
{
    size_t pos, nl, al;

    if (len < FACE_TSS_DISCOVERY_MAGIC_LEN + 1 + 1 + 8) {
        return 0;
    }
    if (memcmp(buf, FACE_TSS_DISCOVERY_MAGIC, FACE_TSS_DISCOVERY_MAGIC_LEN) != 0) {
        return 0;
    }
    pos = FACE_TSS_DISCOVERY_MAGIC_LEN;

    nl = 0;
    while (pos + nl < len && buf[pos + nl] != '\0') {
        nl++;
        if (nl >= FACE_TSS_DISCOVERY_NAME_MAX) {
            return 0;
        }
    }
    if (pos + nl >= len) {
        return 0;
    }
    nl++; /* include NUL */
    memcpy(name_out, buf + pos, nl);
    pos += nl;

    al = 0;
    while (pos + al < len && buf[pos + al] != '\0') {
        al++;
        if (al >= FACE_TSS_DISCOVERY_ADDRESS_MAX) {
            return 0;
        }
    }
    if (pos + al >= len) {
        return 0;
    }
    al++;
    memcpy(address_out, buf + pos, al);
    pos += al;

    if (len < pos + 8) {
        return 0;
    }
    *interval_out = get_u64le(buf + pos);
    if (*interval_out < FACE_TSS_DISCOVERY_INTERVAL_MIN_MS ||
        *interval_out > FACE_TSS_DISCOVERY_INTERVAL_MAX_MS) {
        return 0;
    }
    return 1;
}

static int peer_expired(const disc_peer_t *p, uint64_t now)
{
    return now - p->last_seen_ms >
           p->interval_ms * FACE_TSS_DISCOVERY_EXPIRE_FACTOR;
}

/* Caller holds d->lock. */
static void reap_expired(FACE_TSS_DISCOVERY *d, uint64_t now)
{
    disc_peer_t **pp = &d->peers;

    while (*pp != NULL) {
        if (peer_expired(*pp, now)) {
            disc_peer_t *dead = *pp;
            *pp = dead->next;
            free(dead);
        } else {
            pp = &(*pp)->next;
        }
    }
}

/* Caller holds d->lock. */
static disc_peer_t *find_peer(FACE_TSS_DISCOVERY *d, const char *name,
                              uint64_t now)
{
    disc_peer_t *p;

    for (p = d->peers; p != NULL; p = p->next) {
        if (!peer_expired(p, now) && strcmp(p->name, name) == 0) {
            return p;
        }
    }
    return NULL;
}

/* Caller holds d->lock. Returns 1 if this is our own announcement. */
static int is_self(const FACE_TSS_DISCOVERY *d, const char *name,
                   const char *address)
{
    return d->announcing && strcmp(d->bcast_name, name) == 0 &&
           strcmp(d->bcast_address, address) == 0;
}

/* Caller holds d->lock. */
static void note_peer(FACE_TSS_DISCOVERY *d, const char *name,
                      const char *address, uint64_t interval_ms, uint64_t now)
{
    disc_peer_t *p;

    if (is_self(d, name, address)) {
        return; /* ignore our own broadcasts */
    }
    for (p = d->peers; p != NULL; p = p->next) {
        if (strcmp(p->name, name) == 0) {
            strncpy(p->address, address, sizeof(p->address) - 1);
            p->address[sizeof(p->address) - 1] = '\0';
            p->interval_ms = interval_ms;
            p->last_seen_ms = now;
            pthread_cond_broadcast(&d->changed);
            return;
        }
    }
    p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return;
    }
    strncpy(p->name, name, sizeof(p->name) - 1);
    strncpy(p->address, address, sizeof(p->address) - 1);
    p->interval_ms = interval_ms;
    p->last_seen_ms = now;
    p->next = d->peers;
    d->peers = p;
    pthread_cond_broadcast(&d->changed);
}

static void *listener_main(void *arg)
{
    FACE_TSS_DISCOVERY *d = arg;
    int sock;
    int one = 1;
    struct sockaddr_in bindaddr;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return NULL;
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&bindaddr, 0, sizeof(bindaddr));
    bindaddr.sin_family = AF_INET;
    bindaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindaddr.sin_port = htons(FACE_TSS_DISCOVERY_PORT);
    pthread_mutex_lock(&d->lock);
    if (bind(sock, (struct sockaddr *)&bindaddr, sizeof(bindaddr)) != 0) {
        close(sock);
        d->listener_bound = 0;
        pthread_cond_signal(&d->listener_ready);
        pthread_mutex_unlock(&d->lock);
        return NULL;
    }
    d->listener_sock = sock;
    d->listener_bound = 1;
    pthread_cond_signal(&d->listener_ready);
    pthread_mutex_unlock(&d->lock);

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        int rv;

        pthread_mutex_lock(&d->lock);
        if (d->shutdown) {
            pthread_mutex_unlock(&d->lock);
            break;
        }
        pthread_mutex_unlock(&d->lock);

        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000; /* 200ms poll so shutdown is prompt */
        rv = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (rv > 0 && FD_ISSET(sock, &rfds)) {
            uint8_t buf[DISC_DGRAM_MAX];
            char name[FACE_TSS_DISCOVERY_NAME_MAX];
            char address[FACE_TSS_DISCOVERY_ADDRESS_MAX];
            uint64_t interval;
            ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);

            if (n > 0 &&
                parse_datagram(buf, (size_t)n, name, address, &interval)) {
                pthread_mutex_lock(&d->lock);
                note_peer(d, name, address, interval, now_ms());
                reap_expired(d, now_ms());
                pthread_mutex_unlock(&d->lock);
            }
        }
    }

    close(sock);
    pthread_mutex_lock(&d->lock);
    d->listener_sock = -1;
    pthread_mutex_unlock(&d->lock);
    return NULL;
}

static void *broadcaster_main(void *arg)
{
    FACE_TSS_DISCOVERY *d = arg;
    uint8_t buf[DISC_DGRAM_MAX];
    size_t len;

    pthread_mutex_lock(&d->lock);
    len = build_datagram(buf, d->bcast_name, d->bcast_address,
                         d->bcast_interval_ms);
    pthread_mutex_unlock(&d->lock);
    if (len == 0) {
        return NULL;
    }

    for (;;) {
        uint64_t interval;
        uint64_t slept = 0;

        pthread_mutex_lock(&d->lock);
        if (d->bcast_stop || d->shutdown) {
            pthread_mutex_unlock(&d->lock);
            break;
        }
        interval = d->bcast_interval_ms;
        pthread_mutex_unlock(&d->lock);

        /* Best effort: a dropped datagram just means the next interval. */
        (void)send(d->bcast_sock, buf, len, 0);

        /* Sleep in 50ms slices so stop is prompt. */
        while (slept < interval) {
            struct timespec ts = { 0, 50000000L };
            uint64_t step = interval - slept < 50 ? interval - slept : 50;

            ts.tv_nsec = (long)step * 1000000L;
            nanosleep(&ts, NULL);
            slept += step;
            pthread_mutex_lock(&d->lock);
            if (d->bcast_stop || d->shutdown) {
                pthread_mutex_unlock(&d->lock);
                return NULL;
            }
            pthread_mutex_unlock(&d->lock);
        }
    }
    return NULL;
}

FACE_TSS_DISCOVERY *face_tss_discovery_create(void)
{
    FACE_TSS_DISCOVERY *d = calloc(1, sizeof(*d));

    if (d == NULL) {
        return NULL;
    }
    pthread_mutex_init(&d->lock, NULL);
    {
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&d->changed, &ca);
        pthread_condattr_destroy(&ca);
    }
    pthread_cond_init(&d->listener_ready, NULL);
    d->listener_bound = -1;
    d->listener_sock = -1;
    d->bcast_sock = -1;
    strcpy(d->destination, "255.255.255.255");

    if (pthread_create(&d->listener_thread, NULL, listener_main, d) != 0) {
        pthread_mutex_destroy(&d->lock);
        pthread_cond_destroy(&d->changed);
        pthread_cond_destroy(&d->listener_ready);
        free(d);
        return NULL;
    }
    d->listener_running = 1;

    /* Wait for the listener to bind so that bind order matches create
     * order. With SO_REUSEADDR, the last-bound socket in a process
     * receives each datagram; deterministic ordering keeps multi-object
     * tests reliable. */
    pthread_mutex_lock(&d->lock);
    while (d->listener_bound < 0) {
        pthread_cond_wait(&d->listener_ready, &d->lock);
    }
    pthread_mutex_unlock(&d->lock);
    if (d->listener_bound == 0) {
        /* Bind failed: no listener, tear down. */
        pthread_mutex_lock(&d->lock);
        d->shutdown = 1;
        pthread_mutex_unlock(&d->lock);
        pthread_join(d->listener_thread, NULL);
        pthread_mutex_destroy(&d->lock);
        pthread_cond_destroy(&d->changed);
        pthread_cond_destroy(&d->listener_ready);
        free(d);
        return NULL;
    }
    return d;
}

void face_tss_discovery_destroy(FACE_TSS_DISCOVERY *d)
{
    disc_peer_t *p;

    if (d == NULL) {
        return;
    }
    pthread_mutex_lock(&d->lock);
    d->shutdown = 1;
    d->bcast_stop = 1;
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->lock);

    if (d->bcast_running) {
        pthread_join(d->bcast_thread, NULL);
    }
    if (d->listener_running) {
        pthread_join(d->listener_thread, NULL);
    }
    if (d->bcast_sock >= 0) {
        close(d->bcast_sock);
    }

    for (p = d->peers; p != NULL;) {
        disc_peer_t *next = p->next;
        free(p);
        p = next;
    }
    pthread_mutex_destroy(&d->lock);
    pthread_cond_destroy(&d->changed);
    pthread_cond_destroy(&d->listener_ready);
    free(d);
}

FACE_TSS_RETURN_CODE face_tss_discovery_set_destination(FACE_TSS_DISCOVERY *d,
                                                        const char *destination)
{
    struct sockaddr_in sa;
    int sock;
    int one = 1;

    if (d == NULL || destination == NULL) {
        return FACE_TSS_RC_INVALID_PARAM;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(FACE_TSS_DISCOVERY_PORT);
    if (inet_pton(AF_INET, destination, &sa.sin_addr) != 1) {
        return FACE_TSS_RC_INVALID_PARAM;
    }

    pthread_mutex_lock(&d->lock);
    if (d->announcing) {
        pthread_mutex_unlock(&d->lock);
        return FACE_TSS_RC_INVALID_PARAM;
    }

    /* Probe: the destination must be reachable via connect(). */
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        pthread_mutex_unlock(&d->lock);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(sock);
        pthread_mutex_unlock(&d->lock);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    close(sock);

    strncpy(d->destination, destination, sizeof(d->destination) - 1);
    d->destination[sizeof(d->destination) - 1] = '\0';
    pthread_mutex_unlock(&d->lock);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_discovery_announce(FACE_TSS_DISCOVERY *d,
                                                 const char *name,
                                                 const char *address,
                                                 uint64_t interval_ms)
{
    struct sockaddr_in sa;
    int sock;
    int one = 1;

    if (d == NULL || name == NULL || address == NULL) {
        return FACE_TSS_RC_INVALID_PARAM;
    }
    if (name[0] == '\0' || address[0] == '\0' ||
        strlen(name) >= FACE_TSS_DISCOVERY_NAME_MAX ||
        strlen(address) >= FACE_TSS_DISCOVERY_ADDRESS_MAX ||
        interval_ms < FACE_TSS_DISCOVERY_INTERVAL_MIN_MS ||
        interval_ms > FACE_TSS_DISCOVERY_INTERVAL_MAX_MS) {
        return FACE_TSS_RC_INVALID_PARAM;
    }

    pthread_mutex_lock(&d->lock);
    if (d->announcing) {
        FACE_TSS_RETURN_CODE rc =
            (strcmp(d->bcast_name, name) == 0 &&
             strcmp(d->bcast_address, address) == 0)
                ? FACE_TSS_RC_NO_ACTION
                : FACE_TSS_RC_NOT_AVAILABLE;
        pthread_mutex_unlock(&d->lock);
        return rc;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(FACE_TSS_DISCOVERY_PORT);
    if (inet_pton(AF_INET, d->destination, &sa.sin_addr) != 1) {
        pthread_mutex_unlock(&d->lock);
        return FACE_TSS_RC_INVALID_PARAM;
    }
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) {
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
        if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            close(sock);
            sock = -1;
        }
    }
    if (sock < 0) {
        pthread_mutex_unlock(&d->lock);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }

    strcpy(d->bcast_name, name);
    strcpy(d->bcast_address, address);
    d->bcast_interval_ms = interval_ms;
    d->bcast_sock = sock;
    d->bcast_stop = 0;
    d->announcing = 1;
    if (pthread_create(&d->bcast_thread, NULL, broadcaster_main, d) != 0) {
        close(sock);
        d->bcast_sock = -1;
        d->announcing = 0;
        pthread_mutex_unlock(&d->lock);
        return FACE_TSS_RC_NOT_AVAILABLE;
    }
    d->bcast_running = 1;
    pthread_mutex_unlock(&d->lock);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_discovery_stop_announce(FACE_TSS_DISCOVERY *d)
{
    if (d == NULL) {
        return FACE_TSS_RC_INVALID_PARAM;
    }
    pthread_mutex_lock(&d->lock);
    if (!d->announcing) {
        pthread_mutex_unlock(&d->lock);
        return FACE_TSS_RC_NO_ACTION;
    }
    d->bcast_stop = 1;
    pthread_mutex_unlock(&d->lock);

    pthread_join(d->bcast_thread, NULL);

    pthread_mutex_lock(&d->lock);
    d->bcast_running = 0;
    if (d->bcast_sock >= 0) {
        close(d->bcast_sock);
        d->bcast_sock = -1;
    }
    d->announcing = 0;
    pthread_mutex_unlock(&d->lock);
    return FACE_TSS_RC_NO_ERROR;
}

FACE_TSS_RETURN_CODE face_tss_discovery_lookup(FACE_TSS_DISCOVERY *d,
                                               const char *name,
                                               uint64_t timeout_ms,
                                               char *address_out,
                                               size_t address_len)
{
    uint64_t deadline;
    FACE_TSS_RETURN_CODE rc = FACE_TSS_RC_TIMED_OUT;

    if (d == NULL || name == NULL || address_out == NULL ||
        address_len == 0) {
        return FACE_TSS_RC_INVALID_PARAM;
    }

    pthread_mutex_lock(&d->lock);
    deadline = now_ms() + timeout_ms;
    for (;;) {
        disc_peer_t *p = find_peer(d, name, now_ms());

        if (p != NULL) {
            strncpy(address_out, p->address, address_len - 1);
            address_out[address_len - 1] = '\0';
            rc = FACE_TSS_RC_NO_ERROR;
            break;
        }
        if (timeout_ms == 0) {
            break;
        }
        {
            struct timespec ts;
            uint64_t now = now_ms();
            int wr;

            if (now >= deadline) {
                break;
            }
            ts.tv_sec = (time_t)(deadline / 1000u);
            ts.tv_nsec = (long)(deadline % 1000u) * 1000000L;
            wr = pthread_cond_timedwait(&d->changed, &d->lock, &ts);
            if (wr == ETIMEDOUT) {
                break;
            }
            if (d->shutdown) {
                break;
            }
        }
    }
    pthread_mutex_unlock(&d->lock);
    return rc;
}

FACE_TSS_RETURN_CODE face_tss_discovery_list(FACE_TSS_DISCOVERY *d,
                                             char names[][FACE_TSS_DISCOVERY_NAME_MAX],
                                             char addresses[][FACE_TSS_DISCOVERY_ADDRESS_MAX],
                                             size_t *count, size_t max)
{
    uint64_t now;
    size_t n = 0;
    disc_peer_t *p;

    if (d == NULL || count == NULL) {
        return FACE_TSS_RC_INVALID_PARAM;
    }
    if (max > 0 && (names == NULL || addresses == NULL)) {
        return FACE_TSS_RC_INVALID_PARAM;
    }

    pthread_mutex_lock(&d->lock);
    now = now_ms();
    reap_expired(d, now);
    for (p = d->peers; p != NULL && n < max; p = p->next) {
        strcpy(names[n], p->name);
        strcpy(addresses[n], p->address);
        n++;
    }
    pthread_mutex_unlock(&d->lock);

    *count = n;
    return FACE_TSS_RC_NO_ERROR;
}
