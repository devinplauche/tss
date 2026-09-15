"""Live data-movement tests over real nng sockets (loopback).

* pubsub fan-out: publisher TSS -> subscriber TSS, payload + header intact,
  sequence numbers increasing, timed-out poll raises TIMED_OUT.
* bus mesh: two dial-only... (one listen anchor + one dialer) peers exchange.
* buffer-too-small: oversize send rejected before touching the wire.
* callback: Register_Callback delivers without polling.
"""

import threading
import time

import pytest

from face_tss import (
    BusTransport,
    DataBufferTooSmallError,
    Direction,
    FaceTss,
    PositionReport,
    ReturnCode,
    TimedOutError,
    TssConfigBuilder,
)


def _pubsub_pair(tcp_addr):
    addr = tcp_addr()
    pub_cfg = (
        TssConfigBuilder()
        .add("POSITION", direction=Direction.BI_DIRECTIONAL,
             transport="pubsub", role="publisher", address=addr)
        .build()
    )
    sub_cfg = (
        TssConfigBuilder()
        .add("POSITION", direction=Direction.BI_DIRECTIONAL,
             transport="pubsub", role="subscriber", address=addr)
        .build()
    )
    pub = FaceTss("pub")
    sub = FaceTss("sub")
    pub.initialize(pub_cfg)
    sub.initialize(sub_cfg)
    pub_id, _ = pub.create_connection("position")
    sub_id, _ = sub.create_connection("POSITION")
    time.sleep(0.4)  # dial + subscription settle
    return pub, pub_id, sub, sub_id


def test_pubsub_send_receive_round_trip(tcp_addr):
    pub, pub_id, sub, sub_id = _pubsub_pair(tcp_addr)
    try:
        report = PositionReport("N123", 37.5, -122.25, 1500.0, 270.0, True)
        # Explicit transaction id passes through.
        used = pub.send_message(pub_id, report.serialize(), 5_000_000_000,
                                transaction_id=11)
        assert used == 11
        msg, txn, qos = sub.receive_message(sub_id, timeout_ns=5_000_000_000)
        assert PositionReport.deserialize(msg.payload) == report
        assert txn == 11
        assert msg.header.source_uid == pub.source_id
        assert msg.header.instance_uid != 0
        assert msg.header.timestamp > 0
        assert len(qos) == 2
        assert qos[0].name == "message_age_ns"
        assert qos[1].name == "priority"
        assert qos[1].value == 0

        # Unspecified transaction id: the TSS assigns one (inout semantics).
        used2 = pub.send_message(pub_id, b"second", 5_000_000_000)
        assert used2 != 0
        msg2, txn2, _ = sub.receive_message(sub_id,
                                            timeout_ns=5_000_000_000)
        assert msg2.payload == b"second"
        assert txn2 == used2
        assert msg2.header.instance_uid != msg.header.instance_uid
    finally:
        pub.finalize()
        sub.finalize()


def test_receive_timeout_maps_to_timed_out(tcp_addr):
    _, _, sub, sub_id = _pubsub_pair(tcp_addr)
    try:
        t0 = time.time()
        with pytest.raises(TimedOutError):
            sub.receive_message(sub_id, timeout_ns=200_000_000)
        assert (time.time() - t0) < 5
        assert sub.try_receive(sub_id) is None
        assert sub.stats.receive_timeouts >= 2
    finally:
        sub.finalize()


def test_topic_isolation(tcp_addr):
    """A subscriber with topic AAA ignores frames for topic BBB.

    nng allows exactly one listener per address, so this exercises the raw
    topic filter (the same prefix the PubSubTransport puts on the wire):
    BBB frames never arrive, AAA frames do.
    """
    import pynng as _pynng

    from face_tss.envelope import Envelope as _Env
    from face_tss.envelope import encode_envelope as _enc
    from face_tss.transport import TOPIC_SEPARATOR as _SEP
    from face_tss.types import now_ns as _now

    scratch = tcp_addr()
    raw_pub = _pynng.Pub0(listen=scratch)
    raw_sub = _pynng.Sub0(dial=scratch, block_on_dial=False)
    raw_sub.subscribe(b"AAA" + _SEP)
    time.sleep(0.4)
    try:
        raw_pub.send(
            b"BBB" + _SEP + _enc(_Env("BBB", 0, 0, 1, _now(), b"for-bbb"))
        )
        raw_sub.recv_timeout = 400
        with pytest.raises(_pynng.Timeout):
            raw_sub.recv()
        raw_pub.send(
            b"AAA" + _SEP + _enc(_Env("AAA", 0, 0, 1, _now(), b"for-aaa"))
        )
        raw_sub.recv_timeout = 2000
        frame = bytes(raw_sub.recv())
        assert frame.startswith(b"AAA" + _SEP)
        assert decode_payload(frame) == b"for-aaa"
    finally:
        raw_pub.close()
        raw_sub.close()


