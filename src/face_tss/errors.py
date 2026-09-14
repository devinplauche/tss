"""FACE TSS exceptions - one error type per FACE RETURN_CODE failure."""

from __future__ import annotations

from .types import ReturnCode


class FaceTssError(Exception):
    """Base error carrying the FACE return code a call would have produced."""

    def __init__(self, return_code: ReturnCode, message: str = "") -> None:
        self.return_code = ReturnCode(return_code)
        super().__init__(message or self.return_code.name)


class NotInitializedError(FaceTssError):
    def __init__(self, message: str = "TSS is not initialized") -> None:
        super().__init__(ReturnCode.NOT_AVAILABLE, message)


class InvalidParamError(FaceTssError):
    def __init__(self, message: str = "invalid parameter") -> None:
        super().__init__(ReturnCode.INVALID_PARAM, message)


class InvalidConfigError(FaceTssError):
    def __init__(self, message: str = "invalid configuration") -> None:
        super().__init__(ReturnCode.INVALID_CONFIG, message)


class InvalidModeError(FaceTssError):
    def __init__(self, message: str = "connection direction forbids this operation") -> None:
        super().__init__(ReturnCode.INVALID_MODE, message)


class ConnectionClosedError(FaceTssError):
    def __init__(self, message: str = "connection is closed") -> None:
        super().__init__(ReturnCode.CONNECTION_CLOSED, message)


class TimedOutError(FaceTssError):
    def __init__(self, message: str = "operation timed out") -> None:
        super().__init__(ReturnCode.TIMED_OUT, message)


class MessageStaleError(FaceTssError):
    def __init__(self, message: str = "message is stale") -> None:
        super().__init__(ReturnCode.MESSAGE_STALE, message)


class BufferTooSmallError(FaceTssError):
    def __init__(self, message: str = "supplied buffer is too small") -> None:
        super().__init__(ReturnCode.BUFFER_TOO_SMALL, message)


class TransportError(FaceTssError):
    """The nng transport itself failed (mapped to NO_ACTION / NOT_AVAILABLE)."""

    def __init__(self, message: str = "transport error") -> None:
        super().__init__(ReturnCode.NO_ACTION, message)


__all__ = [
    "FaceTssError",
    "NotInitializedError",
    "InvalidParamError",
    "InvalidConfigError",
    "InvalidModeError",
    "ConnectionClosedError",
    "TimedOutError",
    "MessageStaleError",
    "BufferTooSmallError",
    "TransportError",
]
