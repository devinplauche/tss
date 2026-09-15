"""Threading regression tests for the Python TSS/transport mirror.

* A blocked receive never stalls the rest of the instance.
* destroy_connection / finalize interrupt a blocked infinite receive.
* unregister_callback from another thread while callbacks fire: no
  deadlock, no further dispatches.
* A data callback may unregister its own connection (self-stop): no
  deadlock, and the connection stays usable for re-registration.
* Concurrent receives on one socket keep their own per-call timeouts.
"""

import threading
import time

import pytest

from face_tss import (
    Direction,
    FaceTss,
    ReturnCode,
    TimedOutError,
    TssConfigBuilder,
    TIMEOUT_INFINITE,
)


def _bus_pair(name):
    cfg_a = (
        TssConfigBuilder()
        .add("A", direction=Direction.BI_DIRECTIONAL, transport="bus",
             address=f"inproc://tss-thr-{name}-a")
        .add("B", direction=Direction.BI_DIRECTIONAL, transport="bus",
             address=f"inproc://tss-thr-{name}-b")
        .build()
    )
    return cfg_a


def _pubsub_pair(name):
    addr = f"inproc://tss-thr-{name}"
    pub_cfg = (
        TssConfigBuilder()
        .add("CH", direction=Direction.BI_DIRECTIONAL, transport="pubsub",
             role="publisher", address=addr)
        .build()
    )
    sub_cfg = (
        TssConfigBuilder()
        .add("CH", direction=Direction.BI_DIRECTIONAL, transport="pubsub",
             role="subscriber", address=addr)
        .build()
    )
    return pub_cfg, sub_cfg


def test_blocked_receive_does_not_stall_instance():
    tss = FaceTss("thr")
    tss.initialize(_bus_pair("stall"))
    ida, _ = tss.create_connection("a")
    idb, _ = tss.create_connection("b")

    outcome = {}

    def blocked():
        try:
            tss.receive_message(ida, timeout_ns=TIMEOUT_INFINITE)
            outcome["rc"] = "got-data"
        except Exception as exc:  # noqa: BLE001 - any error means "woken"
            outcome["rc"] = type(exc).__name__

    thr = threading.Thread(target=blocked, daemon=True)
    thr.start()
    time.sleep(0.3)  # let the receive park

    # The rest of the instance stays responsive while one thread is
    # parked in an infinite receive.
    t0 = time.monotonic()
    _ = tss.stats
    tss.send_message(idb, b"ping", timeout_ns=2_000_000_000)
    assert time.monotonic() - t0 < 2.0

    # Destroying the connection interrupts the blocked receive.
    assert tss.destroy_connection(ida) == ReturnCode.NO_ERROR
    thr.join(timeout=5.0)
    assert not thr.is_alive(), "blocked receive was not interrupted"
    assert outcome.get("rc") not in (None, "got-data")

    tss.finalize()


def test_finalize_interrupts_blocked_receive():
    tss = FaceTss("thr-fin")
    tss.initialize(_bus_pair("finalize"))
    ida, _ = tss.create_connection("a")

    outcome = {}

    def blocked():
        try:
            tss.receive_message(ida, timeout_ns=TIMEOUT_INFINITE)
            outcome["rc"] = "got-data"
        except Exception as exc:  # noqa: BLE001
            outcome["rc"] = type(exc).__name__

    thr = threading.Thread(target=blocked, daemon=True)
    thr.start()
    time.sleep(0.3)

    tss.finalize()
    thr.join(timeout=5.0)
    assert not thr.is_alive(), "finalize did not interrupt blocked receive"
    assert outcome.get("rc") not in (None, "got-data")


def _stream(tss, conn_id, n, delay=0.005):
    for _ in range(n):
        try:
            tss.send_message(conn_id, b"ping", timeout_ns=2_000_000_000)
        except Exception:  # noqa: BLE001 - shutting down is fine
            break
        time.sleep(delay)


def test_unregister_while_callbacks_fire():
    pub_cfg, sub_cfg = _pubsub_pair("unreg-fire")
    pub = FaceTss("pub")
    sub = FaceTss("sub")
    pub.initialize(pub_cfg)
    sub.initialize(sub_cfg)
    pid, _ = pub.create_connection("ch")
    sid, _ = sub.create_connection("ch")

    count = 0
    count_lock = threading.Lock()

    def cb(connection_id, transaction_id, message_guid, payload, header,
           qos, context):
        nonlocal count
        with count_lock:
            count += 1
        return ReturnCode.NO_ERROR

    assert sub.register_callback(sid, cb) == ReturnCode.NO_ERROR
    streamer = threading.Thread(target=_stream, args=(pub, pid, 60),
                                daemon=True)
    streamer.start()
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        with count_lock:
            if count >= 3:
                break
        time.sleep(0.01)
    with count_lock:
        assert count >= 3

    # Unregister mid-stream from this thread: must not deadlock.
    t0 = time.monotonic()
    assert sub.unregister_callback(sid) == ReturnCode.NO_ERROR
    assert time.monotonic() - t0 < 5.0
    streamer.join(timeout=10.0)

    # After unregister, no further dispatches happen.
    time.sleep(0.3)
    with count_lock:
        c1 = count
    time.sleep(0.3)
    with count_lock:
        assert count == c1

    assert sub.unregister_callback(sid) == ReturnCode.NO_ACTION
    sub.finalize()
    pub.finalize()


