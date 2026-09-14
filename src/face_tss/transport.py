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
    """Background receive loop returned by Transport.start_callback()."""

    def __init__(self, stop: Callable[[], None], thread: threading.Thread) -> None:
        self._stop = stop
        self._thread = thread

    def cancel(self, timeout: float = 5.0) -> None:
        self._stop()
        self._thread.join(timeout=timeout)

    @property
    def alive(self) -> bool:
        return self._thread.is_alive()


class Transport:
    """One nng socket bound to one FACE connection."""

    def __init__(self, config: ConnectionConfig) -> None:
        self.config = config
        self._socket: pynng.Socket | None = None
        self._lock = threading.Lock()

    # -- lifecycle ------------------------------------------------------
    def open(self) -> None:
        raise NotImplementedError

    def close(self) -> None:
        sock, self._socket = self._socket, None
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass

    @property
    def is_open(self) -> bool:
        return self._socket is not None

    def _require(self) -> pynng.Socket:
        sock = self._socket
        if sock is None:
            raise TransportError(
                f"connection {self.config.name}: transport is not open"
            )
        return sock

    # -- data path ------------------------------------------------------
    def send(self, env: Envelope) -> None:
        raise NotImplementedError

    def receive(self, timeout_ns: int) -> Envelope:
        raise NotImplementedError

    # -- callback (FACE Register_Callback equivalent) -------------------
    def start_callback(
        self, handler: Callable[[Envelope], None]
    ) -> CallbackHandle:
        """Invoke ``handler`` on a daemon thread for every received envelope."""
        stop_event = threading.Event()

        def _loop() -> None:
            while not stop_event.is_set():
                try:
                    env = self.receive(100_000_000)  # 100 ms poll slice
                except TimedOutError:
                    continue
                except Exception:
                    if stop_event.is_set():
                        break
                    continue
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
        return CallbackHandle(stop_event.set, thread)


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
        self._socket = sock

    def send(self, env: Envelope) -> None:
        sock = self._require()
        try:
            sock.send(self._topic + encode_envelope(env))
        except Exception as exc:
            raise TransportError(
                f"connection {self.config.name}: nng send failed: {exc}"
            ) from exc

    def receive(self, timeout_ns: int) -> Envelope:
        sock = self._require()
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
        self._socket = sock

    def open_dial(self) -> None:
        """Join the bus as a dialer (for nodes that must not listen)."""
        self.close()
        try:
            self._socket = pynng.Bus0(
                dial=self.config.address, block_on_dial=False
            )
        except Exception as exc:
            raise TransportError(
                f"connection {self.config.name}: failed to dial bus "
                f"{self.config.address}: {exc}"
            ) from exc

    def send(self, env: Envelope) -> None:
        sock = self._require()
        try:
            sock.send(encode_envelope(env))
        except Exception as exc:
            raise TransportError(
                f"connection {self.config.name}: nng send failed: {exc}"
            ) from exc

    def receive(self, timeout_ns: int) -> Envelope:
        sock = self._require()
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
