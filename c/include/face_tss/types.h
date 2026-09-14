#ifndef FACE_TSS_TYPES_H
#define FACE_TSS_TYPES_H

/*
 * FACE TSS primitive types (C binding).
 *
 * Mirrors the FACE Technical Standard, Edition 3.1, fixed types used by the
 * TSS API (FACE/TSS/Common.idl + FACE/Common.idl): explicit-width integers,
 * UID/timeout/return-code semantics, the connection direction enumeration,
 * and the standard HEADER_TYPE / QoS_EVENT_TYPE shapes.
 *
 * C mapping notes (the standard is normative in IDL; this is the C
 * projection used by this implementation):
 * - FACE::TSS::UID_TYPE            -> FACE_TSS_UID_TYPE (int64_t)
 * - FACE::TSS::CONNECTION_ID_TYPE  -> FACE_TSS_CONNECTION_ID_TYPE
 * - FACE::TSS::TRANSACTION_ID_TYPE -> FACE_TSS_TRANSACTION_ID_TYPE
 * - FACE::TSS::MESSAGE_SIZE_TYPE   -> FACE_TSS_MESSAGE_SIZE_TYPE (int32_t)
 * - FACE::TSS::HEADER_TYPE         -> FACE_TSS_HEADER
 * - FACE::TSS::QoS_EVENT_TYPE      -> FACE_TSS_QOS_EVENT (fixed-capacity)
 * - FACE::RETURN_CODE_TYPE         -> FACE_TSS_RETURN_CODE (all 14 values)
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

/* UIDs: unique within a system (FACE::TSS::UID_TYPE, a long long). */
typedef int64_t FACE_TSS_UID_TYPE;
typedef FACE_TSS_UID_TYPE FACE_TSS_CONNECTION_ID_TYPE;
typedef FACE_TSS_UID_TYPE FACE_TSS_TRANSACTION_ID_TYPE;
/* Message GUID: links a message to its data-model type
 * (FACE::TSS::MESSAGE_GUID_TYPE). */
typedef FACE_TSS_UID_TYPE FACE_TSS_MESSAGE_GUID_TYPE;
typedef int32_t FACE_TSS_MESSAGE_SIZE_TYPE;

#define FACE_TSS_CONNECTION_ID_INVALID ((FACE_TSS_CONNECTION_ID_TYPE)0)
#define FACE_TSS_TRANSACTION_ID_UNSPECIFIED ((FACE_TSS_TRANSACTION_ID_TYPE)0)
#define FACE_TSS_MESSAGE_GUID_UNSPECIFIED ((FACE_TSS_MESSAGE_GUID_TYPE)0)

/* ------------------------------------------------------------------ */
/* Enumerations                                                       */
/* ------------------------------------------------------------------ */

/* FACE::RETURN_CODE_TYPE, all 14 standard values (FACE 3.1, FACE/Common.idl).
 * Order and names match the standard. */
typedef enum FACE_TSS_RETURN_CODE {
    FACE_TSS_RC_NO_ERROR = 0,
    FACE_TSS_RC_NO_ACTION = 1,
    FACE_TSS_RC_NOT_AVAILABLE = 2,
    FACE_TSS_RC_INVALID_PARAM = 3,
    FACE_TSS_RC_INVALID_CONFIG = 4,
    FACE_TSS_RC_INVALID_MODE = 5,
    FACE_TSS_RC_TIMED_OUT = 6,
    FACE_TSS_RC_ADDR_IN_USE = 7,
    FACE_TSS_RC_PERMISSION_DENIED = 8,
    FACE_TSS_RC_MESSAGE_STALE = 9,
    FACE_TSS_RC_IN_PROGRESS = 10,
    FACE_TSS_RC_CONNECTION_CLOSED = 11,
    FACE_TSS_RC_DATA_BUFFER_TOO_SMALL = 12,
    FACE_TSS_RC_DATA_OVERFLOW = 13
} FACE_TSS_RETURN_CODE;

/* FACE connection direction. SOURCE-only sends, DESTINATION-only receives. */
typedef enum FACE_TSS_DIRECTION {
    FACE_TSS_SOURCE = 0,
    FACE_TSS_DESTINATION = 1,
    FACE_TSS_BI_DIRECTIONAL = 2
} FACE_TSS_DIRECTION;

/* Which nng pattern carries a connection (transport selection is a local
 * configuration matter; the standard leaves the underlying transport open). */
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

/* ------------------------------------------------------------------ */
/* Header / QoS                                                       */
/* ------------------------------------------------------------------ */

/* FACE::TSS::HEADER_TYPE: "contains instance UID, source UID, and timestamp". */
typedef struct FACE_TSS_HEADER {
    FACE_TSS_UID_TYPE instance_uid;   /* per-message instance UID */
    FACE_TSS_UID_TYPE source_uid;     /* GUID of the sending TSS instance */
    FACE_SYSTEM_TIME_TYPE timestamp;  /* send time, ns since Unix epoch */
} FACE_TSS_HEADER;

/* FACE::TSS::QoS_Element: a key/value pair. */
typedef struct FACE_TSS_QOS_ELEMENT {
    char keyname[FACE_TSS_MAX_STRING];
    char value[FACE_TSS_MAX_STRING];
} FACE_TSS_QOS_ELEMENT;

/* Maximum QoS elements carried per message. This implementation reports no
 * QoS *policies*; each receive populates one honest, transport-observable
 * element ("message_age_ns"). No staleness policy is enforced. */
#define FACE_TSS_MAX_QOS_ELEMENTS 16

/* FACE::TSS::QoS_EVENT_TYPE: sequence<QoS_Element>. Fixed-capacity C
 * projection to keep the receive path allocation-free. */
typedef struct FACE_TSS_QOS_EVENT {
    FACE_TSS_QOS_ELEMENT elements[FACE_TSS_MAX_QOS_ELEMENTS];
    size_t count;
} FACE_TSS_QOS_EVENT;

/* Zero a QoS event (no elements). */
static inline void face_tss_qos_event_init(FACE_TSS_QOS_EVENT *qos)
{
    if (qos)
        qos->count = 0;
}

#ifdef __cplusplus
}
#endif

#endif /* FACE_TSS_TYPES_H */
