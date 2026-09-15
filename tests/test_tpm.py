"""TPM tests (pure-Python mirror of the C tpm tests in c/tests/test_extended.c
and c/tests/test_threading.c)."""

import threading
import time

import pytest

from face_tss import (
    CHANNEL_ID_INVALID,
    TIMEOUT_INFINITE,
    TPM_MAX_CHANNELS,
    TPM_MAX_MSG,
    FaceTssError,
    InvalidModeError,
    InvalidParamError,
    NotInitializedError,
    ResourceLimitError,
    ReturnCode,
    TimedOutError,
    Tpm,
    TpmCallbackKind,
    TpmEventType,
    TpmStateType,
)


@pytest.fixture()
def tpm():
    t = Tpm.create("test")
    yield t
    t.destroy()


@pytest.fixture()
def open_tpm(tpm):
    assert tpm.initialize() == ReturnCode.NO_ERROR
    ch = tpm.open_channel("ep1")
    yield tpm, ch


def test_lifecycle():
    t = Tpm.create("test")
    assert t.initialize() == ReturnCode.NO_ERROR
    # Second initialize is a no-op returning NO_ACTION (mirrors C).
    assert t.initialize() == ReturnCode.NO_ACTION

    assert t.get_status() == TpmEventType.INIT_COMPLETE

    ch = t.open_channel("ep1")
    assert ch != CHANNEL_ID_INVALID
    assert t.close_channel(ch) == ReturnCode.NO_ERROR
    with pytest.raises(InvalidParamError):
        t.close_channel(99999)

    assert t.request_state_change(TpmStateType.PAUSE) == ReturnCode.NO_ERROR
    assert t.state == TpmStateType.PAUSE
    assert t.request_state_change(TpmStateType.NORMAL) == ReturnCode.NO_ERROR
    assert t.state == TpmStateType.NORMAL
    with pytest.raises(InvalidParamError):
        t.request_state_change(99)

    t.destroy()


def test_open_requires_initialize(tpm):
    with pytest.raises(NotInitializedError):
        tpm.open_channel("ep1")
    with pytest.raises(NotInitializedError):
        tpm.get_status()
    with pytest.raises(NotInitializedError):
        tpm.close_channel(1)


def test_data_loopback(open_tpm):
    t, ch = open_tpm
    payload = b"tpm-payload"

    t.write_to_transport(ch, payload, transaction_id=777)
    txn, msg = t.read_from_transport(ch, 0)
    assert msg == payload
    assert txn == 777

    # Read with no data -> TimedOutError.
    with pytest.raises(TimedOutError):
        t.read_from_transport(ch, 0)


def test_is_data_available_poll(open_tpm):
    t, ch = open_tpm
    # Poll with no data: immediate, empty.
    t0 = time.monotonic_ns()
    assert t.is_data_available([ch], 0) == []
    assert time.monotonic_ns() - t0 < 100_000_000

    # After a write the channel shows up.
    t.write_to_transport(ch, b"x", transaction_id=1)
    assert t.is_data_available([ch], 0) == [ch]
    # Reading consumes it.
    t.read_from_transport(ch, 0)
    assert t.is_data_available([ch], 0) == []


def test_is_data_available_blocking_timeout(open_tpm):
    t, ch = open_tpm
    # 200 ms timeout with no data: blocks ~200 ms, returns empty.
    t0 = time.monotonic_ns()
    assert t.is_data_available([ch], 200_000_000) == []
    elapsed_ms = (time.monotonic_ns() - t0) / 1_000_000
    assert elapsed_ms >= 150  # blocked rather than polling
    assert elapsed_ms < 5000


def test_is_data_available_wakes_on_write(open_tpm):
    t, ch = open_tpm

    def delayed_write():
        time.sleep(0.1)
        t.write_to_transport(ch, b"wakeup", transaction_id=4242)

    w = threading.Thread(target=delayed_write)
    w.start()
    t0 = time.monotonic_ns()
    avail = t.is_data_available([ch], 5_000_000_000)
    elapsed_ms = (time.monotonic_ns() - t0) / 1_000_000
    w.join()
    assert avail == [ch]
    assert elapsed_ms < 4000  # woke on data, not on timeout

    # The writer's message is pending; read it.
    txn, msg = t.read_from_transport(ch, 0)
    assert txn == 4242
    assert msg == b"wakeup"


