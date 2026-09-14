"""FACE TSS (Transport Services Segment) over nng + FlatBuffers.

Implements the FACE TS interface shape - Initialize / Create_Connection /
Destroy_Connection / Send_Message / Receive_Message / Register_Callback /
Unregister_Callback - with data movement provided by nng sockets and framing
provided by FlatBuffers envelopes (see :mod:`face_tss.envelope`).

API shape follows FACE 3.1:

* ``send_message`` takes a timeout and an in/out transaction id: pass
  ``TRANSACTION_ID_UNSPECIFIED`` (0) and the TSS assigns one; the id used
  is returned.
* ``receive_message`` returns ``(ReceivedMessage, transaction_id, qos)``
  where ``qos`` is a :class:`QosEvent` (currently empty).
* Callbacks receive ``(connection_id, transaction_id, message_guid,
  payload, header, qos, context)`` and return a :class:`ReturnCode`.
* Each message carries a FACE ``HEADER_TYPE`` projection
  (``instance_uid`` / ``source_uid`` / ``timestamp``) plus a message GUID
  identifying the application type.

Connection model (one FACE connection = one nng socket, see transport.py):

* SOURCE        - may only Send_Message.
* DESTINATION   - may only Receive_Message.
* BI_DIRECTIONAL - may do both.

IDs: Create_Connection returns increasing ints starting at 1 (0 is reserved
as CONNECTION_ID_INVALID).

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

from .config import (
    CONFIGURATION_INTERFACE_NAME,
    CONFIGURATION_RESOURCE_MAX,
    ConfigurationProvider,
    ConnectionConfig,
    JsonConfigurationProvider,
    TssConfig,
    normalize_name,
)
from .envelope import Envelope
from .errors import (
    ConnectionClosedError,
    DataBufferTooSmallError,
    FaceTssError,
    InvalidConfigError,
    InvalidModeError,
    InvalidParamError,
    NotInitializedError,
    ResourceLimitError,
    TimedOutError,
)
from .transport import CallbackHandle, Transport, open_transport
from .types import (
    CONNECTION_ID_INVALID,
    MAX_CONNECTIONS,
    MESSAGE_GUID_INVALID,
    TIMEOUT_INFINITE,
    TRANSACTION_ID_UNSPECIFIED,
    Direction,
    Header,
    MessageValidity,
    QosElement,
    QosEvent,
    ReturnCode,
    now_ns,
)

# Re-export the timeout sentinel for `from face_tss.tss import ...` users.
TIMEOUT_INFINITE = TIMEOUT_INFINITE

ConnectionId = int
TransactionId = int
MessageGuid = int

#: Built-in JSON adapter used when no Configuration provider is injected.
_JSON_PROVIDER = JsonConfigurationProvider()

#: Callback signature: (connection_id, transaction_id, message_guid,
#: payload, header, qos, context) -> ReturnCode.
MessageCallback = Callable[
    [ConnectionId, TransactionId, MessageGuid, bytes, Header, QosEvent, object],
    ReturnCode,
]


@dataclass
class ReceivedMessage:
    """A typed payload plus its FACE header, as returned to the application."""

    payload: bytes
    header: Header
    message_guid: MessageGuid = MESSAGE_GUID_INVALID
    validity: MessageValidity = MessageValidity.VALID


@dataclass
class _Connection:
    config: ConnectionConfig
    transport: Transport
    closed: bool = False
    send_seq: int = 0
    callback: MessageCallback | None = None
    callback_context: object = None
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
        self._instance_uid = random.getrandbits(63)
        self._txn_counter = itertools.count(1)
        self._lock = threading.RLock()
        self._initialized = False
        self._config = TssConfig(instance_name=instance_name)
        self._configuration: ConfigurationProvider | None = None
        self._configuration_set = False
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
        """Initialize from a parsed config object.

        Convenience adapter (not the FACE IDL shape); the
        FACE::TSS::Base::Initialize(CONFIGURATION_RESOURCE) shape is
        :meth:`initialize_from_resource`. Idempotent.
        """
        with self._lock:
            if self._initialized:
                return ReturnCode.NO_ACTION
            if not isinstance(config, TssConfig):
                raise InvalidParamError("config must be a TssConfig")
            self._config = config
            self._initialized = True
            return ReturnCode.NO_ERROR

    def set_reference(
        self,
        interface_name: str,
        configuration: ConfigurationProvider,
        id: int,
    ) -> ReturnCode:
        """FACE::TSS::Base::Set_Reference (Injectable).

        Installs the Configuration interface reference; must be called
        before initialize. Returns NO_ERROR when stored, NO_ACTION when the
        same provider is already set, NOT_AVAILABLE when a different one is
        (one Configuration per TSS), INVALID_MODE after initialize.
        Raises InvalidParamError on bad arguments or an interface name other
        than "Configuration".
        """
        _ = id  # delineates interface instances; one slot per TSS here
        if not isinstance(interface_name, str) or (
            interface_name != CONFIGURATION_INTERFACE_NAME
        ):
            raise InvalidParamError(
                'interface_name must be "Configuration"'
            )
        if not isinstance(configuration, ConfigurationProvider):
            raise InvalidParamError(
                "configuration must be a ConfigurationProvider"
            )
        with self._lock:
            if self._initialized:
                return ReturnCode.INVALID_MODE  # steady state
            if self._configuration_set:
                if self._configuration is configuration:
                    return ReturnCode.NO_ACTION  # duplicate
                return ReturnCode.NOT_AVAILABLE
            self._configuration = configuration
            self._configuration_set = True
            return ReturnCode.NO_ERROR

    def initialize_from_resource(self, resource: str) -> ReturnCode:
        """FACE::TSS::Base::Initialize(CONFIGURATION_RESOURCE).

        Resolves ``resource`` through the injected Configuration provider
        when one was set via :meth:`set_reference`, otherwise through the
        built-in JSON adapter (``json:{...}`` inline, or a file path).
        Idempotent: a second call returns NO_ACTION.
        """
        if not isinstance(resource, str) or (
            len(resource) >= CONFIGURATION_RESOURCE_MAX
        ):
            raise InvalidParamError("resource must be a bounded string")
        with self._lock:
            provider = (
                self._configuration
                if self._configuration_set
                else _JSON_PROVIDER
            )
        try:
            config = provider.load(resource)
        except FaceTssError:
            raise
        except Exception as exc:
            raise InvalidConfigError(
                f"configuration provider failed: {exc}"
            ) from exc
        return self.initialize(config)

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
            if len(self._connections) >= MAX_CONNECTIONS:
                raise ResourceLimitError("too many open connections")
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
        conn.callback_context = None
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
        timeout_ns: int = TIMEOUT_INFINITE,
        transaction_id: int = TRANSACTION_ID_UNSPECIFIED,
        message_guid: MessageGuid = MESSAGE_GUID_INVALID,
    ) -> int:
        """FACE::TS::Send_Message. Returns the transaction id used.

        Pass ``TRANSACTION_ID_UNSPECIFIED`` (0) to have the TSS assign one.
        Raises InvalidModeError on receive-only connections,
        DataBufferTooSmallError when the payload exceeds max_message_size,
        TimedOutError when the send timeout expires.
        """
        data = bytes(payload)
        with self._lock:
            conn = self._require_open(connection_id)
            if not conn.config.can_send:
                raise InvalidModeError(
                    f"connection {conn.config.name} is DESTINATION-only"
                )
            if len(data) > conn.config.max_message_size:
                raise DataBufferTooSmallError(
                    f"payload {len(data)} > max {conn.config.max_message_size}"
                )
            txn = (
                int(transaction_id)
                if transaction_id != TRANSACTION_ID_UNSPECIFIED
                else next(self._txn_counter)
            )
            conn.send_seq += 1
            instance_uid = (self._instance_uid + conn.send_seq) & ((1 << 63) - 1)
            env = Envelope(
                connection_name=conn.config.name,
                transaction_id=txn,
                source_id=self._source_id,
                sequence_number=conn.send_seq,
                timestamp_ns=now_ns(),
                payload=data,
                message_guid=int(message_guid),
                instance_uid=instance_uid,
            )
            try:
                conn.transport.send(env, timeout_ns)
            except FaceTssError:
                self._stats.send_errors += 1
                raise
            except Exception as exc:  # pragma: no cover - defensive
                self._stats.send_errors += 1
                raise FaceTssError(ReturnCode.NO_ACTION, f"send failed: {exc}") from exc
            self._stats.sent += 1
            return txn

    # -- messaging: Receive_Message ---------------------------------------
    @staticmethod
    def _qos_for(env: Envelope) -> QosEvent:
        """Honest, transport-observable QoS data for one received message.

        Currently one element: message_age_ns (receive time minus the send
        timestamp). No QoS policies are enforced and MESSAGE_STALE is never
        produced; see issue #2.
        """
        age = now_ns() - env.timestamp_ns
        return QosEvent([QosElement(name="message_age_ns", value=max(age, 0))])

    def receive_message(
        self,
        connection_id: ConnectionId,
        timeout_ns: int = TIMEOUT_INFINITE,
        min_message_size: int = 0,
    ) -> tuple[ReceivedMessage, TransactionId, QosEvent]:
        """FACE::TS::Receive_Message - block up to timeout for one message.

        Returns ``(message, transaction_id, qos_event)``. Raises
        TimedOutError (FACE TIMED_OUT) on timeout, InvalidModeError on
        send-only connections, DataBufferTooSmallError if the payload is
        smaller than ``min_message_size`` (FACE buffer-too-small semantics).
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
                raise DataBufferTooSmallError(
                    f"payload {len(env.payload)} < required {min_message_size}"
                )
            self._stats.received += 1
            msg = ReceivedMessage(
                payload=env.payload,
                header=Header(
                    instance_uid=env.instance_uid,
                    source_uid=env.source_id,
                    timestamp=env.timestamp_ns,
                ),
                message_guid=env.message_guid,
            )
            return msg, int(env.transaction_id), self._qos_for(env)

    def receive_into(
        self,
        connection_id: ConnectionId,
        buffer: bytearray,
        timeout_ns: int = TIMEOUT_INFINITE,
        min_message_size: int = 0,
    ) -> tuple[int, TransactionId, Header, MessageGuid, QosEvent]:
        """FACE::TS::Receive_Message with a caller-owned buffer.

        Copies the payload into ``buffer`` instead of allocating, mirroring
        the FACE buffer-too-small semantics. Returns
        ``(payload_len, transaction_id, header, message_guid, qos_event)``.

        Raises TimedOutError (FACE TIMED_OUT) on timeout, InvalidModeError
        on send-only connections, DataBufferTooSmallError if the payload is
        smaller than ``min_message_size`` or does not fit ``buffer``. In
        the too-small case the message is discarded and the error message
        reports the required size.
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
                raise DataBufferTooSmallError(
                    f"payload {len(env.payload)} < required {min_message_size}"
                )
            if len(env.payload) > len(buffer):
                raise DataBufferTooSmallError(
                    f"payload {len(env.payload)} exceeds buffer "
                    f"{len(buffer)}; required size {len(env.payload)}"
                )
            buffer[: len(env.payload)] = env.payload
            self._stats.received += 1
            header = Header(
                instance_uid=env.instance_uid,
                source_uid=env.source_id,
                timestamp=env.timestamp_ns,
            )
            return (
                len(env.payload),
                int(env.transaction_id),
                header,
                env.message_guid,
                self._qos_for(env),
            )

    def try_receive(
        self, connection_id: ConnectionId
    ) -> tuple[ReceivedMessage, TransactionId, QosEvent] | None:
        """Non-blocking receive; returns None instead of raising TimedOutError.

        Explicit extension beyond the FACE interface, kept for convenience.
        """
        try:
            return self.receive_message(connection_id, timeout_ns=0)
        except TimedOutError:
            return None

    # -- callbacks: Register/Unregister -----------------------------------
    def register_callback(
        self,
        connection_id: ConnectionId,
        callback: MessageCallback,
        context: object = None,
    ) -> ReturnCode:
        """FACE::TS::Register_Callback - deliver messages on a bg thread.

        The callback receives ``(connection_id, transaction_id,
        message_guid, payload, header, qos, context)`` and returns a
        ReturnCode; exceptions are swallowed and count as NO_ACTION.
        """
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
            conn.callback_context = context

            def _dispatch(env: Envelope) -> None:
                header = Header(
                    instance_uid=env.instance_uid,
                    source_uid=env.source_id,
                    timestamp=env.timestamp_ns,
                )
                with self._lock:
                    live = self._connections.get(connection_id)
                    cb = live.callback if live is not None else None
                    ctx = live.callback_context if live is not None else None
                if cb is not None:
                    try:
                        cb(
                            connection_id,
                            int(env.transaction_id),
                            int(env.message_guid),
                            env.payload,
                            header,
                            self._qos_for(env),
                            ctx,
                        )
                    except Exception:
                        pass
                with self._lock:
                    self._stats.received += 1

            conn.callback_handle = conn.transport.start_callback(_dispatch)
            return ReturnCode.NO_ERROR

    def unregister_callback(self, connection_id: ConnectionId) -> ReturnCode:
        """Unregister a connection's callback. NO_ACTION if none registered.

        FACE 3.1 placed Unregister_Callback on the Base interface; FACE 3.2
        moved it to TypedTS. The Python mirror exposes one method covering
        both (typed and untyped callbacks share the connection slot).
        """
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
            conn.callback_context = None
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
