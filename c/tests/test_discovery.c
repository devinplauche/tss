/* Tests for UDP-broadcast peer discovery (discovery.h).
 *
 * Sandbox note: UDP broadcast (255.255.255.255) and even sendto() are
 * blocked in this environment, so the tests point the destination at
 * 127.0.0.1 via face_tss_discovery_set_destination(). The wire format
 * and the announce/listen path are identical; only the destination
 * differs.
 *
 * Port-sharing note: two UDP sockets bound to 0.0.0.0:51970 in one
 * process deliver each datagram to exactly one of them (the last-bound
 * socket wins on Linux). The two-object test therefore creates the
 * announcer FIRST and the listener SECOND, so the listener's socket
 * receives the broadcasts. This ordering is deterministic.
 */
#include "test.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "face_tss/discovery.h"
#include "face_tss/tss.h" /* face_tss_rc_str */

#define DEST "127.0.0.1"

static FACE_TSS_DISCOVERY *make_loopback(void)
{
    FACE_TSS_DISCOVERY *d = face_tss_discovery_create();

    if (d == NULL) {
        return NULL;
    }
    if (face_tss_discovery_set_destination(d, DEST) != FACE_TSS_RC_NO_ERROR) {
        face_tss_discovery_destroy(d);
        return NULL;
    }
    return d;
}

static void t_announce_lookup(void)
{
    FACE_TSS_DISCOVERY *announcer, *listener;
    char addr[FACE_TSS_DISCOVERY_ADDRESS_MAX];

    TEST_BEGIN("announce_lookup");
    /* Announcer first: last-bound listener socket wins datagram delivery. */
    announcer = make_loopback();
    CHECK(announcer != NULL);
    listener = make_loopback();
    CHECK(listener != NULL);

    CHECK_RC(face_tss_discovery_announce(announcer, "peer1",
                                         "tcp://127.0.0.1:5001", 100),
             FACE_TSS_RC_NO_ERROR);

    memset(addr, 0, sizeof(addr));
    CHECK_RC(face_tss_discovery_lookup(listener, "peer1", 5000, addr,
                                       sizeof(addr)),
             FACE_TSS_RC_NO_ERROR);
    CHECK(strcmp(addr, "tcp://127.0.0.1:5001") == 0);

    face_tss_discovery_destroy(announcer);
    face_tss_discovery_destroy(listener);
    TEST_END();
}

static void t_poll_miss(void)
{
    FACE_TSS_DISCOVERY *d;
    char addr[FACE_TSS_DISCOVERY_ADDRESS_MAX];

    TEST_BEGIN("poll_miss");
    d = make_loopback();
    CHECK(d != NULL);

    /* Nobody announcing: poll misses immediately. */
    memset(addr, 0, sizeof(addr));
    CHECK_RC(face_tss_discovery_lookup(d, "nobody", 0, addr, sizeof(addr)),
             FACE_TSS_RC_TIMED_OUT);

    /* Short timeout also expires. */
    CHECK_RC(face_tss_discovery_lookup(d, "nobody", 200, addr, sizeof(addr)),
             FACE_TSS_RC_TIMED_OUT);

    face_tss_discovery_destroy(d);
    TEST_END();
}