def decode_payload(frame: bytes) -> bytes:
    from face_tss.envelope import decode_envelope as _dec
    from face_tss.transport import TOPIC_SEPARATOR as _SEP

    topic_end = frame.index(_SEP) + len(_SEP)
    return _dec(frame[topic_end:]).payload


def test_bus_two_peers_exchange(tcp_addr):
    addr = tcp_addr()
    mk = lambda name: (
        TssConfigBuilder()
        .add("CMD", direction=Direction.BI_DIRECTIONAL,
             transport="bus", address=addr)
        .build()
    )
    a = FaceTss("a")
    b = FaceTss("b")
    a.initialize(mk("a"))
    b.initialize(mk("b"))
    a_id, _ = a.create_connection("cmd")
    # Second listen on the same address would clash; join as dialer.
    b_conn_cfg = b._config.lookup("cmd")
    dialer = BusTransport(b_conn_cfg)
    dialer.open_dial()
    import face_tss.tss as tss_mod
    b_holder = {}
    with b._lock:
        bid = next(b._ids)
        b._connections[bid] = tss_mod._Connection(config=b_conn_cfg, transport=dialer)
        b_holder["id"] = bid
    b_id = b_holder["id"]
    time.sleep(0.6)
    try:
        a.send_message(a_id, b"hello-from-a", 5_000_000_000, transaction_id=1)
        got, txn, _qos = b.receive_message(b_id, timeout_ns=5_000_000_000)
        assert got.payload == b"hello-from-a"
        assert txn == 1
    finally:
        a.finalize()
        b.finalize()


def test_oversize_send_rejected(tcp_addr):
    addr = tcp_addr()
    cfg = (
        TssConfigBuilder()
        .add("C", transport="pubsub", role="publisher", address=addr,
             max_message_size=8)
        .build()
    )
    tss = FaceTss()
    tss.initialize(cfg)
    cid, max_size = tss.create_connection("c")
    assert max_size == 8
    try:
        with pytest.raises(DataBufferTooSmallError):
            tss.send_message(cid, b"012345678")  # 9 bytes
        tss.send_message(cid, b"01234567")  # exactly max: fine
    finally:
        tss.finalize()


def test_callback_delivery(tcp_addr):
    pub, pub_id, sub, sub_id = _pubsub_pair(tcp_addr)
    try:
        got: list[tuple] = []
        done = threading.Event()

        def on_msg(connection_id, transaction_id, message_guid, payload,
                   header, qos, context) -> ReturnCode:
            got.append((connection_id, transaction_id, message_guid,
                        payload, header, qos, context))
            done.set()
            return ReturnCode.NO_ERROR

        assert sub.register_callback(sub_id, on_msg,
                                      context="ctx") == ReturnCode.NO_ERROR
        assert sub.register_callback(sub_id, on_msg) == ReturnCode.NO_ACTION
        time.sleep(0.2)
        pub.send_message(pub_id, b"via-callback", 5_000_000_000,
                         transaction_id=99, message_guid=1234)
        assert done.wait(timeout=5.0), "callback never fired"
        (cid, txn, guid, payload, header, qos, ctx), = got
        assert cid == sub_id
        assert txn == 99
        assert guid == 1234
        assert payload == b"via-callback"
        assert header.source_uid == pub.source_id
        assert header.instance_uid != 0
        assert len(qos) == 2
        assert qos[0].name == "message_age_ns"
        assert qos[1].name == "priority"
        assert qos[1].value == 0
        assert ctx == "ctx"
        assert sub.unregister_callback(sub_id) == ReturnCode.NO_ERROR
    finally:
        pub.finalize()
        sub.finalize()


