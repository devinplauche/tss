"""Callback demo: subscriber uses Register_Callback instead of polling.

Run in two terminals::

    python examples/pubsub_demo.py pub
    python examples/callback_demo.py
"""

from __future__ import annotations

import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from face_tss import FaceTss, PositionReport, ReturnCode, config_from_file  # noqa: E402

CONFIG = Path(__file__).resolve().parents[1] / "configs" / "pubsub_subscriber.json"


def main() -> int:
    tss = FaceTss("demo-callback")
    tss.initialize(config_from_file(CONFIG))
    conn_id, _ = tss.create_connection("POSITION")

    arrived = threading.Event()
    seen: list[str] = []

    def on_message(connection_id, transaction_id, message_guid, payload,
                   header, qos, context):
        report = PositionReport.deserialize(payload)
        seen.append(report.vehicle_id)
        print(
            f"[cb] conn={connection_id} txn={transaction_id} "
            f"iuid={header.instance_uid} vehicle={report.vehicle_id} "
            f"lat={report.latitude_deg:.4f}"
        )
        arrived.set()
        return ReturnCode.NO_ERROR

    rc = tss.register_callback(conn_id, on_message)
    print(f"[cb] register -> {rc.name} (waiting ~12s for publisher data...)")
    arrived.wait(timeout=12.0)
    rc = tss.unregister_callback(conn_id)
    print(f"[cb] unregister -> {rc.name} (saw {len(seen)} messages)")
    if rc != ReturnCode.NO_ERROR:
        print("[cb] ERROR: unregister failed")
        return 1
    tss.destroy_connection(conn_id)
    tss.finalize()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
