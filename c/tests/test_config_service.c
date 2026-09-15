/* Tests for the FACE::Configuration service (config_service.h). */
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "face_tss/config_service.h"
#include "face_tss/tss.h" /* face_tss_rc_str */

static void t_memory_round_trip(void)
{
    FACE_TSS_CONFIG_SERVICE *svc;
    FACE_TSS_CONFIG_HANDLE h;
    long size, nread;
    char buf[64];
    static const char data[] = "memory-container-payload";

    TEST_BEGIN("memory_round_trip");
    svc = face_tss_config_service_create();
    CHECK(svc != NULL);
    CHECK_RC(face_tss_config_service_initialize(svc, "test"),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_config_service_open(svc, "memory:test", &h),
             FACE_TSS_RC_NO_ERROR);

    /* write then get_size then read back */
    CHECK_RC(face_tss_config_service_write(
                 svc, h, "greeting", data, (long)sizeof(data)),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_config_service_get_size(svc, h, "greeting", &size),
             FACE_TSS_RC_NO_ERROR);
    CHECK(size == (long)sizeof(data));

    memset(buf, 0, sizeof(buf));
    CHECK_RC(face_tss_config_service_read(
                 svc, h, "greeting", buf, (long)sizeof(buf), &nread),
             FACE_TSS_RC_NO_ERROR);
    CHECK(nread == (long)sizeof(data));
    CHECK(memcmp(buf, data, sizeof(data)) == 0);

    /* reading again hits end-of-stream */
    CHECK_RC(face_tss_config_service_read(
                 svc, h, "greeting", buf, (long)sizeof(buf), &nread),
             FACE_TSS_RC_NOT_AVAILABLE);

    /* unknown set -> INVALID_CONFIG */
    CHECK_RC(face_tss_config_service_get_size(svc, h, "nope", &size),
             FACE_TSS_RC_INVALID_CONFIG);

    /* write is memory-only; close then use -> INVALID_CONFIG */
    CHECK_RC(face_tss_config_service_close(svc, h), FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_config_service_get_size(svc, h, "greeting", &size),
             FACE_TSS_RC_INVALID_CONFIG);
    CHECK_RC(face_tss_config_service_close(svc, h),
             FACE_TSS_RC_INVALID_CONFIG);
    face_tss_config_service_destroy(svc);
    TEST_END();
}

static void t_file_backend(void)
{
    FACE_TSS_CONFIG_SERVICE *svc;
    FACE_TSS_CONFIG_HANDLE h;
    char dir[] = "/tmp/face-tss-cfg-XXXXXX";
    char path[256];
    FILE *f;
    long size, nread;
    char buf[64];
    static const char content[] = "0123456789abcdef";

    TEST_BEGIN("file_backend");
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/blob.bin", dir);
    f = fopen(path, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(content, 1, strlen(content), f) == strlen(content));
    fclose(f);

    svc = face_tss_config_service_create();
    CHECK(svc != NULL);
    {
        char container[300];
        snprintf(container, sizeof(container), "file:%s", dir);
        CHECK_RC(face_tss_config_service_open(svc, container, &h),
                 FACE_TSS_RC_NO_ERROR);
    }

    CHECK_RC(face_tss_config_service_get_size(svc, h, "blob.bin", &size),
             FACE_TSS_RC_NO_ERROR);
    CHECK(size == (long)strlen(content));

    /* read first 4 bytes */
    CHECK_RC(face_tss_config_service_read(svc, h, "blob.bin", buf, 4, &nread),
             FACE_TSS_RC_NO_ERROR);
    CHECK(nread == 4);
    CHECK(memcmp(buf, "0123", 4) == 0);

    /* seek back to start, read all */
    CHECK_RC(face_tss_config_service_seek(
                 svc, h, FACE_TSS_CONFIG_SEEK_FROM_START, 0),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_config_service_read(
                 svc, h, "blob.bin", buf, (long)sizeof(buf), &nread),
             FACE_TSS_RC_NO_ERROR);
    CHECK(nread == (long)strlen(content));
    CHECK(memcmp(buf, content, strlen(content)) == 0);

    /* seek from end: last 4 bytes */
    CHECK_RC(face_tss_config_service_seek(
                 svc, h, FACE_TSS_CONFIG_SEEK_FROM_END, -4),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_config_service_read(svc, h, "blob.bin", buf, 4, &nread),
             FACE_TSS_RC_NO_ERROR);
    CHECK(nread == 4);
    CHECK(memcmp(buf, "cdef", 4) == 0);

    /* seek from current: back up 8 -> "89ab" */
    CHECK_RC(face_tss_config_service_seek(
                 svc, h, FACE_TSS_CONFIG_SEEK_FROM_CURRENT, -8),
             FACE_TSS_RC_NO_ERROR);
    CHECK_RC(face_tss_config_service_read(svc, h, "blob.bin", buf, 4, &nread),
             FACE_TSS_RC_NO_ERROR);
    CHECK(nread == 4);
    CHECK(memcmp(buf, "89ab", 4) == 0);

    /* write() on a file session -> INVALID_CONFIG */
    CHECK_RC(face_tss_config_service_write(svc, h, "x", "y", 1),
             FACE_TSS_RC_INVALID_CONFIG);

    CHECK_RC(face_tss_config_service_close(svc, h), FACE_TSS_RC_NO_ERROR);
    face_tss_config_service_destroy(svc);
    unlink(path);
    rmdir(dir);
    TEST_END();
}

