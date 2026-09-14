#ifndef FACE_TSS_TYPES_H
#define FACE_TSS_TYPES_H

/*
 * FACE TSS primitive types (C binding).
 *
 * Mirrors the FACE Technical Standard fixed types used by the TSS API:
 * explicit-width integers, FACE string/GUID/timeout/return-code semantics,
 * and the connection direction + message-validity enumerations.
 *
 * This header is dependency-free (stdint.h / stdbool.h / stddef.h only).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Scalars                                                            */
/* ------------------------------------------------------------------ */

typedef int8_t   FACE_Char;
typedef uint8_t  FACE_Octet;
typedef int16_t  FACE_Short;
typedef uint16_t FACE_UnsignedShort;
typedef int32_t  FACE_Long;
typedef uint32_t FACE_UnsignedLong;
typedef int64_t  FACE_LongLong;
typedef uint64_t FACE_UnsignedLongLong;
typedef float    FACE_Float;
typedef double   FACE_Double;
typedef bool     FACE_Boolean;

/* 64-bit signed integer with 1 nanosecond resolution. */
typedef int64_t FACE_SYSTEM_TIME_TYPE;
/* "wait forever" timeout sentinel. */
#define FACE_INF_TIME_VALUE ((FACE_SYSTEM_TIME_TYPE)-1)

/* Timeout type: int64 nanoseconds; FACE_INF_TIME_VALUE waits forever. */
typedef FACE_SYSTEM_TIME_TYPE FACE_TIMEOUT_TYPE;
#define FACE_TSS_TIMEOUT_INFINITE FACE_INF_TIME_VALUE

/* FACE fixed string sizes. */
#define FACE_TSS_MAX_CONNECTION_NAME 64
#define FACE_TSS_MAX_STRING 256
#define FACE_TSS_MAX_ADDRESS 128

/* GUIDs and identifiers. */
typedef int64_t FACE_TSS_CONNECTION_ID_TYPE;
typedef int64_t FACE_TSS_TRANSACTION_ID_TYPE;
typedef int64_t FACE_TSS_GUID_TYPE;
typedef int32_t FACE_TSS_MESSAGE_SIZE_TYPE;

#define FACE_TSS_CONNECTION_ID_INVALID ((FACE_TSS_CONNECTION_ID_TYPE)0)
#define FACE_TSS_TRANSACTION_ID_UNSPECIFIED ((FACE_TSS_TRANSACTION_ID_TYPE)0)

/* ------------------------------------------------------------------ */
/* Enumerations                                                       */
/* ------------------------------------------------------------------ */

typedef enum FACE_TSS_RETURN_CODE {
    FACE_TSS_RC_NO_ERROR = 0,
    FACE_TSS_RC_NO_ACTION = 1,
    FACE_TSS_RC_TIMED_OUT = 2,
    FACE_TSS_RC_INVALID_PARAM = 3,
    FACE_TSS_RC_INVALID_CONFIG = 4,
    FACE_TSS_RC_INVALID_MODE = 5,
    FACE_TSS_RC_NOT_AVAILABLE = 6,
    FACE_TSS_RC_CONNECTION_CLOSED = 7,
    FACE_TSS_RC_MESSAGE_STALE = 8,
    FACE_TSS_RC_BUFFER_TOO_SMALL = 9
} FACE_TSS_RETURN_CODE;

/* FACE connection direction. SOURCE-only sends, DESTINATION-only receives. */
typedef enum FACE_TSS_DIRECTION {
    FACE_TSS_SOURCE = 0,
    FACE_TSS_DESTINATION = 1,
    FACE_TSS_BI_DIRECTIONAL = 2
} FACE_TSS_DIRECTION;

/* Which nng pattern carries a connection. */
typedef enum FACE_TSS_TRANSPORT_KIND {
    FACE_TSS_TRANSPORT_PUBSUB = 0,
    FACE_TSS_TRANSPORT_BUS = 1
} FACE_TSS_TRANSPORT_KIND;

/* Pub/Sub socket role. Bus ignores the role. */
typedef enum FACE_TSS_ROLE {
    FACE_TSS_ROLE_PUBLISHER = 0,
    FACE_TSS_ROLE_SUBSCRIBER = 1,
    FACE_TSS_ROLE_BUS = 2
} FACE_TSS_ROLE;

/* Per-sample validity delivered with each received message. */
typedef enum FACE_TSS_VALIDITY {
    FACE_TSS_VALID = 0,
    FACE_TSS_STALE = 1
} FACE_TSS_VALIDITY;

/* ------------------------------------------------------------------ */
/* Header                                                             */
/* ------------------------------------------------------------------ */

/* FACE::TSS::HEADER_TYPE metadata delivered with each received message. */
typedef struct FACE_TSS_HEADER {
    char connection_name[FACE_TSS_MAX_CONNECTION_NAME];
    FACE_TSS_TRANSACTION_ID_TYPE transaction_id;
    FACE_TSS_GUID_TYPE source_id;
    uint64_t sequence_number;
    FACE_SYSTEM_TIME_TYPE timestamp_ns;
    FACE_TSS_VALIDITY validity;
} FACE_TSS_HEADER;

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_TYPES_H */
