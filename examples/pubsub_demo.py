"""Pub/Sub demo: one publisher process streams PositionReports, another prints them.

Run in two terminals (repo root on PYTHONPATH / package installed)::

    python examples/pubsub_demo.py pub
    python examples/pubsub_demo.py sub

Or with explicit configs::

    python examples/pubsub_demo.py pub configs/pubsub_publisher.json
    python examples/pubsub_demo.py sub configs/pubsub_subscriber.json

Start the publisher first: it owns the listen side of the nng address.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from face_tss import (  # noqa: E402
    FaceTss,
    PositionReport,
    TimedOutError,
    config_from_file,
)

DEFAULT_PUB = Path(__file__).resolve().parents[1] / "configs" / "pubsub_publisher.json"
DEFAULT_SUB = Path(__file__).resolve().parents[1] / "configs" / "pubsub_subscriber.json"


def run_publisher(config_path: Path, count: int = 20) -> None:
    tss = FaceTss("demo-publisher")
    rc = tss.initialize(config_from_file(config_path))
    print(f"[pub] initialize -> {rc.name}")
    conn_id, max_size = tss.create_connection("position")
    print(f"[pub] created POSITION id={conn_id} max={max_size}")
    time.sleep(0.5)  # let the subscriber's dial + subscription settle
    for i in range(count):
        report = PositionReport(
            vehicle_id="DEMO-1",
            latitude_deg=37.0 + i * 0.001,
            longitude_deg=-122.0 - i * 0.001,
            altitude_m=1000.0 + i,
            heading_deg=(90.0 + i * 5) % 360,
            valid=True,
        )
        tss.send_message(conn_id, report.serialize(), transaction_id=i + 1)
        print(f"[pub] sent seq={i + 1} lat={report.latitude_deg:.4f}")
        time.sleep(0.2)
    tss.destroy_connection(conn_id)
    tss.finalize()
    print("[pub] done")


def run_subscriber(config_path: Path, timeout_s: float = 15.0) -> None:
    tss = FaceTss("demo-subscriber")
    rc = tss.initialize(config_from_file(config_path))
    print(f"[sub] initialize -> {rc.name}")
    conn_id, _ = tss.create_connection("POSITION")
    print(f"[sub] created POSITION id={conn_id} (waiting for data...)")
    deadline = time.time() + timeout_s
    received = 0
    while time.time() < deadline:
        try:
            msg = tss.receive_message(conn_id, timeout_ns=500_000_000)
        except TimedOutError:
            continue
        report = PositionReport.deserialize(msg.payload)
        received += 1
        h = msg.header
        print(
            f"[sub] #{received} txn={h.transaction_id} seq={h.sequence_number} "
            f"vehicle={report.vehicle_id} lat={report.latitude_deg:.4f} "
            f"lon={report.longitude_deg:.4f} alt={report.altitude_m:.0f}"
        )
    tss.destroy_connection(conn_id)
    tss.finalize()
    print(f"[sub] done, received={received}")


def main(argv: list[str]) -> int:
    if len(argv) < 2 or argv[1] not in ("pub", "sub"):
        print("usage: pubsub_demo.py [pub|sub] [config.json]")
        return 2
    mode = argv[1]
    if mode == "pub":
        cfg = Path(argv[2]) if len(argv) > 2 else DEFAULT_PUB
        run_publisher(cfg)
    else:
        cfg = Path(argv[2]) if len(argv) > 2 else DEFAULT_SUB
        run_subscriber(cfg)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
