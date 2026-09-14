"""Bus demo: two peers exchange pings over an nng Bus0 mesh.

The broker anchors the mesh (owns the listen side)::

    python -m face_tss.broker configs/bus_demo.json   # terminal 1

Then run two peers (either order)::

    python examples/bus_demo.py A   # terminal 2
    python examples/bus_demo.py B   # terminal 3

Each peer sends 5 pings and prints everything it receives. Note a Bus0
socket does not deliver its own sends back to itself, so peer A only sees
peer B's messages and vice versa.
"""

from __future__ import annotations

import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from face_tss import FaceTss, TimedOutError, config_from_file  # noqa: E402

CONFIG = Path(__file__).resolve().parents[1] / "configs" / "bus_demo.json"


def run_peer(name: str, count: int = 5) -> None:
    tss = FaceTss(f"bus-peer-{name}")
    tss.initialize(config_from_file(CONFIG))
    conn_id, _ = tss.create_connection("command")
    print(f"[{name}] up on COMMAND id={conn_id}")

    stop = threading.Event()

    def _rx() -> None:
        while not stop.is_set():
            try:
                msg = tss.receive_message(conn_id, timeout_ns=200_000_000)
            except TimedOutError:
                continue
            print(f"[{name}] got {msg.payload.decode()} (txn={msg.header.transaction_id})")

    rx = threading.Thread(target=_rx, daemon=True)
    rx.start()
    time.sleep(1.0)  # let the mesh settle
    for i in range(1, count + 1):
        body = f"ping-{name}-{i}".encode()
        tss.send_message(conn_id, body, transaction_id=i)
        print(f"[{name}] sent {body.decode()}")
        time.sleep(0.5)
    time.sleep(1.0)  # drain inbound
    stop.set()
    rx.join(timeout=3.0)
    tss.destroy_connection(conn_id)
    tss.finalize()
    print(f"[{name}] done (sent={tss.stats.sent} rx={tss.stats.received})")


if __name__ == "__main__":
    peer = sys.argv[1] if len(sys.argv) > 1 else "A"
    run_peer(peer)
