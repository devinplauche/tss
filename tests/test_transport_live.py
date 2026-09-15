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
        assert len(qos) == 1
        assert qos[0].name == "message_age_ns"

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
        assert len(qos) == 1
        assert qos[0].name == "message_age_ns"
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
        assert len(qos) == 1
        assert qos[0].name == "message_age_ns"

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
        assert len(qos) == 1
        assert qos[0].name == "message_age_ns"
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
        assert len(qos) == 1
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
