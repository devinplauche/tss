"""FACE data-model primitive types.

Mirrors the FACE Technical Standard fixed types used by the TSS API:
integers/floats with explicit widths, FACE string/GUID/timeout/return-code
semantics, and the connection direction + message-validity enumerations.
"""

from __future__ import annotations

import enum
import time
from dataclasses import dataclass

FACE_MAX_STRING_LENGTH = 256
FACE_MAX_CONNECTION_NAME_LENGTH = 64

CONNECTION_ID_INVALID: int = 0
TRANSACTION_ID_UNSPECIFIED: int = 0


class Direction(enum.IntEnum):
    """FACE::TSS::CONNECTION_DIRECTION_TYPE."""

    SOURCE = 0
    DESTINATION = 1
    BI_DIRECTIONAL = 2


class ReturnCode(enum.IntEnum):
    """FACE::RETURN_CODE_TYPE values used by this TSS."""

    NO_ERROR = 0
    NO_ACTION = 1
    TIMED_OUT = 2
    INVALID_PARAM = 3
    INVALID_CONFIG = 4
    INVALID_MODE = 5
    NOT_AVAILABLE = 6
    CONNECTION_CLOSED = 7
    MESSAGE_STALE = 8
    BUFFER_TOO_SMALL = 9


class MessageValidity(enum.IntEnum):
    """Per-sample validity carried alongside typed receives."""

    VALID = 0
    STALE = 1


#: FACE::TIMEOUT_TYPE is int64 nanoseconds; this sentinel means "wait forever".
TIMEOUT_INFINITE: int = -1

__all__ = [
    "FACE_MAX_STRING_LENGTH",
    "FACE_MAX_CONNECTION_NAME_LENGTH",
    "CONNECTION_ID_INVALID",
    "TRANSACTION_ID_UNSPECIFIED",
    "TIMEOUT_INFINITE",
    "Direction",
    "ReturnCode",
    "MessageValidity",
]


@dataclass(frozen=True)
class Header:
    """FACE::TSS::HEADER_TYPE metadata delivered with each received message."""

    connection_name: str
    transaction_id: int
    source_id: int
    sequence_number: int
    timestamp_ns: int


def now_ns() -> int:
    return time.time_ns()
