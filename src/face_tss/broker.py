"""A tiny central databus (broker) process for ``bus``-mesh demos.

nng Bus0 is a full mesh - every node hears every other node - so a local
test rig still needs exactly one process to ``listen`` on each address while
the rest dial. This broker owns the listen side for a set of addresses and
forwards nothing itself: Bus0 delivers peer messages directly once pipes are
up; the broker's presence just anchors the mesh so dial-only TSS nodes can
join in any order.

Run: ``python -m face_tss.broker bus_config.json`` (or import and use
programmatically). Exits on Ctrl-C.
"""

from __future__ import annotations

import json
import signal
import sys
import threading
import time

import pynng

__all__ = ["Broker", "main"]


class Broker:
    """Listens on a set of Bus0 addresses to anchor the mesh."""

    def __init__(self, addresses: list[str]) -> None:
        self.addresses = list(addresses)
        self._sockets: list[pynng.Bus0] = []
        self._stop = threading.Event()

    def start(self) -> None:
        for addr in self.addresses:
            try:
                sock = pynng.Bus0(listen=addr)
            except pynng.AddressInUse:
                sock = pynng.Bus0(dial=addr, block_on_dial=False)
            self._sockets.append(sock)
        print(f"[broker] up on {', '.join(self.addresses)}", flush=True)

    def run_forever(self) -> None:
        def _stop(*_: object) -> None:
            self._stop.set()

        try:
            signal.signal(signal.SIGINT, _stop)  # type: ignore[arg-type]
            signal.signal(signal.SIGTERM, _stop)  # type: ignore[arg-type]
        except ValueError:
            pass  # not on the main thread; loop still exits via stop()
        while not self._stop.wait(0.5):
            pass

    def stop(self) -> None:
        self._stop.set()
        for sock in self._sockets:
            try:
                sock.close()
            except Exception:
                pass
        self._sockets.clear()
        print("[broker] down", flush=True)


def _addresses_from_config_file(path: str) -> list[str]:
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    addrs: list[str] = []
    for conn in data.get("connections", []):
        if conn.get("transport") == "bus" and conn.get("address"):
            addrs.append(conn["address"])
    # Preserve order, drop duplicates.
    return list(dict.fromkeys(addrs))


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if not args or args[0] in ("-h", "--help"):
        print(
            "usage: python -m face_tss.broker <config.json> [addr ...]\n"
            "  Listens on every bus-transport address so dial-only nodes can join.",
            flush=True,
        )
        return 0 if args else 2
    addrs: list[str] = []
    for arg in args:
        if arg.endswith(".json"):
            addrs.extend(_addresses_from_config_file(arg))
        else:
            addrs.append(arg)
    addrs = list(dict.fromkeys(addrs))
    if not addrs:
        print("[broker] no addresses to listen on", flush=True)
        return 2
    broker = Broker(addrs)
    broker.start()
    try:
        broker.run_forever()
    finally:
        broker.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
