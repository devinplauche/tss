"""FACE 3.2 TPM (Transport Protocol Module) - pure-Python implementation.

Mirrors ``c/include/face_tss/tpm.h`` / ``c/src/tpm.c``: a local,
channel-based loopback transport. Written messages become readable on
the same channel (loopback, for testing); there is no real I/O.

Threading mirrors the C implementation: a single re-entrant condition
variable guards all state. ``write_to_transport``, ``close_channel``
and ``request_state_change`` broadcast to wake threads blocked in
``is_data_available`` / ``read_from_transport``; ``destroy()`` wakes
every blocked waiter and does not return until they have drained.

Timeouts are nanoseconds like the FACE ``TIMEOUT_TYPE``:
``TIMEOUT_INFINITE`` (-1) blocks forever, 0 polls once, a positive value
waits up to that long. Clocks are monotonic (``time.monotonic_ns``).
"""

from __future__ import annotations

import enum
import threading
import time

from .errors import (
    ConnectionClosedError,
    FaceTssError,
    InvalidModeError,
    InvalidParamError,
    NotInitializedError,
    ResourceLimitError,
    TimedOutError,
)
from .types import TIMEOUT_INFINITE, ReturnCode

#: FACE::TSS::TPM::CHANNEL_ID_TYPE invalid value.
CHANNEL_ID_INVALID: int = 0

#: Maximum simultaneously open channels (mirrors TPM_MAX_CHANNELS in C).
TPM_MAX_CHANNELS: int = 16

#: Largest single message the loopback transport accepts (mirrors TPM_MAX_MSG).
TPM_MAX_MSG: int = 65536


class TpmEventType(enum.IntEnum):
    """FACE::TSS::TPM::EVENT_TYPE."""

    INIT_COMPLETE = 0
    XPORT_DEGRADED = 1
    CBIT_FAIL = 2
    IBIT_FAIL = 3
    CHANNEL_FAIL = 4
    LOST_LINK = 5
    TRANSMIT_COMPLETE = 6


class TpmStateType(enum.IntEnum):
    """FACE::TSS::TPM::TPMTS::TPM_STATE_TYPE."""

    NORMAL = 0
    TEST = 1
    RESUME = 2
    PAUSE = 3
    SHUTDOWN = 4
    SECURE = 5


class TpmLevelOfTestType(enum.IntEnum):
    """FACE::TSS::TPM::TPMTS::LEVEL_OF_TEST_TYPE."""

    CBIT = 0
    IBIT = 1
    PBIT = 2


class TpmCallbackKind(enum.IntEnum):
    """FACE::TSS::TPM::TPM_Callback::CALLBACK_KIND_TYPE."""

    DATA = 0
    EVENT = 1
    BOTH = 2


class _Channel:
    """One TPM channel slot (mirrors tpm_channel_t in C)."""

    __slots__ = (
        "endpoint_name",
        "in_use",
        "open",
        "pending_msg",
        "pending_txn",
        "data_cb",
        "event_cb",
        "cb_user",
        "cb_kind",
        "cb_registered",
    )

    def __init__(self) -> None:
        self.endpoint_name = ""
        self.in_use = False
        self.open = False
        self.pending_msg: bytes | None = None
        self.pending_txn: int = 0
        self.data_cb = None
        self.event_cb = None
        self.cb_user = None
        self.cb_kind = TpmCallbackKind.DATA
        self.cb_registered = False