static void t_list_shows_peer(void)
{
    FACE_TSS_DISCOVERY *announcer, *listener;
    char names[8][FACE_TSS_DISCOVERY_NAME_MAX];
    char addrs[8][FACE_TSS_DISCOVERY_ADDRESS_MAX];
    size_t count = 0;
    char addr[FACE_TSS_DISCOVERY_ADDRESS_MAX];
    int found = 0;

    TEST_BEGIN("list_shows_peer");
    announcer = make_loopback();
    CHECK(announcer != NULL);
    listener = make_loopback();
    CHECK(listener != NULL);

    CHECK_RC(face_tss_discovery_announce(announcer, "peer2",
                                         "tcp://127.0.0.1:5002", 100),
             FACE_TSS_RC_NO_ERROR);
    /* Wait until the peer is visible, then snapshot. */
    CHECK_RC(face_tss_discovery_lookup(listener, "peer2", 5000, addr,
                                       sizeof(addr)),
             FACE_TSS_RC_NO_ERROR);

    CHECK_RC(face_tss_discovery_list(listener, names, addrs, &count, 8),
             FACE_TSS_RC_NO_ERROR);
    CHECK(count >= 1);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], "peer2") == 0 &&
            strcmp(addrs[i], "tcp://127.0.0.1:5002") == 0) {
            found = 1;
        }
    }
    CHECK(found);

    face_tss_discovery_destroy(announcer);
    face_tss_discovery_destroy(listener);
    TEST_END();
}

static void t_stop_expires(void)
{
    FACE_TSS_DISCOVERY *announcer, *listener;
    char addr[FACE_TSS_DISCOVERY_ADDRESS_MAX];
    char names[8][FACE_TSS_DISCOVERY_NAME_MAX];
    char addrs[8][FACE_TSS_DISCOVERY_ADDRESS_MAX];
    size_t count = 0;

    TEST_BEGIN("stop_expires");
    announcer = make_loopback();
    CHECK(announcer != NULL);
    listener = make_loopback();
    CHECK(listener != NULL);

    /* 100ms interval -> entry expires 300ms after the last datagram. */
    CHECK_RC(face_tss_discovery_announce(announcer, "peer3",
                                         "tcp://127.0.0.1:5003", 100),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_discovery_lookup(listener, "peer3", 5000, addr,
                                       sizeof(addr)),
             FACE_TSS_RC_NO_ERROR);

    CHECK_RC(face_tss_discovery_stop_announce(announcer),
             FACE_TSS_RC_NO_ERROR);
    /* Wait well past the 3x100ms expiry, then the peer must be gone. */
    usleep(800000);
    CHECK_RC(face_tss_discovery_lookup(listener, "peer3", 0, addr,
                                       sizeof(addr)),
             FACE_TSS_RC_TIMED_OUT);

    CHECK_RC(face_tss_discovery_list(listener, names, addrs, &count, 8),
             FACE_TSS_RC_NO_ERROR);
    for (size_t i = 0; i < count; i++) {
        CHECK(strcmp(names[i], "peer3") != 0);
    }

    face_tss_discovery_destroy(announcer);
    face_tss_discovery_destroy(listener);
    TEST_END();
}