def test_callback_self_unregister_keeps_connection_usable():
    pub_cfg, sub_cfg = _pubsub_pair("self-unreg")
    pub = FaceTss("pub")
    sub = FaceTss("sub")
    pub.initialize(pub_cfg)
    sub.initialize(sub_cfg)
    pid, _ = pub.create_connection("ch")
    sid, _ = sub.create_connection("ch")

    state = {"count": 0, "self_rc": None}
    lock = threading.Lock()

    def cb(connection_id, transaction_id, message_guid, payload, header,
           qos, context):
        # Unregister our own connection from inside the data callback:
        # the stop is signaled and the dispatch thread is not joined by
        # itself, so this must return promptly.
        with lock:
            state["count"] += 1
            if state["self_rc"] is None:
                t0 = time.monotonic()
                state["self_rc"] = sub.unregister_callback(sid)
                state["self_dt"] = time.monotonic() - t0
        return ReturnCode.NO_ERROR

    assert sub.register_callback(sid, cb) == ReturnCode.NO_ERROR
    streamer = threading.Thread(target=_stream, args=(pub, pid, 10),
                                daemon=True)
    streamer.start()
    streamer.join(timeout=10.0)

    with lock:
        assert state["count"] >= 1
        assert state["self_rc"] == ReturnCode.NO_ERROR
        assert state["self_dt"] < 5.0, "self-unregister stalled"
    # Exactly one delivery: after self-unregister nothing more arrives.
    time.sleep(0.4)
    with lock:
        c1 = state["count"]
    time.sleep(0.3)
    with lock:
        assert state["count"] == c1 == 1

    # The connection survived the self-unregister: re-register and the
    # callback fires again.
    state2 = {"count": 0}

    def cb2(connection_id, transaction_id, message_guid, payload, header,
            qos, context):
        state2["count"] += 1
        return ReturnCode.NO_ERROR

    assert sub.register_callback(sid, cb2) == ReturnCode.NO_ERROR
    pub.send_message(pid, b"again", timeout_ns=2_000_000_000)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and state2["count"] == 0:
        time.sleep(0.01)
    assert state2["count"] >= 1, "re-registration did not deliver"

    sub.finalize()
    pub.finalize()


def test_unregister_keeps_connection_usable():
    """unregister_callback stops the thread but leaves the socket open."""
    pub_cfg, sub_cfg = _pubsub_pair("unreg-usable")
    pub = FaceTss("pub")
    sub = FaceTss("sub")
    pub.initialize(pub_cfg)
    sub.initialize(sub_cfg)
    pid, _ = pub.create_connection("ch")
    sid, _ = sub.create_connection("ch")
    time.sleep(0.3)

    seen = []

    def cb(connection_id, transaction_id, message_guid, payload, header,
           qos, context):
        seen.append(payload)
        return ReturnCode.NO_ERROR

    assert sub.register_callback(sid, cb) == ReturnCode.NO_ERROR
    assert sub.unregister_callback(sid) == ReturnCode.NO_ERROR

    # Plain send/receive still works on the unregistered connection.
    pub.send_message(pid, b"hello", timeout_ns=2_000_000_000)
    msg, _, _ = sub.receive_message(sid, timeout_ns=5_000_000_000)
    assert msg.payload == b"hello"
    assert seen == []

    sub.finalize()
    pub.finalize()


def test_concurrent_receives_keep_timeouts():
    """A long receive keeps its own timeout while polls hammer the socket.

    The nng timeout options are per-socket; the transport serializes
    same-direction I/O so concurrent calls cannot apply each other's
    timeouts.
    """
    pub_cfg, sub_cfg = _pubsub_pair("timeouts")
    pub = FaceTss("pub")
    sub = FaceTss("sub")
    pub.initialize(pub_cfg)
    sub.initialize(sub_cfg)
    pid, _ = pub.create_connection("ch")
    sid, _ = sub.create_connection("ch")
    time.sleep(0.3)

    outcome = {}

    def long_receive():
        try:
            msg, _, _ = sub.receive_message(sid, timeout_ns=10_000_000_000)
            outcome["rc"] = "got-data"
            outcome["payload"] = msg.payload
        except TimedOutError:
            outcome["rc"] = "timed-out"
        except Exception as exc:  # noqa: BLE001
            outcome["rc"] = type(exc).__name__

    thr = threading.Thread(target=long_receive, daemon=True)
    thr.start()
    time.sleep(0.3)  # let the long receive park

    def delayed_send():
        time.sleep(0.5)
        pub.send_message(pid, b"ping", timeout_ns=2_000_000_000)

    sender = threading.Thread(target=delayed_send, daemon=True)
    sender.start()

    # Many timeout-0 polls racing the parked 10 s receive. They serialize
    # behind it on the receive lock; each must keep its own 0 timeout
    # (returning promptly) rather than inheriting the 10 s one.
    t0 = time.monotonic()
    for _ in range(50):
        with pytest.raises(TimedOutError):
            sub.receive_message(sid, timeout_ns=0)
    poll_dt = time.monotonic() - t0

    sender.join(timeout=5.0)
    thr.join(timeout=12.0)
    assert not thr.is_alive()
    assert outcome.get("rc") == "got-data", outcome
    assert outcome.get("payload") == b"ping"
    # 50 polls at timeout 0 must not have inherited the 10 s timeout.
    assert poll_dt < 5.0

    sub.finalize()
    pub.finalize()
