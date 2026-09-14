/* Minimal test harness: register + run, abort on first failure. */
#ifndef FACE_TSS_TEST_H
#define FACE_TSS_TEST_H

#include <stdio.h>

static int face_tss_test_failures = 0;
static const char *face_tss_test_current = "";

#define TEST_BEGIN(name) \
    do { face_tss_test_current = (name); printf("  %-46s", (name)); fflush(stdout); } while (0)
#define TEST_END() \
    do { printf("ok\n"); } while (0)
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL\n    %s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            face_tss_test_failures++; \
            return; \
        } \
    } while (0)
#define CHECK_RC(expr, want) \
    do { \
        FACE_TSS_RETURN_CODE _rc = (expr); \
        if (_rc != (want)) { \
            printf("FAIL\n    %s:%d: %s -> %s (%d), want %s (%d)\n", __FILE__, __LINE__, \
                #expr, face_tss_rc_str(_rc), (int)_rc, \
                face_tss_rc_str((want)), (int)(want)); \
            face_tss_test_failures++; \
            return; \
        } \
    } while (0)

#define TEST_SUMMARY() \
    (printf(face_tss_test_failures ? "FAILED (%d)\n" : "PASSED\n", \
        face_tss_test_failures), face_tss_test_failures)

#endif