def test_receive_into_caller_owned_buffer(tcp_addr):
    pub, pub_id, sub, sub_id = _pubsub_pair(tcp_addr)
    try:
        # Undersized buffer: required size reported in the error, message
        # discarded.
        pub.send_message(pub_id, b"hello", 5_000_000_000, transaction_id=21)
        with pytest.raises(DataBufferTooSmallError) as exc:
            sub.receive_into(sub_id, bytearray(3),
                             timeout_ns=5_000_000_000)
        assert "required size 5" in str(exc.value)

        # Exact fit: payload copied, metadata returned.
        pub.send_message(pub_id, b"hello", 5_000_000_000, transaction_id=22)
        buf = bytearray(64)
        payload_len, txn, header, guid, qos = sub.receive_into(
            sub_id, buf, timeout_ns=5_000_000_000)
        assert payload_len == 5
        assert bytes(buf[:5]) == b"hello"
        assert txn == 22
        assert header.source_uid == pub.source_id
        assert header.timestamp > 0
        assert len(qos) == 2
        assert qos[0].name == "message_age_ns"
        assert qos[1].name == "priority"
        assert qos[1].value == 0

        # Zero-length payload: empty buffer works.
        pub.send_message(pub_id, b"", 5_000_000_000, transaction_id=23)
        payload_len, txn, _, _, _ = sub.receive_into(
            sub_id, bytearray(0), timeout_ns=5_000_000_000)
        assert payload_len == 0
        assert txn == 23
    finally:
        pub.finalize()
        sub.finalize()


def test_subscriber_first_connects(tcp_addr):
    """Subscriber started before the publisher still connects (issue #5).

    The C dial was one-shot blocking (0/20 received); it is now
    non-blocking with background retry, matching the Python transport.
    Early messages may drop while the subscription propagates, so this
    asserts a large majority arrives.
    """
    import time as _time

    addr = tcp_addr()
    sub_cfg = (
        TssConfigBuilder()
        .add("POSITION", direction=Direction.BI_DIRECTIONAL,
             transport="pubsub", role="subscriber", address=addr)
        .build()
    )
    pub_cfg = (
        TssConfigBuilder()
        .add("POSITION", direction=Direction.BI_DIRECTIONAL,
             transport="pubsub", role="publisher", address=addr)
        .build()
    )
    sub = FaceTss("sub")
    sub.initialize(sub_cfg)
    sub_id, _ = sub.create_connection("POSITION")  # listener not up yet
    _time.sleep(0.3)
    pub = FaceTss("pub")
    pub.initialize(pub_cfg)
    pub_id, _ = pub.create_connection("position")
    _time.sleep(0.5)  # dial + subscription settle
    for i in range(20):
        pub.send_message(pub_id, b"m%02d" % i, timeout_ns=1_000_000_000,
                         transaction_id=100 + i)
        _time.sleep(0.05)
    got = 0
    for _ in range(22):
        try:
            sub.receive_message(sub_id, timeout_ns=500_000_000)
        except TimedOutError:
            break
        got += 1
    assert got >= 10
    pub.finalize()
    sub.finalize()


def test_pubsub_inproc_round_trip():
    """Same pub/sub round trip as TCP, but over nng inproc:// (in-process).

    No code changes were needed to support this: the transport layer passes
    the address straight through to nng, which ships an inproc transport.
    inproc names are process-global, so use a unique name per test.
    """
    addr = "inproc://tss-test-inproc-round-trip"
    pub_cfg = (
        TssConfigBuilder()
        .add("POSITION", direction=Direction.BI_DIRECTIONAL,
             transport="pubsub", role="publisher", address=addr)
        .build()
    )
    sub_cfg = (
        TssConfigBuilder()
        .add("POSITION", direction=Direction.BI_DIRECTIONAL,
             transport="pubsub", role="subscriber", address=addr)
        .build()
    )
    pub = FaceTss("pub")
    sub = FaceTss("sub")
    pub.initialize(pub_cfg)
    sub.initialize(sub_cfg)
    pub_id, _ = pub.create_connection("position")
    sub_id, _ = sub.create_connection("POSITION")
    try:
        time.sleep(0.4)  # dial + subscription settle
        report = PositionReport("N123", 37.5, -122.25, 1500.0, 270.0, True)
        used = pub.send_message(pub_id, report.serialize(), 5_000_000_000,
                                transaction_id=11)
        assert used == 11
        msg, txn, qos = sub.receive_message(sub_id, timeout_ns=5_000_000_000)
        assert PositionReport.deserialize(msg.payload) == report
        assert txn == 11
        assert msg.header.source_uid == pub.source_id
        assert msg.header.instance_uid != 0
        assert msg.header.timestamp > 0
        assert len(qos) == 2
        assert qos[0].name == "message_age_ns"
        assert qos[1].name == "priority"
        assert qos[1].value == 0
    finally:
        pub.finalize()
        sub.finalize()


