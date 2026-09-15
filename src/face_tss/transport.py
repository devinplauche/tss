"""nng transport layer.

Each FACE connection maps to exactly one nng socket:

* ``pubsub`` - Pub0/Sub0 fan-out. The publisher ``listen``\\ s on the
  configured address; every subscriber ``dial``\\ s it (non-blocking, so
  start order never matters) and subscribes to the topic
  ``CONNECTION_NAME + b'\\x00'``. Sends prepend that topic to the
  FlatBuffers envelope bytes; receives strip it. The NUL separator keeps
  ``HELLO`` from matching ``HELLO2`` (prefix match would collide without it).
  Best for telemetry fan-out: many readers, publisher never blocks.
* ``bus`` - Bus0 mesh. Every participant dials/listens on the same address;
  all peers receive everything the others send. No topic prefix is used -
  each frame is exactly one FlatBuffers envelope. Best for a small set of
  peers that all both send and receive (BI_DIRECTIONAL command nets).

Timeouts: FACE timeouts are int64 nanoseconds; ``TIMEOUT_INFINITE`` (-1)
blocks forever. pynng wants milliseconds; the conversion helpers keep the
units in one place. ``recv_timeout=0`` polls. A receive that exceeds the
timeout raises :class:`TimedOutError` (FACE TIMED_OUT), which is how the TSS
polling loop distinguishes "no data" from real failures.
"""

from __future__ import annotations

import threading
from typing import Callable

import pynng

from .config import ConnectionConfig
from .envelope import Envelope, decode_envelope, encode_envelope
from .errors import InvalidConfigError, TimedOutError, TransportError
from .types import TIMEOUT_INFINITE

__all__ = [
    "TOPIC_SEPARATOR",
    "ns_to_ms",
    "Transport",
    "PubSubTransport",
    "BusTransport",
    "open_transport",
    "CallbackHandle",
]

#: Topic framing: topic bytes + separator + envelope bytes.
TOPIC_SEPARATOR = b"\x00"

_MS_PER_NS = 1_000_000