class Tpm:
    """A FACE 3.2 TPM instance (pure-Python mirror of ``FACE_TSS_TPM``).

    Create with :meth:`create`, tear down with :meth:`destroy` (also
    usable as a context manager). Entry points raise the
    :mod:`face_tss.errors` exceptions; idempotent duplicate operations
    (double initialize / double callback register / double unregister)
    return :attr:`ReturnCode.NO_ACTION` like the C implementation.
    """

    def __init__(self, name: str = "tpm") -> None:
        self._name = name or ""
        self._initialized = False
        self._state = TpmStateType.NORMAL
        self._status = TpmEventType.INIT_COMPLETE
        self._channels = [_Channel() for _ in range(TPM_MAX_CHANNELS)]
        # Condition (over an RLock) guarding every field below.
        self._cond = threading.Condition(threading.RLock())
        # Set by destroy(); afterwards every entry point fails with
        # NOT_AVAILABLE and blocked waiters are woken.
        self._destroying = False
        # Threads currently inside a cond_wait; destroy() waits for this
        # to drain before returning (mirrors tpm->waiters in C).
        self._waiters = 0

    # -- lifecycle ------------------------------------------------------
    @classmethod
    def create(cls, name: str = "tpm") -> "Tpm":
        """Create a TPM instance (mirrors ``face_tss_tpm_create``)."""
        return cls(name)

    def destroy(self) -> None:
        """Tear the TPM down (mirrors ``face_tss_tpm_destroy``).

        Wakes every thread blocked in :meth:`is_data_available` /
        :meth:`read_from_transport` (they raise
        :class:`FaceTssError` with ``ReturnCode.NOT_AVAILABLE``) and
        does not return until those waiters have drained.
        """
        with self._cond:
            if self._destroying:
                return
            self._destroying = True
            self._cond.notify_all()
            while self._waiters > 0:
                self._cond.wait()
            for ch in self._channels:
                ch.in_use = False
                ch.open = False
                ch.pending_msg = None
                ch.cb_registered = False

    def __enter__(self) -> "Tpm":
        return self

    def __exit__(self, *exc: object) -> None:
        self.destroy()

    # -- internal helpers -------------------------------------------------
    def _check_alive(self) -> None:
        """Raise NOT_AVAILABLE if destroy() has been called."""
        if self._destroying:
            raise FaceTssError(ReturnCode.NOT_AVAILABLE, "TPM is destroyed")

    def _check_initialized(self) -> None:
        self._check_alive()
        if not self._initialized:
            raise NotInitializedError("TPM is not initialized")

    def _find_channel(self, channel_id: int) -> _Channel | None:
        """Caller must hold the condition. Mirrors find_channel()."""
        if 1 <= channel_id <= TPM_MAX_CHANNELS:
            ch = self._channels[channel_id - 1]
            if ch.in_use:
                return ch
        return None

    @staticmethod
    def _deadline_ns(timeout_ns: int) -> tuple[bool, int]:
        """Split a FACE timeout into (infinite, monotonic deadline ns)."""
        infinite = timeout_ns == TIMEOUT_INFINITE
        wait_ns = timeout_ns if timeout_ns > 0 else 0
        return infinite, time.monotonic_ns() + wait_ns

    def _wait_until(self, deadline_ns: int, infinite: bool) -> None:
        """Wait for a broadcast or the deadline. Caller holds the condition.

        Mirrors tpm_wait_until(): counted in _waiters so destroy() can
        wait for waiters to drain; the last waiter to wake notifies a
        destroy() that is waiting.
        """
        self._waiters += 1
        try:
            if infinite:
                self._cond.wait()
            else:
                remaining = (deadline_ns - time.monotonic_ns()) / 1e9
                if remaining > 0:
                    self._cond.wait(timeout=remaining)
        finally:
            self._waiters -= 1
            if self._destroying and self._waiters == 0:
                self._cond.notify_all()

    # -- FACE::TSS::TPM::TPMTS --------------------------------------------
    def initialize(self, configuration_resource: str = "") -> ReturnCode:
        """Initialize the TPM (mirrors ``face_tss_tpm_initialize``).

        Idempotent: the second call returns ``ReturnCode.NO_ACTION``.
        """
        with self._cond:
            self._check_alive()
            if self._initialized:
                return ReturnCode.NO_ACTION
            self._initialized = True
            self._status = TpmEventType.INIT_COMPLETE
            return ReturnCode.NO_ERROR

    def open_channel(
        self,
        endpoint_name: str,
        transport_config: bytes | None = None,
        security_config: bytes | None = None,
    ) -> int:
        """Open a channel (mirrors ``face_tss_tpm_open_channel``).

        Returns the new channel id. Raises :class:`ResourceLimitError`
        when all channel slots are in use, :class:`InvalidModeError`
        while the TPM is shut down.
        """
        if not endpoint_name:
            raise InvalidParamError("endpoint_name is required")
        with self._cond:
            self._check_initialized()
            if self._state == TpmStateType.SHUTDOWN:
                raise InvalidModeError("TPM is shut down")
            for i, ch in enumerate(self._channels):
                if not ch.in_use:
                    ch.in_use = True
                    ch.open = True
                    ch.endpoint_name = endpoint_name
                    ch.pending_msg = None
                    ch.cb_registered = False
                    return i + 1
            raise ResourceLimitError("too many open TPM channels")

    def close_channel(self, channel_id: int) -> ReturnCode:
        """Close a channel (mirrors ``face_tss_tpm_close_channel``).

        Wakes threads blocked in :meth:`is_data_available` /
        :meth:`read_from_transport` so they observe the closed channel.
        """
        with self._cond:
            self._check_initialized()
            ch = self._find_channel(channel_id)
            if ch is None:
                raise InvalidParamError(
                    f"invalid TPM channel id {channel_id}"
                )
            ch.pending_msg = None
            ch.in_use = False
            ch.open = False
            ch.cb_registered = False
            ch.data_cb = None
            ch.event_cb = None
            self._cond.notify_all()
            return ReturnCode.NO_ERROR

    def request_state_change(
        self, new_state: TpmStateType | int, data: bytes | None = None
    ) -> ReturnCode:
        """Request a TPM state change (mirrors the C function).

        Wakes blocked waiters so they observe the new state.
        """
        try:
            state = TpmStateType(new_state)
        except ValueError:
            raise InvalidParamError(
                f"invalid TPM state {new_state}"
            ) from None
        with self._cond:
            self._check_initialized()
            self._state = state
            self._cond.notify_all()
            return ReturnCode.NO_ERROR

    @property
    def state(self) -> TpmStateType:
        """Current TPM state (not part of the FACE IDL; test aid)."""
        with self._cond:
            return self._state

    def is_data_available(
        self, channel_ids: list[int] | tuple[int, ...], timeout_ns: int
    ) -> list[int]:
        """Channels (in the order given) with a pending message.

        Mirrors ``face_tss_tpm_is_data_available``: ``TIMEOUT_INFINITE``
        blocks until data arrives, 0 polls once. Returns the available
        channel ids; an empty list on timeout (no exception).
        """
        if channel_ids is None:
            raise InvalidParamError("channel_ids is required")
        infinite, deadline_ns = self._deadline_ns(timeout_ns)
        with self._cond:
            self._check_initialized()
            while True:
                available = [
                    cid
                    for cid in channel_ids
                    if (ch := self._find_channel(cid)) is not None
                    and ch.pending_msg is not None
                ]
                if available:
                    return available
                if self._destroying:
                    raise FaceTssError(
                        ReturnCode.NOT_AVAILABLE, "TPM is destroyed"
                    )
                if not infinite and time.monotonic_ns() >= deadline_ns:
                    return []
                # Spurious wakeups are harmless: the loop re-scans.
                self._wait_until(deadline_ns, infinite)

    def get_status(self) -> TpmEventType:
        """Current TPM status event (mirrors ``face_tss_tpm_get_status``)."""
        with self._cond:
            self._check_initialized()
            return self._status

    def read_from_transport(
        self, channel_id: int, timeout_ns: int
    ) -> tuple[int, bytes]:
        """Read one message (mirrors ``face_tss_tpm_read_from_transport``).

        Returns ``(transaction_id, message)``. Raises
        :class:`TimedOutError` when the timeout expires with no data.
        """
        infinite, deadline_ns = self._deadline_ns(timeout_ns)
        with self._cond:
            self._check_initialized()
            while True:
                ch = self._find_channel(channel_id)
                if ch is None:
                    raise InvalidParamError(
                        f"invalid TPM channel id {channel_id}"
                    )
                if not ch.open:
                    raise ConnectionClosedError("TPM channel is closed")
                if ch.pending_msg is not None:
                    msg = ch.pending_msg
                    txn = ch.pending_txn
                    ch.pending_msg = None
                    return txn, msg
                if self._destroying:
                    raise FaceTssError(
                        ReturnCode.NOT_AVAILABLE, "TPM is destroyed"
                    )
                if not infinite and time.monotonic_ns() >= deadline_ns:
                    raise TimedOutError("TPM read timed out")
                # Spurious wakeups are harmless: the loop re-checks.
                self._wait_until(deadline_ns, infinite)

    def write_to_transport(
        self,
        channel_id: int,
        message: bytes,
        transaction_id: int = 0,
        max_delay_ns: int = TIMEOUT_INFINITE,
    ) -> ReturnCode:
        """Write a message (mirrors ``face_tss_tpm_write_to_transport``).

        Loopback: the message becomes readable on the same channel and
        blocked waiters are woken. A registered data callback fires
        after the lock is released (it may re-enter the TPM).
        """
        if message is None:
            raise InvalidParamError("message is required")
        msg = bytes(message)
        if len(msg) > TPM_MAX_MSG:
            raise FaceTssError(
                ReturnCode.DATA_OVERFLOW,
                f"message of {len(msg)} bytes exceeds {TPM_MAX_MSG}",
            )
        with self._cond:
            self._check_initialized()
            ch = self._find_channel(channel_id)
            if ch is None:
                raise InvalidParamError(
                    f"invalid TPM channel id {channel_id}"
                )
            if not ch.open:
                raise ConnectionClosedError("TPM channel is closed")
            # Loopback: deliver to the channel's pending queue.
            ch.pending_msg = msg
            ch.pending_txn = transaction_id
            data_cb = ch.data_cb
            cb_user = ch.cb_user
            cb_kind = ch.cb_kind
            cb_registered = ch.cb_registered
            self._cond.notify_all()
        # Fire the data callback outside the lock (mirrors C: the
        # callback may re-enter the TPM).
        if (
            cb_registered
            and data_cb is not None
            and cb_kind in (TpmCallbackKind.DATA, TpmCallbackKind.BOTH)
        ):
            data_cb(channel_id, transaction_id, msg, cb_user)
        return ReturnCode.NO_ERROR

    def register_callback(
        self,
        channel_id: int,
        kind: TpmCallbackKind | int = TpmCallbackKind.DATA,
        data_cb=None,
        event_cb=None,
        user=None,
    ) -> ReturnCode:
        """Register data/event callbacks (mirrors the C function).

        ``data_cb(channel_id, transaction_id, message, user)`` fires on
        each write; ``event_cb(channel_id, transaction_id, event,
        event_code, diagnostic_msg, user)`` is stored but the loopback
        transport generates no events. Returns ``ReturnCode.NO_ACTION``
        if a callback is already registered.
        """
        try:
            cb_kind = TpmCallbackKind(kind)
        except ValueError:
            raise InvalidParamError(
                f"invalid TPM callback kind {kind}"
            ) from None
        with self._cond:
            self._check_initialized()
            ch = self._find_channel(channel_id)
            if ch is None:
                raise InvalidParamError(
                    f"invalid TPM channel id {channel_id}"
                )
            if ch.cb_registered:
                return ReturnCode.NO_ACTION
            ch.data_cb = data_cb
            ch.event_cb = event_cb
            ch.cb_user = user
            ch.cb_kind = cb_kind
            ch.cb_registered = True
            return ReturnCode.NO_ERROR

    def unregister_callback(
        self,
        channel_id: int,
        kind: TpmCallbackKind | int = TpmCallbackKind.BOTH,
    ) -> ReturnCode:
        """Unregister callbacks (mirrors the C function; kind is ignored).

        Returns ``ReturnCode.NO_ACTION`` when nothing is registered.
        """
        with self._cond:
            self._check_initialized()
            ch = self._find_channel(channel_id)
            if ch is None:
                raise InvalidParamError(
                    f"invalid TPM channel id {channel_id}"
                )
            if not ch.cb_registered:
                return ReturnCode.NO_ACTION
            ch.cb_registered = False
            ch.data_cb = None
            ch.event_cb = None
            return ReturnCode.NO_ERROR


__all__ = [
    "CHANNEL_ID_INVALID",
    "TPM_MAX_CHANNELS",
    "TPM_MAX_MSG",
    "Tpm",
    "TpmCallbackKind",
    "TpmEventType",
    "TpmLevelOfTestType",
    "TpmStateType",
]