def test_is_data_available_infinite_with_pending(open_tpm):
    t, ch = open_tpm
    t.write_to_transport(ch, b"x", transaction_id=7)
    t0 = time.monotonic_ns()
    assert t.is_data_available([ch], TIMEOUT_INFINITE) == [ch]
    assert time.monotonic_ns() - t0 < 100_000_000


def test_is_data_available_multiple_channels(open_tpm):
    t, ch1 = open_tpm
    ch2 = t.open_channel("ep2")
    ch3 = t.open_channel("ep3")
    t.write_to_transport(ch2, b"two")
    t.write_to_transport(ch3, b"three")
    # Order of the input list is preserved.
    assert t.is_data_available([ch1, ch2, ch3], 0) == [ch2, ch3]
    assert t.is_data_available([ch3, ch2], 0) == [ch3, ch2]


def test_read_timeouts(open_tpm):
    t, ch = open_tpm
    # Poll with no data: TIMED_OUT at once.
    t0 = time.monotonic_ns()
    with pytest.raises(TimedOutError):
        t.read_from_transport(ch, 0)
    assert time.monotonic_ns() - t0 < 100_000_000

    # 200 ms timeout with no data: TIMED_OUT after ~200 ms.
    t0 = time.monotonic_ns()
    with pytest.raises(TimedOutError):
        t.read_from_transport(ch, 200_000_000)
    elapsed_ms = (time.monotonic_ns() - t0) / 1_000_000
    assert elapsed_ms >= 150
    assert elapsed_ms < 5000


def test_read_wakes_on_delayed_write(open_tpm):
    t, ch = open_tpm

    def delayed_write():
        time.sleep(0.1)
        t.write_to_transport(ch, b"wakeup", transaction_id=4242)

    w = threading.Thread(target=delayed_write)
    w.start()
    t0 = time.monotonic_ns()
    txn, msg = t.read_from_transport(ch, 5_000_000_000)
    elapsed_ms = (time.monotonic_ns() - t0) / 1_000_000
    w.join()
    assert msg == b"wakeup"
    assert txn == 4242
    assert elapsed_ms < 4000


def test_data_callback(open_tpm):
    t, ch = open_tpm
    calls = []

    def on_data(channel_id, transaction_id, message, user):
        calls.append((channel_id, transaction_id, message, user))

    assert t.register_callback(
        ch, TpmCallbackKind.DATA, data_cb=on_data, user="u1"
    ) == ReturnCode.NO_ERROR
    # Double register -> NO_ACTION.
    assert t.register_callback(ch, TpmCallbackKind.DATA, data_cb=on_data) == (
        ReturnCode.NO_ACTION
    )

    t.write_to_transport(ch, b"cb-payload", transaction_id=555)
    assert calls == [(ch, 555, b"cb-payload", "u1")]

    # Unregister stops delivery.
    assert t.unregister_callback(ch) == ReturnCode.NO_ERROR
    # Double unregister -> NO_ACTION.
    assert t.unregister_callback(ch) == ReturnCode.NO_ACTION
    t.write_to_transport(ch, b"no-cb", transaction_id=556)
    assert len(calls) == 1


def test_data_callback_both_kind(open_tpm):
    t, ch = open_tpm
    calls = []
    assert t.register_callback(
        ch, TpmCallbackKind.BOTH, data_cb=lambda c, x, m, u: calls.append(m)
    ) == ReturnCode.NO_ERROR
    t.write_to_transport(ch, b"m")
    assert calls == [b"m"]


def test_callback_invalid_channel(tpm):
    tpm.initialize()
    with pytest.raises(InvalidParamError):
        tpm.register_callback(999, TpmCallbackKind.DATA, data_cb=lambda *a: None)
    with pytest.raises(InvalidParamError):
        tpm.unregister_callback(999)
    with pytest.raises(InvalidParamError):
        tpm.register_callback(1, 99, data_cb=lambda *a: None)