def test_qos_staleness_enforcement(tcp_addr):
    """Staleness policy: stale messages raise MessageStaleError, fresh pass.

    Mirrors the C t_qos_staleness test: policy set/get validation, the
    MAX_AGE alias, MESSAGE_STALE on an aged message, stale_dropped stat,
    and normal delivery of a fresh message.
    """
    from face_tss import (
        ConnectionClosedError,
        InvalidParamError,
        MessageStaleError,
        QosPolicyKind,
    )

    pub, pub_id, sub, sub_id = _pubsub_pair(tcp_addr)
    try:
        # Policy API validation.
        with pytest.raises(ConnectionClosedError):
            sub.set_qos_policy(999, QosPolicyKind.STALENESS, 1000)
        with pytest.raises(InvalidParamError):
            sub.set_qos_policy(sub_id, QosPolicyKind.STALENESS, -1)
        assert sub.set_qos_policy(
            sub_id, QosPolicyKind.STALENESS, 100_000_000
        ) == ReturnCode.NO_ERROR
        assert sub.get_qos_policy(
            sub_id, QosPolicyKind.STALENESS) == 100_000_000
        assert sub.get_qos_policy(
            sub_id, QosPolicyKind.PRIORITY) is None
        # MAX_AGE is a documented alias for STALENESS.
        sub.set_qos_policy(sub_id, QosPolicyKind.MAX_AGE, 200_000_000)
        assert sub.get_qos_policy(
            sub_id, QosPolicyKind.STALENESS) == 200_000_000

        # Aged message -> MessageStaleError, counted in stats.
        pub.send_message(pub_id, b"old", 5_000_000_000, transaction_id=1)
        time.sleep(0.3)  # exceed the 200ms threshold
        with pytest.raises(MessageStaleError) as exc_info:
            sub.receive_message(sub_id, timeout_ns=5_000_000_000)
        assert exc_info.value.return_code == ReturnCode.MESSAGE_STALE
        assert sub.stats.stale_dropped == 1

        # Fresh message delivers normally.
        pub.send_message(pub_id, b"new", 5_000_000_000, transaction_id=2)
        msg, txn, qos = sub.receive_message(sub_id,
                                            timeout_ns=5_000_000_000)
        assert msg.payload == b"new"
        assert txn == 2
        assert len(qos) == 2
    finally:
        pub.finalize()
        sub.finalize()


def test_qos_staleness_callback_drop(tcp_addr):
    """Stale messages are not delivered to registered callbacks."""
    from face_tss import QosPolicyKind

    pub, pub_id, sub, sub_id = _pubsub_pair(tcp_addr)
    delivered = []
    try:
        sub.register_callback(
            sub_id,
            lambda cid, txn, guid, payload, header, qos, ctx:
                delivered.append(payload),
        )
        # Zero threshold: any message that took any time at all is stale,
        # so the background dispatch drops it deterministically.
        sub.set_qos_policy(sub_id, QosPolicyKind.STALENESS, 0)
        pub.send_message(pub_id, b"stale", 5_000_000_000, transaction_id=1)
        time.sleep(0.5)  # let the background dispatch run
        assert delivered == []
        assert sub.stats.stale_dropped >= 1
        # Generous threshold: fresh messages reach the callback.
        sub.set_qos_policy(sub_id, QosPolicyKind.STALENESS, 3600_000_000_000)
        pub.send_message(pub_id, b"fresh", 5_000_000_000, transaction_id=2)
        deadline = time.time() + 5
        while not delivered and time.time() < deadline:
            time.sleep(0.05)
        assert delivered == [b"fresh"]
    finally:
        pub.finalize()
        sub.finalize()