def ns_to_ms(timeout_ns: int | None) -> int:
    """Convert a FACE timeout (ns) to the ms value pynng expects."""
    if timeout_ns is None or timeout_ns == TIMEOUT_INFINITE:
        return -1  # pynng: block forever
    if timeout_ns <= 0:
        return 0  # poll
    # Round up so tiny-but-nonzero timeouts still wait at least ~1 ms.
    return max(1, (int(timeout_ns) + _MS_PER_NS - 1) // _MS_PER_NS)


class CallbackHandle:
    """Background receive loop returned by Transport.start_callback().

    ``cancel()`` signals the loop to stop and waits for the thread.
    Calling it from the callback thread itself is safe: the stop is
    signaled and the join is skipped (joining the current thread would
    raise), so the loop simply exits after the in-flight dispatch.
    """

    def __init__(self, stop: Callable[[], None], thread: threading.Thread) -> None:
        self._stop = stop
        self._thread = thread

    def request_stop(self) -> None:
        self._stop()

    def join(self, timeout: float | None = None) -> None:
        # Never join the current thread: a callback that unregisters
        # itself would deadlock (or raise RuntimeError) otherwise.
        if threading.current_thread() is self._thread:
            return
        self._thread.join(timeout=timeout)

    def cancel(self, timeout: float = 5.0) -> None:
        self.request_stop()
        self.join(timeout=timeout)

    @property
    def alive(self) -> bool:
        return self._thread.is_alive()


class Transport:
    """One nng socket bound to one FACE connection."""

    def __init__(self, config: ConnectionConfig) -> None:
        self.config = config
        self._socket: pynng.Socket | None = None
        self._lock = threading.Lock()
        # The nng timeout options are per-socket, not per-call, so a
        # send and a receive that program them concurrently would apply
        # each other's timeouts. Serialize same-direction I/O; sends and
        # receives stay independent of each other.
        self._send_lock = threading.Lock()
        self._recv_lock = threading.Lock()
        self._callback_handle: CallbackHandle | None = None

    # -- lifecycle ------------------------------------------------------
    def open(self) -> None:
        raise NotImplementedError

    def close(self) -> None:
        """Close the socket, aborting any blocked send/receive.

        Idempotent. Frees nothing else; see shutdown() for the ordered
        teardown that also stops the callback thread first.
        """
        with self._lock:
            sock, self._socket = self._socket, None
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass

    def stop_callback(self) -> None:
        """Stop the callback thread without closing the transport.

        Signals the loop to stop and joins it, leaving the socket open
        so the connection stays usable (e.g. for re-registration). Safe
        to call from any thread, including the callback thread itself
        (the join is skipped then); idempotent.
        """
        with self._lock:
            handle, self._callback_handle = self._callback_handle, None
        if handle is not None:
            handle.cancel()

    def shutdown(self) -> None:
        """Ordered teardown: stop callbacks, abort blocked I/O, join.

        Signals the callback loop to stop, then closes the socket so any
        thread blocked in send()/receive() wakes with an error, then
        joins the callback thread. Safe to call from any thread,
        including the callback thread itself (the join is skipped then).
        After this returns no callback dispatch is in flight.
        """
        self.stop_callback()
        # Close before joining: a callback thread parked in a blocking
        # receive would otherwise stall the join indefinitely.
        self.close()

    @property
    def is_open(self) -> bool:
        with self._lock:
            return self._socket is not None

    def _require(self) -> pynng.Socket:
        with self._lock:
            sock = self._socket
        if sock is None:
            raise TransportError(
                f"connection {self.config.name}: transport is not open"
            )
        return sock

    # -- data path ------------------------------------------------------
    def send(self, env: Envelope, timeout_ns: int = TIMEOUT_INFINITE) -> None:
        raise NotImplementedError

    def receive(self, timeout_ns: int) -> Envelope:
        raise NotImplementedError

    # -- callback (FACE Register_Callback equivalent) -------------------
    def start_callback(
        self, handler: Callable[[Envelope], None]
    ) -> CallbackHandle:
        """Invoke ``handler`` on a daemon thread for every received envelope.

        The loop polls in 100 ms slices; ``shutdown()`` aborts a blocked
        receive by closing the socket. Only one callback runs per
        transport; starting a second one replaces the first.
        """
        stop_event = threading.Event()

        def _loop() -> None:
            while not stop_event.is_set():
                try:
                    env = self.receive(100_000_000)  # 100 ms poll slice
                except TimedOutError:
                    continue
                except Exception:
                    # Socket closed (shutdown) or a fatal transport
                    # error: leave rather than spin on a dead socket.
                    break
                if stop_event.is_set():
                    break
                try:
                    handler(env)
                except Exception:
                    continue

        thread = threading.Thread(
            target=_loop,
            name=f"face-tss-{self.config.name}",
            daemon=True,
        )
        thread.start()
        handle = CallbackHandle(stop_event.set, thread)
        with self._lock:
            old, self._callback_handle = self._callback_handle, handle
        if old is not None:
            # Join outside the transport lock: the old loop may briefly
            # need it on its way out.
            old.cancel()
        return handle


class PubSubTransport(Transport):
    """Pub0/Sub0 transport with per-connection topic filtering."""

    def __init__(self, config: ConnectionConfig) -> None:
        super().__init__(config)
        self._topic = config.name.encode("utf-8") + TOPIC_SEPARATOR

    @property
    def topic(self) -> bytes:
        return self._topic

    def open(self) -> None:
        self.close()
        try:
            if self.config.role == "publisher":
                sock: pynng.Socket = pynng.Pub0(listen=self.config.address)
            elif self.config.role == "subscriber":
                sock = pynng.Sub0(dial=self.config.address, block_on_dial=False)
                sock.subscribe(self._topic)
            else:  # pragma: no cover - validated by ConnectionConfig
                raise InvalidConfigError(
                    f"connection {self.config.name}: bad pubsub role "
                    f"{self.config.role!r}"
                )
        except Exception as exc:
            raise TransportError(
                f"connection {self.config.name}: failed to open "
                f"{self.config.role} on {self.config.address}: {exc}"
            ) from exc
        with self._lock:
            self._socket = sock

    def send(self, env: Envelope, timeout_ns: int = TIMEOUT_INFINITE) -> None:
        sock = self._require()
        with self._send_lock:
            sock.send_timeout = ns_to_ms(timeout_ns)
            try:
                sock.send(self._topic + encode_envelope(env))
            except pynng.Timeout as exc:
                raise TimedOutError(
                    f"connection {self.config.name}: send timed out"
                ) from exc
            except Exception as exc:
                raise TransportError(
                    f"connection {self.config.name}: nng send failed: {exc}"
                ) from exc

    def receive(self, timeout_ns: int) -> Envelope:
        sock = self._require()
        with self._recv_lock:
            sock.recv_timeout = ns_to_ms(timeout_ns)
            try:
                raw = bytes(sock.recv())
            except pynng.Timeout as exc:
                raise TimedOutError(
                    f"connection {self.config.name}: receive timed out"
                ) from exc
            except Exception as exc:
                raise TransportError(
                    f"connection {self.config.name}: nng recv failed: {exc}"
                ) from exc
        if not raw.startswith(self._topic):
            raise TransportError(
                f"connection {self.config.name}: frame topic mismatch"
            )
        try:
            return decode_envelope(raw[len(self._topic) :])
        except ValueError as exc:
            raise TransportError(
                f"connection {self.config.name}: bad envelope: {exc}"
            ) from exc


class BusTransport(Transport):
    """Bus0 mesh transport - raw envelopes, every peer hears every peer."""

    def open(self) -> None:
        self.close()
        try:
            # Bus0: this end listens; peers dial the same address. Peers that
            # start first with listen would clash (AddressInUse), so callers
            # that only ever dial should use dial mode... pynng Bus0 supports
            # listen= or dial=; listen here, dial from the other TSS nodes.
            sock = pynng.Bus0(listen=self.config.address)
        except pynng.AddressInUse:
            try:
                sock = pynng.Bus0(
                    dial=self.config.address, block_on_dial=False
                )
            except Exception as exc:
                raise TransportError(
                    f"connection {self.config.name}: failed to join bus "
                    f"{self.config.address}: {exc}"
                ) from exc
        except Exception as exc:
            raise TransportError(
                f"connection {self.config.name}: failed to open bus on "
                f"{self.config.address}: {exc}"
            ) from exc
        with self._lock:
            self._socket = sock

    def open_dial(self) -> None:
        """Join the bus as a dialer (for nodes that must not listen)."""
        self.close()
        try:
            with self._lock:
                self._socket = pynng.Bus0(
                    dial=self.config.address, block_on_dial=False
                )
        except Exception as exc:
            raise TransportError(
                f"connection {self.config.name}: failed to dial bus "
                f"{self.config.address}: {exc}"
            ) from exc

    def send(self, env: Envelope, timeout_ns: int = TIMEOUT_INFINITE) -> None:
        sock = self._require()
        with self._send_lock:
            sock.send_timeout = ns_to_ms(timeout_ns)
            try:
                sock.send(encode_envelope(env))
            except pynng.Timeout as exc:
                raise TimedOutError(
                    f"connection {self.config.name}: send timed out"
                ) from exc
            except Exception as exc:
                raise TransportError(
                    f"connection {self.config.name}: nng send failed: {exc}"
                ) from exc

    def receive(self, timeout_ns: int) -> Envelope:
        sock = self._require()
        with self._recv_lock:
            sock.recv_timeout = ns_to_ms(timeout_ns)
            try:
                raw = bytes(sock.recv())
            except pynng.Timeout as exc:
                raise TimedOutError(
                    f"connection {self.config.name}: receive timed out"
                ) from exc
            except Exception as exc:
                raise TransportError(
                    f"connection {self.config.name}: nng recv failed: {exc}"
                ) from exc
        try:
            return decode_envelope(raw)
        except ValueError as exc:
            raise TransportError(
                f"connection {self.config.name}: bad envelope: {exc}"
            ) from exc


def open_transport(config: ConnectionConfig) -> Transport:
    """Create (and open) the transport for a connection config."""
    if config.transport == "pubsub":
        transport: Transport = PubSubTransport(config)
    elif config.transport == "bus":
        transport = BusTransport(config)
    else:  # pragma: no cover - validated by ConnectionConfig
        raise InvalidConfigError(f"unknown transport {config.transport!r}")
    transport.open()
    return transport
