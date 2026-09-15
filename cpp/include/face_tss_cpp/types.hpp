/* FACE 3.2 C++ language mapping: TSS type definitions.
 *
 * Mirrors the FACE 3.2 C++ mapping (FACE::RETURN_CODE_TYPE::Value,
 * FACE::TSS::CONNECTION_ID_TYPE, etc.) as used by the CTS gold standard
 * headers. Self-contained: does not depend on the CTS bundle.
 */

#ifndef FACE_TSS_CPP_TYPES_HPP
#define FACE_TSS_CPP_TYPES_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace FACE {

/* FACE::RETURN_CODE_TYPE (FACE 3.2: 15 values). */
struct RETURN_CODE_TYPE {
    enum Value {
        NO_ERROR = 0,
        NO_ACTION = 1,
        NOT_AVAILABLE = 2,
        INVALID_PARAM = 3,
        INVALID_CONFIG = 4,
        INVALID_MODE = 5,
        TIMED_OUT = 6,
        ADDR_IN_USE = 7,
        PERMISSION_DENIED = 8,
        MESSAGE_STALE = 9,
        IN_PROGRESS = 10,
        CONNECTION_CLOSED = 11,
        DATA_BUFFER_TOO_SMALL = 12,
        DATA_OVERFLOW = 13,
        RESOURCE_LIMIT_REACHED = 14
    };
private:
    RETURN_CODE_TYPE();
};

/* FACE::TIMEOUT_TYPE: nanoseconds; -1 = infinite. */
typedef int64_t TIMEOUT_TYPE;
#ifndef FACE_INF_TIME_VALUE
#define FACE_INF_TIME_VALUE ((FACE::TIMEOUT_TYPE)-1)
#endif

typedef int64_t UID_TYPE;

namespace TSS {

typedef std::string CONNECTION_NAME_TYPE;
typedef uint32_t MESSAGE_SIZE_TYPE;
typedef UID_TYPE CONNECTION_ID_TYPE;
typedef UID_TYPE TRANSACTION_ID_TYPE;
typedef UID_TYPE MESSAGE_GUID_TYPE;

/* Opaque message payload. */
typedef std::vector<uint8_t> MESSAGE_TYPE;

/* QoS event: key/value pair. */
struct QOS_EVENT {
    std::string key;
    int64_t value;
};
typedef std::vector<QOS_EVENT> QOS_EVENT_TYPE;

/* TSS header (subset). */
struct HEADER_TYPE {
    MESSAGE_GUID_TYPE message_guid;
};

} // namespace TSS
} // namespace FACE

#endif /* FACE_TSS_CPP_TYPES_HPP */
