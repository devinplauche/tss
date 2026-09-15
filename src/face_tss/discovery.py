"""FACE TSS UDP-broadcast peer discovery - pure-Python implementation.

Mirrors ``c/include/face_tss/discovery.h`` / ``c/src/discovery.c``: TSS
instances announce themselves via UDP datagrams and discover peers
without pre-configured addresses.

Wire protocol (IPv4 only): UDP datagrams on port 51970. Datagram layout:
magic ``b"FTSSD1"`` (6 bytes) | name (NUL-terminated, max 64 incl. NUL) |
address (NUL-terminated, max 256 incl. NUL) | interval_ms (8 bytes,
little-endian).

Threading mirrors the C implementation: a background broadcaster thread
(started by :meth:`announce`) and a background listener thread (started
by :meth:`create`, runs for the object's lifetime). A single re-entrant
condition variable guards the peer table; peer entries expire after 3x
the sender's interval without a refresh.

The broadcaster uses a connected UDP socket (``connect()`` + ``send()``)
rather than ``sendto()``: both reach a broadcast destination, and
``connect()`` + ``send()`` works in sandboxed environments where
``sendto()`` is restricted. The default destination is the limited
broadcast address ``255.255.255.255``; use :meth:`set_destination` to
point at ``127.0.0.1`` for loopback testing where broadcast is
unavailable.
"""

from __future__ import annotations

import socket
import struct
import threading
import time

from .errors import (
    FaceTssError,
    InvalidParamError,
    TimedOutError,
)
from .types import ReturnCode

#: UDP port for discovery datagrams.
DISCOVERY_PORT: int = 51970

#: Magic prefix of every discovery datagram.
DISCOVERY_MAGIC: bytes = b"FTSSD1"

#: Max bytes for a peer name / address, including the NUL terminator.
DISCOVERY_NAME_MAX: int = 64
DISCOVERY_ADDRESS_MAX: int = 256

#: Announce interval bounds (ms).
DISCOVERY_INTERVAL_MIN_MS: int = 100
DISCOVERY_INTERVAL_MAX_MS: int = 60000

#: A peer is forgotten after this many missed announce intervals.
DISCOVERY_EXPIRE_FACTOR: int = 3


def _now_ms() -> int:
    return time.monotonic_ns() // 1_000_000


def _build_datagram(name: str, address: str, interval_ms: int) -> bytes:
    nb = name.encode("utf-8") + b"\x00"
    ab = address.encode("utf-8") + b"\x00"
    if len(nb) > DISCOVERY_NAME_MAX or len(ab) > DISCOVERY_ADDRESS_MAX:
        raise InvalidParamError("name/address too long")
    return DISCOVERY_MAGIC + nb + ab + struct.pack("<Q", interval_ms)


def _parse_datagram(buf: bytes) -> tuple[str, str, int] | None:
    if len(buf) < len(DISCOVERY_MAGIC) + 1 + 1 + 8:
        return None
    if not buf.startswith(DISCOVERY_MAGIC):
        return None
    pos = len(DISCOVERY_MAGIC)
    try:
        end = buf.index(b"\x00", pos, pos + DISCOVERY_NAME_MAX)
    except ValueError:
        return None
    name = buf[pos:end].decode("utf-8", "replace")
    pos = end + 1
    try:
        end = buf.index(b"\x00", pos, pos + DISCOVERY_ADDRESS_MAX)
    except ValueError:
        return None
    address = buf[pos:end].decode("utf-8", "replace")
    pos = end + 1
    if len(buf) < pos + 8:
        return None
    (interval_ms,) = struct.unpack("<Q", buf[pos : pos + 8])
    if not (DISCOVERY_INTERVAL_MIN_MS <= interval_ms <= DISCOVERY_INTERVAL_MAX_MS):
        return None
    return name, address, interval_ms


class _Peer:
    __slots__ = ("name", "address", "interval_ms", "last_seen_ms")

    def __init__(self, name: str, address: str, interval_ms: int) -> None:
        self.name = name
        self.address = address
        self.interval_ms = interval_ms
        self.last_seen_ms = _now_ms()