def test_qos_priority_threshold(tcp_addr):
    """QoS priority: stamped on send, threshold-filtered on receive.

    The sender's PRIORITY policy value travels on the wire (field 8) and
    is reported in the receiver's QoS event. A receiving connection with
    a PRIORITY threshold drops below-threshold messages; a blocking
    receive keeps waiting for a qualifying message and raises
    TimedOutError when the timeout expires.
    """
    from face_tss import QosPolicyKind

    pub, pub_id, sub, sub_id = _pubsub_pair(tcp_addr)
    try:
        # Priority stamping: reported in the receiver's QoS event.
        pub.set_qos_policy(pub_id, QosPolicyKind.PRIORITY, 5)
        pub.send_message(pub_id, b"p5", 5_000_000_000, transaction_id=1)
        msg, txn, qos = sub.receive_message(
            sub_id, timeout_ns=5_000_000_000
        )
        assert msg.payload == b"p5"
        assert qos[1].name == "priority"
        assert qos[1].value == 5

        # Threshold: below-threshold message is dropped; the receive
        # waits for a qualifying message instead of returning it.
        sub.set_qos_policy(sub_id, QosPolicyKind.PRIORITY, 5)
        pub.set_qos_policy(pub_id, QosPolicyKind.PRIORITY, 3)
        pub.send_message(pub_id, b"low", 5_000_000_000, transaction_id=2)
        with pytest.raises(TimedOutError):
            sub.receive_message(sub_id, timeout_ns=1_000_000_000)
        assert sub.stats.priority_dropped == 1

        # Above-threshold message still delivers with its priority.
        pub.set_qos_policy(pub_id, QosPolicyKind.PRIORITY, 7)
        pub.send_message(pub_id, b"high", 5_000_000_000, transaction_id=3)
        msg, txn, qos = sub.receive_message(
            sub_id, timeout_ns=5_000_000_000
        )
        assert msg.payload == b"high"
        assert qos[1].value == 7
    finally:
        pub.finalize()
        sub.finalize()


def test_qos_priority_callback_drop(tcp_addr):
    """QoS priority threshold in callback dispatch: dropped, not delivered."""
    from face_tss import QosPolicyKind

    pub, pub_id, sub, sub_id = _pubsub_pair(tcp_addr)
    delivered = []
    try:
        sub.set_qos_policy(sub_id, QosPolicyKind.PRIORITY, 5)
        pub.set_qos_policy(pub_id, QosPolicyKind.PRIORITY, 2)
        sub.register_callback(
            sub_id,
            lambda cid, txn, guid, payload, header, qos, ctx:
                delivered.append(payload),
        )
        pub.send_message(pub_id, b"cb-low", 5_000_000_000, transaction_id=1)
        time.sleep(0.5)  # let the background dispatch run
        assert delivered == []
        assert sub.stats.priority_dropped == 1
        # Above threshold reaches the callback.
        pub.set_qos_policy(pub_id, QosPolicyKind.PRIORITY, 9)
        pub.send_message(pub_id, b"cb-high", 5_000_000_000, transaction_id=2)
        deadline = time.time() + 5
        while not delivered and time.time() < deadline:
            time.sleep(0.05)
        assert delivered == [b"cb-high"]
    finally:
        pub.finalize()
        sub.finalize()


def test_qos_reliability_admission(tcp_addr):
    """QoS reliability: level validation and transport admission control.

    Only the two documented levels exist; RELIABLE is rejected with
    NOT_AVAILABLE on the best-effort nng transports (pub/sub, bus)
    instead of being silently pretended.
    """
    from face_tss import (
        FaceTssError,
        InvalidParamError,
        QOS_BEST_EFFORT,
        QOS_RELIABLE,
        QosPolicyKind,
    )

    pub, pub_id, sub, sub_id = _pubsub_pair(tcp_addr)
    try:
        with pytest.raises(InvalidParamError):
            sub.set_qos_policy(sub_id, QosPolicyKind.RELIABILITY, 99)
        with pytest.raises(FaceTssError) as exc_info:
            sub.set_qos_policy(
                sub_id, QosPolicyKind.RELIABILITY, QOS_RELIABLE
            )
        assert exc_info.value.return_code == ReturnCode.NOT_AVAILABLE
        assert sub.set_qos_policy(
            sub_id, QosPolicyKind.RELIABILITY, QOS_BEST_EFFORT
        ) == ReturnCode.NO_ERROR
        assert sub.get_qos_policy(
            sub_id, QosPolicyKind.RELIABILITY
        ) == QOS_BEST_EFFORT
    finally:
        pub.finalize()
        sub.finalize()


