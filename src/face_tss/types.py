"""FACE data-model primitive types.

Mirrors the FACE Technical Standard v3.1 fixed types used by the TSS API:
integers/floats with explicit widths, FACE string/GUID/timeout/return-code
semantics, the connection direction + message-validity enumerations, the
HEADER_TYPE projection (instance_uid / source_uid / timestamp), and the
fixed-capacity QoS_EVENT_TYPE projection.

Field-for-field this mirrors ``c/include/face_tss/types.h``.
"""

from __future__ import annotations

import enum
import time
from dataclasses import dataclass, field

FACE_MAX_STRING_LENGTH = 256
FACE_MAX_CONNECTION_NAME_LENGTH = 64

#: Fixed capacity of the QoS_EVENT_TYPE projection (mirrors C).
FACE_TSS_QOS_EVENT_MAX = 8

CONNECTION_ID_INVALID: int = 0
TRANSACTION_ID_UNSPECIFIED: int = 0
MESSAGE_GUID_INVALID: int = 0


class Direction(enum.IntEnum):
    """FACE::TSS::CONNECTION_DIRECTION_TYPE."""

    SOURCE = 0
    DESTINATION = 1
    BI_DIRECTIONAL = 2


class ReturnCode(enum.IntEnum):
    """FACE 3.1 RETURN_CODE_TYPE - all 14 values, in standard order."""

    NO_ERROR = 0
    NO_ACTION = 1
    NOT_AVAILABLE = 2
    INVALID_PARAM = 3
    INVALID_CONFIG = 4
    INVALID_MODE = 5
    TIMED_OUT = 6
    ADDR_IN_USE = 7
    PERMISSION_DENIED = 8
    MESSAGE_STALE = 9
    IN_PROGRESS = 10
    CONNECTION_CLOSED = 11
    DATA_BUFFER_TOO_SMALL = 12
    DATA_OVERFLOW = 13


class MessageValidity(enum.IntEnum):
    """Per-sample validity carried alongside typed receives."""

    VALID = 0
    STALE = 1


#: FACE::TIMEOUT_TYPE is int64 nanoseconds; this sentinel means "wait forever".
TIMEOUT_INFINITE: int = -1


@dataclass(frozen=True)
class Header:
    """FACE::TSS::HEADER_TYPE projection delivered with each message.

    instance_uid identifies the sending TSS instance's current message
    (unique per send within the instance); source_uid identifies the
    sending TSS instance itself; timestamp is send time, ns since epoch.
    """

    instance_uid: int
    source_uid: int
    timestamp: int


@dataclass(frozen=True)
class QosElement:
    """One QoS name/value pair inside a QoS_EVENT_TYPE."""

    name: str
    value: int


class QosEvent(list):
    """FACE::TSS::QoS_EVENT_TYPE projection: fixed-capacity QoS elements.

    Behaves like a list of QosElement capped at FACE_TSS_QOS_EVENT_MAX
    entries. Receives populate one element, ``message_age_ns``; no QoS
    policies are enforced (see issue #2).
    """

    def __init__(self, elements: list[QosElement] | None = None) -> None:
        items = list(elements) if elements else []
        if len(items) > FACE_TSS_QOS_EVENT_MAX:
            raise ValueError(
                f"QoS event holds at most {FACE_TSS_QOS_EVENT_MAX} elements"
            )
        super().__init__(items)

    def append(self, element: QosElement) -> None:  # type: ignore[override]
        if len(self) >= FACE_TSS_QOS_EVENT_MAX:
            raise ValueError("QoS event is full")
        super().append(element)


def now_ns() -> int:
    return time.time_ns()


__all__ = [
    "FACE_MAX_STRING_LENGTH",
    "FACE_MAX_CONNECTION_NAME_LENGTH",
    "FACE_TSS_QOS_EVENT_MAX",
    "CONNECTION_ID_INVALID",
    "TRANSACTION_ID_UNSPECIFIED",
    "MESSAGE_GUID_INVALID",
    "TIMEOUT_INFINITE",
    "Direction",
    "ReturnCode",
    "MessageValidity",
    "Header",
    "QosElement",
    "QosEvent",
    "now_ns",
]
