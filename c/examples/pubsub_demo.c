/* Pub/Sub demo: publisher streams, subscriber prints.
 * The publisher owns the listen side: start it first.
 *   face_tss_pubsub pub [config.json]
 *   face_tss_pubsub sub [config.json]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
static void msleep(long ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void msleep(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

#include "face_tss/config.h"
#include "face_tss/tss.h"

static int run_pub(const char *cfg_path)
{
    FACE_TSS_CONFIG cfg;
    FACE_TSS *tss;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    FACE_TSS_RETURN_CODE rc;
    int i;
    face_tss_config_init(&cfg, "demo");
    rc = face_tss_config_from_file(cfg_path, &cfg);
    if (rc != FACE_TSS_RC_NO_ERROR) {
        printf("[pub] config %s: %s\n", cfg_path, face_tss_rc_str(rc));
        return 1;
    }
    tss = face_tss_create("demo-publisher");
    rc = face_tss_initialize(tss, &cfg);
    printf("[pub] initialize -> %s\n", face_tss_rc_str(rc));
    rc = face_tss_create_connection(tss, "POSITION", &id, &mx, 0);
    printf("[pub] create POSITION -> %s id=%lld max=%d\n",
           face_tss_rc_str(rc), (long long)id, (int)mx);
    if (rc != FACE_TSS_RC_NO_ERROR)
        return 1;
    msleep(500);
    for (i = 1; i <= 20; i++) {
        char body[64];
        int n = snprintf(body, sizeof(body), "position-%d", i);
        rc = face_tss_send_message(tss, id, (const uint8_t *)body,
                                   (size_t)n, i);
        printf("[pub] sent #%d -> %s\n", i, face_tss_rc_str(rc));
        msleep(200);
    }
    face_tss_destroy_connection(tss, id);
    face_tss_destroy(tss);
    face_tss_config_fini(&cfg);
    printf("[pub] done\n");
    return 0;
}

static int run_sub(const char *cfg_path)
{
    FACE_TSS_CONFIG cfg;
    FACE_TSS *tss;
    FACE_TSS_CONNECTION_ID_TYPE id;
    FACE_TSS_MESSAGE_SIZE_TYPE mx;
    FACE_TSS_RETURN_CODE rc;
    int i, received = 0;
    face_tss_config_init(&cfg, "demo");
    rc = face_tss_config_from_file(cfg_path, &cfg);
    if (rc != FACE_TSS_RC_NO_ERROR) {
        printf("[sub] config %s: %s\n", cfg_path, face_tss_rc_str(rc));
        return 1;
    }
    tss = face_tss_create("demo-subscriber");
    rc = face_tss_initialize(tss, &cfg);
    printf("[sub] initialize -> %s\n", face_tss_rc_str(rc));
    rc = face_tss_create_connection(tss, "POSITION", &id, &mx, 0);
    printf("[sub] create POSITION -> %s\n", face_tss_rc_str(rc));
    if (rc != FACE_TSS_RC_NO_ERROR)
        return 1;
    for (i = 0; i < 30; i++) {
        FACE_TSS_MESSAGE m;
        memset(&m, 0, sizeof(m));
        rc = face_tss_receive_message(tss, id, 500 * 1000000LL, 0, &m);
        if (rc == FACE_TSS_RC_TIMED_OUT)
            continue;
        if (rc != FACE_TSS_RC_NO_ERROR) {
            printf("[sub] receive -> %s\n", face_tss_rc_str(rc));
            break;
        }
        received++;
        printf("[sub] #%d txn=%lld seq=%llu payload=%.*s\n", received,
               (long long)m.header.transaction_id,
               (unsigned long long)m.header.sequence_number,
               (int)m.payload_len, (const char *)m.payload);
        face_tss_message_fini(&m);
    }
    face_tss_destroy_connection(tss, id);
    face_tss_destroy(tss);
    face_tss_config_fini(&cfg);
    printf("[sub] done received=%d\n", received);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode, *cfg;
    if (argc < 2 || (strcmp(argv[1], "pub") && strcmp(argv[1], "sub"))) {
        printf("usage: %s [pub|sub] [config.json]\n", argv[0]);
        return 2;
    }
    mode = argv[1];
    if (argc > 2) {
        cfg = argv[2];
    } else if (!strcmp(mode, "pub")) {
        cfg = "configs/pubsub_publisher.json";
    } else {
        cfg = "configs/pubsub_subscriber.json";
    }
    return !strcmp(mode, "pub") ? run_pub(cfg) : run_sub(cfg);
}