def test_qos_reliability_gap_detection(tcp_addr):
    """QoS reliability monitoring: sequence gaps observed on receive.

    Setting a RELIABILITY policy (either level) enables per-connection
    sequence tracking. A raw publisher injects envelopes that skip a
    sequence number; the gap is reported in the QoS event and counted in
    stats. The first message from a source re-baselines without counting
    a gap (a late subscriber legitimately misses earlier messages).
    """
    import pynng

    from face_tss import QOS_BEST_EFFORT, QosPolicyKind
    from face_tss.envelope import Envelope, encode_envelope

    addr = tcp_addr()
    sub = FaceTss("sub")
    sub_cfg = (
        TssConfigBuilder()
        .add("GAP", direction=Direction.BI_DIRECTIONAL,
             transport="pubsub", role="subscriber", address=addr)
        .build()
    )
    sub.initialize(sub_cfg)
    sub_id, _ = sub.create_connection("GAP")
    # Raw publisher on the address the TSS subscriber dials.
    raw_pub = pynng.Pub0(listen=addr)
    time.sleep(0.4)  # dial + subscription settle
    try:
        # Monitoring inactive: no sequence_gap element.
        topic = b"GAP\x00"
        now = time.time_ns()

        def raw_send(seq, payload):
            env = Envelope(
                connection_name="GAP",
                transaction_id=seq,
                source_id=4242,
                sequence_number=seq,
                timestamp_ns=now,
                payload=payload,
            )
            raw_pub.send(topic + encode_envelope(env))

        raw_send(1, b"one")
        # The subscriber's dial/subscription may still be settling (inherent
        # pub/sub early loss); retry until the first message gets through,
        # then drain any duplicate retries before enabling monitoring.
        deadline = time.time() + 10
        msg = None
        while msg is None and time.time() < deadline:
            raw_send(1, b"one")
            try:
                msg, txn, qos = sub.receive_message(
                    sub_id, timeout_ns=500_000_000
                )
            except TimedOutError:
                continue
        assert msg is not None and msg.payload == b"one"
        assert len(qos) == 2  # message_age_ns + priority only
        while True:
            try:
                dup, _, _ = sub.receive_message(
                    sub_id, timeout_ns=200_000_000
                )
                assert dup.payload == b"one"  # only retries in flight
            except TimedOutError:
                break

        # Monitoring active: first monitored message re-baselines
        # (the pre-monitoring message can't establish a baseline).
        sub.set_qos_policy(
            sub_id, QosPolicyKind.RELIABILITY, QOS_BEST_EFFORT
        )
        raw_send(2, b"two")
        msg, txn, qos = sub.receive_message(
            sub_id, timeout_ns=5_000_000_000
        )
        assert msg.payload == b"two"
        assert len(qos) == 3
        assert qos[2].name == "sequence_gap"
        assert qos[2].value == 0
        assert sub.stats.reliability_gaps == 0

        # A skipped sequence number is reported as a gap.
        raw_send(4, b"four")  # sequence 3 skipped
        msg, txn, qos = sub.receive_message(
            sub_id, timeout_ns=5_000_000_000
        )
        assert msg.payload == b"four"
        assert qos[2].name == "sequence_gap"
        assert qos[2].value == 1
        assert sub.stats.reliability_gaps == 1

        # Clean run after the gap: sequence_gap 0, no new gaps counted.
        raw_send(5, b"five")
        msg, txn, qos = sub.receive_message(
            sub_id, timeout_ns=5_000_000_000
        )
        assert qos[2].value == 0
        assert sub.stats.reliability_gaps == 1
    finally:
        raw_pub.close()
        sub.finalize()


def test_qos_reliability_no_phantom_gap_from_policy_drops(tcp_addr):
    """Policy-dropped messages are observed on the wire.

    A below-threshold (dropped) message followed by a delivered one must
    not report a phantom sequence gap: the drop was a receiver policy
    choice, not transport loss.
    """
    from face_tss import QOS_BEST_EFFORT, QosPolicyKind

    pub, pub_id, sub, sub_id = _pubsub_pair(tcp_addr)
    try:
        sub.set_qos_policy(
            sub_id, QosPolicyKind.RELIABILITY, QOS_BEST_EFFORT
        )
        sub.set_qos_policy(sub_id, QosPolicyKind.PRIORITY, 5)
        pub.set_qos_policy(pub_id, QosPolicyKind.PRIORITY, 3)
        pub.send_message(pub_id, b"dropped", 5_000_000_000, transaction_id=1)
        pub.set_qos_policy(pub_id, QosPolicyKind.PRIORITY, 7)
        pub.send_message(pub_id, b"kept", 5_000_000_000, transaction_id=2)
        msg, txn, qos = sub.receive_message(
            sub_id, timeout_ns=5_000_000_000
        )
        assert msg.payload == b"kept"
        assert qos[2].name == "sequence_gap"
        assert qos[2].value == 0
        assert sub.stats.priority_dropped == 1
        assert sub.stats.reliability_gaps == 0
    finally:
        pub.finalize()
        sub.finalize()
