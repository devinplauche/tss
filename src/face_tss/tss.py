"""FACE TSS (Transport Services Segment) over nng + FlatBuffers.

Implements the FACE TS interface shape - Initialize / Create_Connection /
Destroy_Connection / Send_Message / Receive_Message / Register_Callback /
Unregister_Callback - with data movement provided by nng sockets and framing
provided by FlatBuffers envelopes (see :mod:`face_tss.envelope`).

Connection model (one FACE connection = one nng socket, see transport.py):

* SOURCE        - may only Send_Message.
* DESTINATION   - may only Receive_Message.
* BI_DIRECTIONAL - may do both.

IDs: Create_Connection returns increasing ints starting at 1 (0 is reserved
as CONNECTION_ID_INVALID). Transaction IDs pass through untouched for
request/reply correlation; the TSS stamps source_id (random per-instance
GUID), per-connection sequence numbers, and a send timestamp on every
outgoing envelope.

Threading: one RLock guards all connection-table and sequence mutations.
Transports are used under that lock for table/sequence consistency; the nng
sockets themselves are thread-safe for concurrent send/recv.
"""

from __future__ import annotations

import itertools
import random
import threading
from dataclasses import dataclass, field
from typing import Callable

from .config import ConnectionConfig, TssConfig, normalize_name
from .envelope import Envelope
from .errors import (
    BufferTooSmallError,
    ConnectionClosedError,
    FaceTssError,
    InvalidModeError,
    InvalidParamError,
    NotInitializedError,
    TimedOutError,
)
from .transport import CallbackHandle, Transport, open_transport
from .types import (
    CONNECTION_ID_INVALID,
    TIMEOUT_INFINITE,
    Direction,
    Header,
    MessageValidity,
    ReturnCode,
    now_ns,
)

# Re-export the timeout sentinel for `from face_tss.tss import ...` users.
TIMEOUT_INFINITE = TIMEOUT_INFINITE

ConnectionId = int
TransactionId = int


@dataclass
class ReceivedMessage:
    """A typed payload plus its FACE header, as returned to the application."""

    payload: bytes
    header: Header
    validity: MessageValidity = MessageValidity.VALID


@dataclass
class _Connection:
    config: ConnectionConfig
    transport: Transport
    closed: bool = False
    send_seq: int = 0
    callback: Callable[[ReceivedMessage], None] | None = None
    callback_handle: CallbackHandle | None = None


@dataclass
class TssStats:
    sent: int = 0
    received: int = 0
    send_errors: int = 0
    receive_timeouts: int = 0
    stale_dropped: int = 0


