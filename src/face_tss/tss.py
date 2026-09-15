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

Threading: one RLock guards the connection table, sequence numbers,
stats, and QoS policies. The lock is never held across blocking
transport I/O: send/receive snapshot what they need, release the lock,
perform the I/O, then re-take the lock for stats. Callback threads are
never joined while holding the lock (``Transport.shutdown`` stops the
loop and closes the socket first, outside the TSS lock), so a callback
that re-enters the TSS - including one that unregisters itself - cannot
deadlock. nng sockets are thread-safe for concurrent send/recv.
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
    MessageStaleError,
    NotInitializedError,
    ResourceLimitError,
    TimedOutError,
)
from .transport import Transport, open_transport
from .types import (
    CONNECTION_ID_INVALID,
    MAX_CONNECTIONS,
    MESSAGE_GUID_INVALID,
    QOS_BEST_EFFORT,
    QOS_RELIABLE,
    TIMEOUT_INFINITE,
    TRANSACTION_ID_UNSPECIFIED,
    Direction,
    Header,
    MessageValidity,
    QosElement,
    QosEvent,
    QosPolicyKind,
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
    # Receive-side sequence baseline for QoS reliability gap monitoring.
    # Only meaningful while a RELIABILITY policy is set; guarded by the
    # TSS lock. A source change (or the first message) re-baselines
    # without counting a gap.
    rx_source: int = 0
    rx_seq: int = 0
    rx_seen: bool = False


@dataclass
class TssStats:
    sent: int = 0
    received: int = 0
    send_errors: int = 0
    receive_timeouts: int = 0
    stale_dropped: int = 0
    priority_dropped: int = 0  # dropped by the QoS priority threshold
    reliability_gaps: int = 0  # skipped sequence numbers observed while
    # reliability monitoring was active


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
        self._qos_policies: dict[ConnectionId, dict[QosPolicyKind, int]] = {}
        self._stats = TssStats()
        # Set while finalize() is tearing down. Entry points fail fast
        # with NotInitializedError instead of racing the teardown.
        self._finalizing = False

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
                priority_dropped=self._stats.priority_dropped,
                reliability_gaps=self._stats.reliability_gaps,
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
        """Close every connection and return to the uninitialized state.

        Callback threads are stopped and transports shut down with the
        TSS lock released, so a callback blocked re-entering the TSS
        cannot deadlock the teardown.
        """
        with self._lock:
            if self._finalizing:
                return
            self._finalizing = True
            conns = list(self._connections.values())
            self._connections.clear()
            self._qos_policies.clear()
            for conn in conns:
                conn.closed = True
                conn.callback = None
                conn.callback_context = None
        # Outside the lock: stop callback loops (joining a dispatch that
        # may need the TSS lock), abort blocked I/O via socket close.
        for conn in conns:
            try:
                conn.transport.shutdown()
            except Exception:
                pass
        with self._lock:
            self._initialized = False
            self._finalizing = False

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
            self._require_usable()
            conn = self._connections.get(connection_id)
            if conn is None:
                # Unknown id: either never existed (INVALID_PARAM) or was
                # already destroyed. We only track live ones, so treat any
                # nonzero unknown id as already-destroyed -> NO_ACTION, and
                # the reserved 0 as INVALID_PARAM.
                if connection_id == CONNECTION_ID_INVALID:
                    raise InvalidParamError("invalid connection id 0")
                return ReturnCode.NO_ACTION
            conn = self._detach_locked(connection_id)
        # Outside the lock: stop the callback thread (it may be inside a
        # dispatch needing the TSS lock) and abort blocked I/O.
        try:
            conn.transport.shutdown()
        except Exception:
            pass
        return ReturnCode.NO_ERROR

    def _detach_locked(self, connection_id: ConnectionId) -> _Connection | None:
        """Pop a connection and detach its callback; caller holds the lock.

        New dispatches observe callback=None and drop. The caller must
        shut the transport down with the lock released.
        """
        conn = self._connections.pop(connection_id, None)
        if conn is None:
            return None
        conn.closed = True
        conn.callback = None
        conn.callback_context = None
        self._qos_policies.pop(connection_id, None)
        return conn

    def connection_names(self) -> dict[ConnectionId, str]:
        with self._lock:
            return {cid: c.config.name for cid, c in self._connections.items()}

    # -- QoS policies (extensions; not in the FACE IDL) --------------------
    def set_qos_policy(
        self,
        connection_id: ConnectionId,
        kind: QosPolicyKind,
        value_ns: int,
    ) -> ReturnCode:
        """Set a per-connection QoS policy.

        - ``QosPolicyKind.STALENESS`` / ``MAX_AGE`` (nanoseconds):
          messages older than the threshold are discarded on receive and
          the receive raises MessageStaleError (callbacks are not invoked
          for stale messages).
        - ``QosPolicyKind.PRIORITY``: the connection's send priority
          (stamped on the wire) and, on a receiving connection, the
          minimum-priority delivery threshold. Below-threshold messages
          are dropped -- a blocking receive keeps waiting for a
          qualifying message until the timeout, callbacks are not
          invoked. 0 (or unset) accepts everything.
        - ``QosPolicyKind.RELIABILITY``: ``QOS_BEST_EFFORT`` (0) or
          ``QOS_RELIABLE`` (1). ``QOS_RELIABLE`` raises a
          NOT_AVAILABLE FaceTssError on the best-effort transports
          (pub/sub, bus), which cannot provide reliable delivery.
          Setting either level enables receive-side sequence-gap
          monitoring.
        """
        if value_ns < 0:
            raise InvalidParamError("QoS policy value must be >= 0")
        if kind == QosPolicyKind.MAX_AGE:
            kind = QosPolicyKind.STALENESS  # documented alias
        if kind == QosPolicyKind.RELIABILITY and value_ns not in (
            QOS_BEST_EFFORT,
            QOS_RELIABLE,
        ):
            raise InvalidParamError(
                f"unknown reliability level {value_ns} "
                f"(expected {QOS_BEST_EFFORT} or {QOS_RELIABLE})"
            )
        with self._lock:
            self._require_initialized()
            conn = self._require_open(connection_id)
            if (
                kind == QosPolicyKind.RELIABILITY
                and value_ns == QOS_RELIABLE
                and conn.config.transport in ("pubsub", "bus")
            ):
                raise FaceTssError(
                    ReturnCode.NOT_AVAILABLE,
                    "QOS_RELIABLE is not available on the best-effort "
                    f"{conn.config.transport} transport",
                )
            self._qos_policies.setdefault(connection_id, {})[kind] = value_ns
            return ReturnCode.NO_ERROR

    def get_qos_policy(
        self, connection_id: ConnectionId, kind: QosPolicyKind
    ) -> int | None:
        """Return a connection's QoS policy value, or None if unset."""
        if kind == QosPolicyKind.MAX_AGE:
            kind = QosPolicyKind.STALENESS  # documented alias
        with self._lock:
            self._require_initialized()
            self._require_open(connection_id)
            return self._qos_policies.get(connection_id, {}).get(kind)

    def _is_stale(self, connection_id: ConnectionId, timestamp_ns: int) -> bool:
        """True if the connection's staleness policy rejects this message.

        Call with the lock held. Records the drop in stats.
        """
        threshold = self._qos_policies.get(connection_id, {}).get(
            QosPolicyKind.STALENESS
        )
        if threshold is None:
            return False
        age = now_ns() - timestamp_ns
        if age > threshold:
            self._stats.stale_dropped += 1
            return True
        return False

    def _is_below_priority(
        self, connection_id: ConnectionId, priority: int
    ) -> bool:
        """True if the connection's priority threshold rejects this message.

        Call with the lock held. Records the drop in stats.
        """
        threshold = self._qos_policies.get(connection_id, {}).get(
            QosPolicyKind.PRIORITY, 0
        )
        if threshold > 0 and priority < threshold:
            self._stats.priority_dropped += 1
            return True
        return False

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
            transport = conn.transport
            # Snapshot everything the blocking send needs; the _Connection
            # stays alive via this reference even if another thread
            # destroys the connection mid-send (the transport then fails
            # the send instead of touching freed state).
            env = Envelope(
                connection_name=conn.config.name,
                transaction_id=txn,
                source_id=self._source_id,
                sequence_number=conn.send_seq,
                timestamp_ns=now_ns(),
                payload=data,
                message_guid=int(message_guid),
                instance_uid=instance_uid,
                # QoS priority: stamp the connection's policy value
                # (0 when unset).
                priority=int(
                    self._qos_policies.get(connection_id, {}).get(
                        QosPolicyKind.PRIORITY, 0
                    )
                ),
            )
        # Blocking I/O with the TSS lock released: one slow send must not
        # stall unrelated connections or block finalize().
        try:
            transport.send(env, timeout_ns)
        except FaceTssError:
            with self._lock:
                self._stats.send_errors += 1
            raise
        except Exception as exc:  # pragma: no cover - defensive
            with self._lock:
                self._stats.send_errors += 1
            raise FaceTssError(ReturnCode.NO_ACTION, f"send failed: {exc}") from exc
        with self._lock:
            self._stats.sent += 1
        return txn

    # -- messaging: Receive_Message ---------------------------------------
    @staticmethod
    def _qos_for(env: Envelope, seq_gap: int = -1) -> QosEvent:
        """Honest, transport-observable QoS data for one received message.

        Elements: ``message_age_ns`` (receive time minus the send
        timestamp), ``priority`` (the sender's priority from the
        envelope), and -- when reliability monitoring is active for the
        connection (``seq_gap >= 0``) -- ``sequence_gap`` (skipped
        sequence numbers immediately before this message). Staleness and
        priority enforcement happen in the receive paths before this
        runs; see issue #2.
        """
        age = now_ns() - env.timestamp_ns
        elements = [
            QosElement(name="message_age_ns", value=max(age, 0)),
            QosElement(name="priority", value=int(env.priority)),
        ]
        if seq_gap >= 0:
            elements.append(QosElement(name="sequence_gap", value=seq_gap))
        return QosEvent(elements)

    def _rx_gap_locked(self, connection_id: ConnectionId, env: Envelope) -> int:
        """Sequence-gap bookkeeping for QoS reliability monitoring.

        Call with the lock held, once per wire-observed envelope (before
        policy filtering, so stale/priority drops do not surface as
        phantom gaps). Returns -1 when no RELIABILITY policy is set
        (monitoring inactive); otherwise updates the per-connection
        baseline and returns the
        number of skipped sequence numbers immediately before this
        message (0 when none). A new source (or the first message)
        re-baselines without counting a gap: on pub/sub a late
        subscriber legitimately misses the messages sent before it
        arrived.
        """
        policies = self._qos_policies.get(connection_id, {})
        if QosPolicyKind.RELIABILITY not in policies:
            return -1
        conn = self._connections.get(connection_id)
        if conn is None:  # destroyed concurrently; nothing to track
            return -1
        gap = 0
        if not conn.rx_seen or env.source_id != conn.rx_source:
            pass  # (re)baseline: no gap counted
        elif env.sequence_number > conn.rx_seq + 1:
            gap = env.sequence_number - conn.rx_seq - 1
            self._stats.reliability_gaps += gap
        if (
            not conn.rx_seen
            or env.source_id != conn.rx_source
            or env.sequence_number > conn.rx_seq
        ):
            conn.rx_source = env.source_id
            conn.rx_seq = env.sequence_number
            conn.rx_seen = True
        return gap

    def _receive_envelope(
        self, connection_id: ConnectionId, timeout_ns: int
    ) -> tuple[Envelope, int]:
        """Blocking receive core with QoS priority filtering.

        Below-threshold messages are dropped (counted in stats) and the
        wait continues until the timeout expires; raises TimedOutError
        when nothing qualifying arrives. A poll (timeout 0) tries once.

        Returns ``(envelope, seq_gap)`` where ``seq_gap`` is the
        reliability sequence gap observed immediately before the
        delivered envelope (-1 when monitoring is inactive). Gap
        bookkeeping observes every envelope that arrives on the wire,
        before policy filtering, so stale/priority drops do not surface
        as phantom gaps later.
        """
        with self._lock:
            conn = self._require_open(connection_id)
            if not conn.config.can_receive:
                raise InvalidModeError(
                    f"connection {conn.config.name} is SOURCE-only"
                )
            transport = conn.transport
            prio_threshold = self._qos_policies.get(connection_id, {}).get(
                QosPolicyKind.PRIORITY, 0
            )
        # Deadline-based so priority drops do not extend the caller's
        # timeout. None = infinite; timeout 0 polls once.
        deadline = now_ns() + timeout_ns if timeout_ns > 0 else None
        seq_gap = -1
        while True:
            remaining = timeout_ns
            if deadline is not None:
                remaining = deadline - now_ns()
                if remaining <= 0:
                    with self._lock:
                        self._stats.receive_timeouts += 1
                    raise TimedOutError()
            try:
                env = transport.receive(remaining)
            except TimedOutError:
                with self._lock:
                    self._stats.receive_timeouts += 1
                raise
            with self._lock:
                seq_gap = self._rx_gap_locked(connection_id, env)
            if prio_threshold > 0 and env.priority < prio_threshold:
                with self._lock:
                    self._stats.priority_dropped += 1
                if timeout_ns == 0:
                    with self._lock:
                        self._stats.receive_timeouts += 1
                    raise TimedOutError()
                continue
            return env, seq_gap

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
        env, seq_gap = self._receive_envelope(connection_id, timeout_ns)
        with self._lock:
            if len(env.payload) < min_message_size:
                raise DataBufferTooSmallError(
                    f"payload {len(env.payload)} < required {min_message_size}"
                )
            if self._is_stale(connection_id, env.timestamp_ns):
                raise MessageStaleError(
                    "message exceeded the connection's staleness policy"
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
            return msg, int(env.transaction_id), self._qos_for(env, seq_gap)

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
        env, seq_gap = self._receive_envelope(connection_id, timeout_ns)
        with self._lock:
            if len(env.payload) < min_message_size:
                raise DataBufferTooSmallError(
                    f"payload {len(env.payload)} < required {min_message_size}"
                )
            if len(env.payload) > len(buffer):
                raise DataBufferTooSmallError(
                    f"payload {len(env.payload)} exceeds buffer "
                    f"{len(buffer)}; required size {len(env.payload)}"
                )
            if self._is_stale(connection_id, env.timestamp_ns):
                raise MessageStaleError(
                    "message exceeded the connection's staleness policy"
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
                self._qos_for(env, seq_gap),
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
                seq_gap = -1
                with self._lock:
                    live = self._connections.get(connection_id)
                    cb = live.callback if live is not None else None
                    ctx = live.callback_context if live is not None else None
                    # Gap bookkeeping observes every envelope on the wire,
                    # before policy filtering, so stale/priority drops do
                    # not surface as phantom gaps later.
                    if cb is not None:
                        seq_gap = self._rx_gap_locked(connection_id, env)
                    # Stale and below-priority messages are dropped, not
                    # delivered; the drop is recorded in stats (no
                    # return-code channel here).
                    if cb is not None and self._is_stale(
                        connection_id, env.timestamp_ns
                    ):
                        cb = None
                    elif cb is not None and self._is_below_priority(
                        connection_id, env.priority
                    ):
                        cb = None
                if cb is not None:
                    try:
                        cb(
                            connection_id,
                            int(env.transaction_id),
                            int(env.message_guid),
                            env.payload,
                            header,
                            self._qos_for(env, seq_gap),
                            ctx,
                        )
                    except Exception:
                        pass
                with self._lock:
                    self._stats.received += 1

            conn.transport.start_callback(_dispatch)
            return ReturnCode.NO_ERROR

    def unregister_callback(self, connection_id: ConnectionId) -> ReturnCode:
        """Unregister a connection's callback. NO_ACTION if none registered.

        FACE 3.1 placed Unregister_Callback on the Base interface; FACE 3.2
        moved it to TypedTS. The Python mirror exposes one method covering
        both (typed and untyped callbacks share the connection slot).

        Safe to call from inside the callback itself: the stop is
        signaled and the dispatch thread is not joined by its own thread.
        """
        with self._lock:
            conn = self._require_open(connection_id)
            if conn.callback is None:
                return ReturnCode.NO_ACTION
            # Detach first so no new dispatch starts; the transport owns
            # the thread handle now.
            conn.callback = None
            conn.callback_context = None
            transport = conn.transport
        # Join the dispatch thread with the TSS lock released: an
        # in-flight dispatch may be blocked acquiring it. stop_callback
        # leaves the socket open, so the connection stays usable and a
        # callback can be re-registered later.
        try:
            transport.stop_callback()
        except Exception:
            pass
        return ReturnCode.NO_ERROR

    # -- internals ---------------------------------------------------------
    def _require_usable(self) -> None:
        """Fail fast while finalize() is tearing down (lock held)."""
        if self._finalizing or not self._initialized:
            raise NotInitializedError

    def _require_initialized(self) -> None:
        if self._finalizing or not self._initialized:
            raise NotInitializedError

    def _require_open(self, connection_id: ConnectionId) -> _Connection:
        self._require_usable()
        conn = self._connections.get(connection_id)
        if conn is None or conn.closed:
            raise ConnectionClosedError(f"connection id {connection_id} is closed")
        return conn