static void t_invalid_params(void)
{
    FACE_TSS_DISCOVERY *d;
    char addr[FACE_TSS_DISCOVERY_ADDRESS_MAX];
    size_t count;

    TEST_BEGIN("invalid_params");
    d = make_loopback();
    CHECK(d != NULL);

    CHECK_RC(face_tss_discovery_announce(NULL, "n", "a", 100),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_discovery_announce(d, NULL, "a", 100),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_discovery_announce(d, "n", NULL, 100),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_discovery_announce(d, "", "a", 100),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_discovery_announce(d, "n", "", 100),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_discovery_announce(d, "n", "a", 99),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_discovery_announce(d, "n", "a", 60001),
             FACE_TSS_RC_INVALID_PARAM);
    /* Name too long (64 incl. NUL). */
    {
        char longname[FACE_TSS_DISCOVERY_NAME_MAX + 1];
        memset(longname, 'x', sizeof(longname) - 1);
        longname[sizeof(longname) - 1] = '\0';
        CHECK_RC(face_tss_discovery_announce(d, longname, "a", 100),
                 FACE_TSS_RC_INVALID_PARAM);
    }

    /* stop_announce with nothing announcing. */
    CHECK_RC(face_tss_discovery_stop_announce(d), FACE_TSS_RC_NO_ACTION);
    CHECK_RC(face_tss_discovery_stop_announce(NULL),
             FACE_TSS_RC_INVALID_PARAM);

    /* lookup param checks. */
    CHECK_RC(face_tss_discovery_lookup(NULL, "n", 0, addr, sizeof(addr)),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_discovery_lookup(d, NULL, 0, addr, sizeof(addr)),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_discovery_lookup(d, "n", 0, NULL, sizeof(addr)),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_discovery_lookup(d, "n", 0, addr, 0),
             FACE_TSS_RC_INVALID_PARAM);

    /* list param checks. */
    {
        char names[1][FACE_TSS_DISCOVERY_NAME_MAX];
        char addrs[1][FACE_TSS_DISCOVERY_ADDRESS_MAX];
        CHECK_RC(face_tss_discovery_list(NULL, names, addrs, &count, 1),
                 FACE_TSS_RC_INVALID_PARAM);
        CHECK_RC(face_tss_discovery_list(d, names, addrs, NULL, 1),
                 FACE_TSS_RC_INVALID_PARAM);
        CHECK_RC(face_tss_discovery_list(d, NULL, addrs, &count, 1),
                 FACE_TSS_RC_INVALID_PARAM);
        /* max == 0 with NULL arrays is fine. */
        CHECK_RC(face_tss_discovery_list(d, NULL, NULL, &count, 0),
                 FACE_TSS_RC_NO_ERROR);
        CHECK(count == 0);
    }

    /* set_destination param checks. */
    CHECK_RC(face_tss_discovery_set_destination(NULL, DEST),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_discovery_set_destination(d, NULL),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_discovery_set_destination(d, "not-an-ip"),
             FACE_TSS_RC_INVALID_PARAM);

    face_tss_discovery_destroy(d);
    TEST_END();
}

static void t_double_announce(void)
{
    FACE_TSS_DISCOVERY *d;

    TEST_BEGIN("double_announce");
    d = make_loopback();
    CHECK(d != NULL);

    CHECK_RC(face_tss_discovery_announce(d, "me", "tcp://127.0.0.1:5004",
                                         100),
             FACE_TSS_RC_NO_ERROR);
    /* Same name+address -> NO_ACTION. */
    CHECK_RC(face_tss_discovery_announce(d, "me", "tcp://127.0.0.1:5004",
                                         200),
             FACE_TSS_RC_NO_ACTION);
    /* Different address -> NOT_AVAILABLE (one announcement per object). */
    CHECK_RC(face_tss_discovery_announce(d, "me", "tcp://127.0.0.1:5005",
                                         100),
             FACE_TSS_RC_NOT_AVAILABLE);
    /* Different name -> NOT_AVAILABLE too. */
    CHECK_RC(face_tss_discovery_announce(d, "other", "tcp://127.0.0.1:5004",
                                         100),
             FACE_TSS_RC_NOT_AVAILABLE);
    /* set_destination while announcing -> INVALID_PARAM. */
    CHECK_RC(face_tss_discovery_set_destination(d, DEST),
             FACE_TSS_RC_INVALID_PARAM);

    CHECK_RC(face_tss_discovery_stop_announce(d), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_discovery_stop_announce(d), FACE_TSS_RC_NO_ACTION);
    /* After stopping, announcing again works. */
    CHECK_RC(face_tss_discovery_announce(d, "me2", "tcp://127.0.0.1:5006",
                                         100),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_discovery_stop_announce(d), FACE_TSS_RC_NO_ERROR);

    face_tss_discovery_destroy(d);
    TEST_END();
}

int main(void)
{
    printf("[face_tss_discovery]\n");
    printf("FACE TSS UDP-broadcast peer discovery tests.\n\n");

    t_announce_lookup();
    t_poll_miss();
    t_list_shows_peer();
    t_stop_expires();
    t_invalid_params();
    t_double_announce();

    printf("\n%d failures\n", face_tss_test_failures);
    return face_tss_test_failures != 0;
}