static void t_errors(void)
{
    FACE_TSS_CONFIG_SERVICE *svc;
    FACE_TSS_CONFIG_HANDLE h, bad = 123456789;
    long size, nread;
    char buf[16];

    TEST_BEGIN("errors");
    svc = face_tss_config_service_create();
    CHECK(svc != NULL);

    /* NULL parameters -> INVALID_PARAM */
    CHECK_RC(face_tss_config_service_initialize(NULL, "x"),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_config_service_initialize(svc, NULL),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_config_service_open(NULL, "memory:x", &h),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_config_service_open(svc, NULL, &h),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_config_service_open(svc, "memory:x", NULL),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_config_service_close(NULL, 1),
             FACE_TSS_RC_INVALID_PARAM);

    /* unknown backend prefix -> INVALID_CONFIG */
    CHECK_RC(face_tss_config_service_open(svc, "nfs:/x", &h),
             FACE_TSS_RC_INVALID_CONFIG);
    /* missing directory -> INVALID_CONFIG */
    CHECK_RC(face_tss_config_service_open(svc, "file:/no/such/dir", &h),
             FACE_TSS_RC_INVALID_CONFIG);

    CHECK_RC(face_tss_config_service_open(svc, "memory:e", &h),
             FACE_TSS_RC_NO_ERROR);

    /* bad handle -> INVALID_CONFIG */
    CHECK_RC(face_tss_config_service_get_size(svc, bad, "s", &size),
             FACE_TSS_RC_INVALID_CONFIG);
    CHECK_RC(face_tss_config_service_read(svc, bad, "s", buf, 4, &nread),
             FACE_TSS_RC_INVALID_CONFIG);
    CHECK_RC(face_tss_config_service_seek(
                 svc, bad, FACE_TSS_CONFIG_SEEK_FROM_START, 0),
             FACE_TSS_RC_INVALID_CONFIG);
    CHECK_RC(face_tss_config_service_close(svc, bad),
             FACE_TSS_RC_INVALID_CONFIG);
    CHECK_RC(face_tss_config_service_write(svc, bad, "s", "d", 1),
             FACE_TSS_RC_INVALID_CONFIG);

    /* NULL / bad args on a good handle -> INVALID_PARAM */
    CHECK_RC(face_tss_config_service_get_size(svc, h, NULL, &size),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_config_service_get_size(svc, h, "s", NULL),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_config_service_read(svc, h, "s", NULL, 4, &nread),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_config_service_read(svc, h, "s", buf, -1, &nread),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_config_service_read(svc, h, "s", buf, 4, NULL),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_config_service_seek(svc, h, (FACE_TSS_CONFIG_WHENCE)99, 0),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_config_service_seek(
                 svc, h, FACE_TSS_CONFIG_SEEK_FROM_START, -1),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_config_service_seek(
                 svc, h, FACE_TSS_CONFIG_SEEK_FROM_END, 1),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_config_service_write(svc, h, NULL, "d", 1),
             FACE_TSS_RC_INVALID_PARAM);
    CHECK_RC(face_tss_config_service_write(svc, h, "s", NULL, 1),
             FACE_TSS_RC_INVALID_PARAM);
    /* SEEK_FROM_END with no set selected yet -> INVALID_CONFIG */
    CHECK_RC(face_tss_config_service_seek(
                 svc, h, FACE_TSS_CONFIG_SEEK_FROM_END, -1),
             FACE_TSS_RC_INVALID_CONFIG);

    CHECK_RC(face_tss_config_service_close(svc, h), FACE_TSS_RC_NO_ERROR);
    face_tss_config_service_destroy(svc);
    TEST_END();
}

int main(void)
{
    printf("[face_tss_config_service]\n");
    printf("FACE 3.2 Configuration service tests.\n\n");

    t_memory_round_trip();
    t_file_backend();
    t_errors();

    printf("\n%d failures\n", face_tss_test_failures);
    return face_tss_test_failures != 0;
}