class FaceTss:
    """A FACE Transport Services Segment instance."""

    def __init__(self, instance_name: str = "face-tss") -> None:
        self._instance_name = instance_name
        self._source_id = random.getrandbits(63)
        self._lock = threading.RLock()
        self._initialized = False
        self._config = TssConfig(instance_name=instance_name)
        self._ids = itertools.count(1)
        self._connections: dict[ConnectionId, _Connection] = {}
        self._stats = TssStats()

    # -- properties -----------------------------------------------------
    @property
    def instance_name(self) -> str:
        return self._instance_name

    @property
    def source_id(self) -> int:
        return self._source_id

    @property
    def initialized(self) -> bool:
        with self._lock:
            return self._initialized

    @property
    def stats(self) -> TssStats:
        with self._lock:
            return TssStats(
                sent=self._stats.sent,
                received=self._stats.received,
                send_errors=self._stats.send_errors,
                receive_timeouts=self._stats.receive_timeouts,
                stale_dropped=self._stats.stale_dropped,
            )

    # -- lifecycle: Initialize ------------------------------------------
    def initialize(self, config: TssConfig) -> ReturnCode:
        """FACE::TS::Initialize - load configuration. Idempotent."""
        with self._lock:
            if self._initialized:
                return ReturnCode.NO_ACTION
            if not isinstance(config, TssConfig):
                raise InvalidParamError("config must be a TssConfig")
            self._config = config
            self._initialized = True
            return ReturnCode.NO_ERROR

    def finalize(self) -> None:
        """Close every connection and return to the uninitialized state."""
        with self._lock:
            for conn_id in list(self._connections.keys()):
                self._destroy_locked(conn_id)
            self._initialized = False

    # -- connections -----------------------------------------------------
    def create_connection(self, name: str) -> tuple[ConnectionId, int]:
        """FACE::TS::Create_Connection -> (connection_id, max_message_size)."""
        with self._lock:
            self._require_initialized()
            cfg = self._config.lookup(name)  # raises InvalidParamError
            transport = open_transport(cfg)  # raises TransportError
            conn_id = next(self._ids)
            self._connections[conn_id] = _Connection(config=cfg, transport=transport)
            return conn_id, cfg.max_message_size

    def destroy_connection(self, connection_id: ConnectionId) -> ReturnCode:
        """FACE::TS::Destroy_Connection. Idempotent per FACE (NO_ACTION)."""
        with self._lock:
            self._require_initialized()
            conn = self._connections.get(connection_id)
            if conn is None:
                # Unknown id: either never existed (INVALID_PARAM) or was
                # already destroyed. We only track live ones, so treat any
                # nonzero unknown id as already-destroyed -> NO_ACTION, and
                # the reserved 0 as INVALID_PARAM.
                if connection_id == CONNECTION_ID_INVALID:
                    raise InvalidParamError("invalid connection id 0")
                return ReturnCode.NO_ACTION
            self._destroy_locked(connection_id)
            return ReturnCode.NO_ERROR

    def _destroy_locked(self, connection_id: ConnectionId) -> None:
        conn = self._connections.pop(connection_id, None)
        if conn is None:
            return
        conn.closed = True
        if conn.callback_handle is not None:
            try:
                conn.callback_handle.cancel()
            except Exception:
                pass
            conn.callback_handle = None
        conn.callback = None
        try:
            conn.transport.close()
        except Exception:
            pass

    def connection_names(self) -> dict[ConnectionId, str]:
        with self._lock:
            return {cid: c.config.name for cid, c in self._connections.items()}

    # -- messaging: Send_Message -----------------------------------------
    def send_message(
        self,
        connection_id: ConnectionId,
        payload: bytes | bytearray | memoryview,
        transaction_id: int = 0,
    ) -> int:
        """FACE::TS::Send_Message. Returns the transaction id used.

        Raises InvalidModeError on receive-only connections,
        BufferTooSmallError when the payload exceeds max_message_size.
        """
        data = bytes(payload)
        with self._lock:
            conn = self._require_open(connection_id)
            if not conn.config.can_send:
                raise InvalidModeError(
                    f"connection {conn.config.name} is DESTINATION-only"
                )
            if len(data) > conn.config.max_message_size:
                raise BufferTooSmallError(
                    f"payload {len(data)} > max {conn.config.max_message_size}"
                )
            conn.send_seq += 1
            env = Envelope(
                connection_name=conn.config.name,
                transaction_id=int(transaction_id),
                source_id=self._source_id,
                sequence_number=conn.send_seq,
                timestamp_ns=now_ns(),
                payload=data,
            )
            try:
                conn.transport.send(env)
            except FaceTssError:
                self._stats.send_errors += 1
                raise
            except Exception as exc:  # pragma: no cover - defensive
                self._stats.send_errors += 1
                raise FaceTssError(ReturnCode.NO_ACTION, f"send failed: {exc}") from exc
            self._stats.sent += 1
            return int(transaction_id)

    # -- messaging: Receive_Message ---------------------------------------
    def receive_message(
        self,
        connection_id: ConnectionId,
        timeout_ns: int = TIMEOUT_INFINITE,
        min_message_size: int = 0,
        transaction_id: int = 0,
    ) -> ReceivedMessage:
        """FACE::TS::Receive_Message - block up to timeout for one message.

        Raises TimedOutError (FACE TIMED_OUT) on timeout, InvalidModeError on
        send-only connections, BufferTooSmallError if the payload is smaller
        than ``min_message_size`` (FACE buffer-too-small semantics).
        """
        with self._lock:
            conn = self._require_open(connection_id)
            if not conn.config.can_receive:
                raise InvalidModeError(
                    f"connection {conn.config.name} is SOURCE-only"
                )
            try:
                env = conn.transport.receive(timeout_ns)
            except TimedOutError:
                self._stats.receive_timeouts += 1
                raise
            if len(env.payload) < min_message_size:
                raise BufferTooSmallError(
                    f"payload {len(env.payload)} < required {min_message_size}"
                )
            self._stats.received += 1
            void = transaction_id  # kept for signature parity with FACE API
            del void
            return ReceivedMessage(
                payload=env.payload,
                header=Header(
                    connection_name=env.connection_name,
                    transaction_id=env.transaction_id,
                    source_id=env.source_id,
                    sequence_number=env.sequence_number,
                    timestamp_ns=env.timestamp_ns,
                ),
            )

    def try_receive(self, connection_id: ConnectionId) -> ReceivedMessage | None:
        """Non-blocking receive; returns None instead of raising TimedOutError."""
        try:
            return self.receive_message(connection_id, timeout_ns=0)
        except TimedOutError:
            return None

    # -- callbacks: Register/Unregister -----------------------------------
    def register_callback(
        self,
        connection_id: ConnectionId,
        callback: Callable[[ReceivedMessage], None],
    ) -> ReturnCode:
        """FACE::TS::Register_Callback - deliver messages on a bg thread."""
        if not callable(callback):
            raise InvalidParamError("callback must be callable")
        with self._lock:
            conn = self._require_open(connection_id)
            if not conn.config.can_receive:
                raise InvalidModeError(
                    f"connection {conn.config.name} is SOURCE-only"
                )
            if conn.callback is not None:
                return ReturnCode.NO_ACTION
            conn.callback = callback

            def _dispatch(env: Envelope) -> None:
                msg = ReceivedMessage(
                    payload=env.payload,
                    header=Header(
                        connection_name=env.connection_name,
                        transaction_id=env.transaction_id,
                        source_id=env.source_id,
                        sequence_number=env.sequence_number,
                        timestamp_ns=env.timestamp_ns,
                    ),
                )
                with self._lock:
                    live = self._connections.get(connection_id)
                    cb = live.callback if live is not None else None
                if cb is not None:
                    cb(msg)
                with self._lock:
                    self._stats.received += 1

            conn.callback_handle = conn.transport.start_callback(_dispatch)
            return ReturnCode.NO_ERROR

    def unregister_callback(self, connection_id: ConnectionId) -> ReturnCode:
        """FACE::TS::Unregister_Callback. NO_ACTION if none registered."""
        with self._lock:
            conn = self._require_open(connection_id)
            if conn.callback is None:
                return ReturnCode.NO_ACTION
            if conn.callback_handle is not None:
                try:
                    conn.callback_handle.cancel()
                except Exception:
                    pass
                conn.callback_handle = None
            conn.callback = None
            return ReturnCode.NO_ERROR

    # -- internals ---------------------------------------------------------
    def _require_initialized(self) -> None:
        if not self._initialized:
            raise NotInitializedError

    def _require_open(self, connection_id: ConnectionId) -> _Connection:
        self._require_initialized()
        conn = self._connections.get(connection_id)
        if conn is None or conn.closed:
            raise ConnectionClosedError(f"connection id {connection_id} is closed")
        return conn