class Discovery:
    """UDP-broadcast peer discovery. Use as a context manager or call
    :meth:`destroy` explicitly."""

    def __init__(self) -> None:
        self._cond = threading.Condition(threading.RLock())
        self._shutdown = False
        self._peers: dict[str, _Peer] = {}
        self._destination = "255.255.255.255"
        self._announcing = False
        self._bcast_name = ""
        self._bcast_address = ""
        self._bcast_interval_ms = 0
        self._bcast_sock: socket.socket | None = None
        self._bcast_stop = False
        self._bcast_thread: threading.Thread | None = None
        self._listener_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._listener_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener_sock.bind(("0.0.0.0", DISCOVERY_PORT))
        self._listener_sock.settimeout(0.2)
        self._listener_thread = threading.Thread(
            target=self._listener_main, daemon=True, name="face-tss-discovery"
        )
        self._listener_thread.start()

    @classmethod
    def create(cls) -> "Discovery":
        """Create a discovery instance (listener starts immediately)."""
        return cls()

    def destroy(self) -> None:
        """Stop threads and release resources. Idempotent."""
        with self._cond:
            if self._shutdown:
                return
            self._shutdown = True
            self._bcast_stop = True
            self._cond.notify_all()
        if self._bcast_thread is not None:
            self._bcast_thread.join()
            self._bcast_thread = None
        self._listener_thread.join()
        with self._cond:
            if self._bcast_sock is not None:
                self._bcast_sock.close()
                self._bcast_sock = None
            self._listener_sock.close()
            self._peers.clear()

    def __enter__(self) -> "Discovery":
        return self

    def __exit__(self, *exc: object) -> None:
        self.destroy()

    # -- internals ----------------------------------------------------

    def _expired(self, peer: _Peer, now: int) -> bool:
        return now - peer.last_seen_ms > peer.interval_ms * DISCOVERY_EXPIRE_FACTOR

    def _reap_expired(self, now: int) -> None:
        dead = [n for n, p in self._peers.items() if self._expired(p, now)]
        for n in dead:
            del self._peers[n]

    def _is_self(self, name: str, address: str) -> bool:
        return (
            self._announcing
            and self._bcast_name == name
            and self._bcast_address == address
        )

    def _note_peer(self, name: str, address: str, interval_ms: int) -> None:
        if self._is_self(name, address):
            return
        now = _now_ms()
        peer = self._peers.get(name)
        if peer is None:
            peer = _Peer(name, address, interval_ms)
            self._peers[name] = peer
        else:
            peer.address = address
            peer.interval_ms = interval_ms
            peer.last_seen_ms = now
        self._reap_expired(now)
        self._cond.notify_all()

    def _listener_main(self) -> None:
        while True:
            with self._cond:
                if self._shutdown:
                    return
            try:
                buf, _ = self._listener_sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            parsed = _parse_datagram(buf)
            if parsed is None:
                continue
            name, address, interval_ms = parsed
            with self._cond:
                if not self._shutdown:
                    self._note_peer(name, address, interval_ms)

    def _broadcaster_main(self) -> None:
        try:
            datagram = _build_datagram(
                self._bcast_name, self._bcast_address, self._bcast_interval_ms
            )
        except InvalidParamError:
            return
        sock = self._bcast_sock
        assert sock is not None
        while True:
            with self._cond:
                if self._bcast_stop or self._shutdown:
                    return
                interval = self._bcast_interval_ms
            try:
                sock.send(datagram)
            except OSError:
                pass  # best effort; next interval retries
            slept = 0.0
            while slept < interval / 1000.0:
                time.sleep(min(0.05, interval / 1000.0 - slept))
                slept += 0.05
                with self._cond:
                    if self._bcast_stop or self._shutdown:
                        return

    # -- public API ---------------------------------------------------

    def set_destination(self, destination: str) -> ReturnCode:
        """Override the broadcast destination (dotted IPv4). Must be
        called before :meth:`announce`."""
        if not destination:
            raise InvalidParamError("destination is required")
        try:
            socket.inet_pton(socket.AF_INET, destination)
        except OSError:
            raise InvalidParamError(f"bad IPv4 destination: {destination!r}")
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            probe.connect((destination, DISCOVERY_PORT))
        except OSError as e:
            raise FaceTssError(ReturnCode.NOT_AVAILABLE, str(e))
        finally:
            probe.close()
        with self._cond:
            if self._announcing:
                raise InvalidParamError("cannot change destination while announcing")
            self._destination = destination
        return ReturnCode.NO_ERROR

    def announce(self, name: str, address: str, interval_ms: int) -> ReturnCode:
        """Start broadcasting our presence every ``interval_ms``."""
        if not name or not address:
            raise InvalidParamError("name and address are required")
        if len(name.encode()) >= DISCOVERY_NAME_MAX or len(address.encode()) >= DISCOVERY_ADDRESS_MAX:
            raise InvalidParamError("name/address too long")
        if not (DISCOVERY_INTERVAL_MIN_MS <= interval_ms <= DISCOVERY_INTERVAL_MAX_MS):
            raise InvalidParamError(
                f"interval_ms must be in [{DISCOVERY_INTERVAL_MIN_MS}, "
                f"{DISCOVERY_INTERVAL_MAX_MS}]"
            )
        with self._cond:
            if self._announcing:
                if self._bcast_name == name and self._bcast_address == address:
                    return ReturnCode.NO_ACTION
                raise FaceTssError(
                    ReturnCode.NOT_AVAILABLE,
                    "already announcing a different name/address",
                )
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                sock.connect((self._destination, DISCOVERY_PORT))
            except OSError as e:
                sock.close()
                raise FaceTssError(ReturnCode.NOT_AVAILABLE, str(e))
            self._bcast_sock = sock
            self._bcast_name = name
            self._bcast_address = address
            self._bcast_interval_ms = interval_ms
            self._bcast_stop = False
            self._announcing = True
            self._bcast_thread = threading.Thread(
                target=self._broadcaster_main, daemon=True, name="face-tss-bcast"
            )
            self._bcast_thread.start()
        return ReturnCode.NO_ERROR

    def stop_announce(self) -> ReturnCode:
        """Stop broadcasting. Returns NO_ACTION if not announcing."""
        with self._cond:
            if not self._announcing:
                return ReturnCode.NO_ACTION
            self._bcast_stop = True
        assert self._bcast_thread is not None
        self._bcast_thread.join()
        with self._cond:
            self._bcast_thread = None
            if self._bcast_sock is not None:
                self._bcast_sock.close()
                self._bcast_sock = None
            self._announcing = False
        return ReturnCode.NO_ERROR

    def lookup(self, name: str, timeout_ms: int) -> str:
        """Block up to ``timeout_ms`` for a peer called ``name``; return
        its address. ``timeout_ms == 0`` polls once. Raises
        :class:`TimedOutError` on expiry."""
        if not name:
            raise InvalidParamError("name is required")
        if timeout_ms < 0:
            raise InvalidParamError("timeout_ms must be >= 0")
        deadline = _now_ms() + timeout_ms
        with self._cond:
            while True:
                now = _now_ms()
                self._reap_expired(now)
                peer = self._peers.get(name)
                if peer is not None and not self._expired(peer, now):
                    return peer.address
                if timeout_ms == 0 or now >= deadline:
                    raise TimedOutError(f"no peer named {name!r}")
                self._cond.wait(timeout=max(0.0, (deadline - now) / 1000.0))

    def list(self) -> list[tuple[str, str]]:
        """Snapshot of currently known (non-expired) peers as
        ``(name, address)`` pairs."""
        with self._cond:
            now = _now_ms()
            self._reap_expired(now)
            return [(p.name, p.address) for p in self._peers.values()]