def test_write_invalid_channel(tpm):
    tpm.initialize()
    with pytest.raises(InvalidParamError):
        tpm.write_to_transport(999, b"x")
    with pytest.raises(InvalidParamError):
        tpm.read_from_transport(999, 0)


def test_write_overflow(open_tpm):
    t, ch = open_tpm
    with pytest.raises(FaceTssError) as exc:
        t.write_to_transport(ch, b"x" * (TPM_MAX_MSG + 1))
    assert exc.value.return_code == ReturnCode.DATA_OVERFLOW


def test_resource_limit(tpm):
    tpm.initialize()
    channels = [tpm.open_channel(f"ep{i}") for i in range(TPM_MAX_CHANNELS)]
    assert len(set(channels)) == TPM_MAX_CHANNELS
    with pytest.raises(ResourceLimitError):
        tpm.open_channel("one-too-many")
    # Closing one frees a slot.
    tpm.close_channel(channels[0])
    assert tpm.open_channel("replacement") != CHANNEL_ID_INVALID


def test_shutdown_state_blocks_open(tpm):
    tpm.initialize()
    assert tpm.request_state_change(TpmStateType.SHUTDOWN) == ReturnCode.NO_ERROR
    with pytest.raises(InvalidModeError):
        tpm.open_channel("ep1")


def test_close_channel_wakes_blocked_reader(open_tpm):
    t, ch = open_tpm
    outcome = {}

    def blocked_read():
        try:
            t.read_from_transport(ch, TIMEOUT_INFINITE)
            outcome["rc"] = "unexpected-success"
        except FaceTssError as e:
            outcome["rc"] = e.return_code

    r = threading.Thread(target=blocked_read)
    r.start()
    time.sleep(0.2)  # let the reader block
    t.close_channel(ch)
    r.join(timeout=5)
    assert not r.is_alive()
    # Channel is gone after close -> INVALID_PARAM (mirrors C find_channel).
    assert outcome["rc"] == ReturnCode.INVALID_PARAM


def test_destroy_wakes_blocked_reader():
    t = Tpm.create("tpm-thr")
    t.initialize()
    ch = t.open_channel("ep1")
    outcome = {}

    def blocked_read():
        try:
            t.read_from_transport(ch, TIMEOUT_INFINITE)
            outcome["rc"] = "unexpected-success"
        except FaceTssError as e:
            outcome["rc"] = e.return_code

    r = threading.Thread(target=blocked_read)
    r.start()
    time.sleep(0.2)  # let the reader block

    # Destroy must wake the blocked reader; the reader returns
    # NOT_AVAILABLE without touching the destroyed TPM.
    t.destroy()
    r.join(timeout=5)
    assert not r.is_alive()
    assert outcome["rc"] == ReturnCode.NOT_AVAILABLE


def test_destroy_wakes_blocked_is_data_available():
    t = Tpm.create("tpm-thr2")
    t.initialize()
    ch = t.open_channel("ep1")
    outcome = {}

    def blocked_wait():
        try:
            t.is_data_available([ch], TIMEOUT_INFINITE)
            outcome["rc"] = "unexpected-success"
        except FaceTssError as e:
            outcome["rc"] = e.return_code

    r = threading.Thread(target=blocked_wait)
    r.start()
    time.sleep(0.2)
    t.destroy()
    r.join(timeout=5)
    assert not r.is_alive()
    assert outcome["rc"] == ReturnCode.NOT_AVAILABLE


def test_context_manager():
    with Tpm.create("ctx") as t:
        t.initialize()
        ch = t.open_channel("ep1")
        t.write_to_transport(ch, b"ping", transaction_id=1)
        txn, msg = t.read_from_transport(ch, 0)
        assert (txn, msg) == (1, b"ping")
    # After the with block the TPM is destroyed.
    with pytest.raises(FaceTssError) as exc:
        t.get_status()
    assert exc.value.return_code == ReturnCode.NOT_AVAILABLE


def test_empty_message_round_trip(open_tpm):
    t, ch = open_tpm
    t.write_to_transport(ch, b"", transaction_id=9)
    txn, msg = t.read_from_transport(ch, 0)
    assert (txn, msg) == (9, b"")
