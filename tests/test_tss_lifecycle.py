"""TSS lifecycle tests (no network beyond loopback binds)."""

import pytest

from face_tss import (
    ConnectionClosedError,
    Direction,
    FaceTss,
    InvalidParamError,
    NotInitializedError,
    ReturnCode,
    TssConfig,
    TssConfigBuilder,
)


def _cfg(**kw):
    kw.setdefault("address", "tcp://127.0.0.1:1")  # never actually bound here
    return TssConfigBuilder().add("C", **kw).build()


def test_send_before_init_raises():
    tss = FaceTss()
    with pytest.raises(NotInitializedError):
        tss.create_connection("C")
    with pytest.raises(NotInitializedError):
        tss.send_message(1, b"x")


def test_initialize_idempotent():
    tss = FaceTss()
    assert tss.initialize(_cfg()) == ReturnCode.NO_ERROR
    assert tss.initialize(_cfg()) == ReturnCode.NO_ACTION


def test_create_unknown_connection():
    tss = FaceTss()
    tss.initialize(TssConfig())
    with pytest.raises(InvalidParamError):
        tss.create_connection("NOPE")


def test_direction_enforcement(tcp_addr):
    addr = tcp_addr()
    # SOURCE-only: subscriber side would need a publisher; use bus instead so
    # a single connection can be created without a peer.
    cfg = (
        TssConfigBuilder()
        .add("SRC", direction=Direction.SOURCE, transport="bus", address=addr)
        .build()
    )
    tss = FaceTss()
    tss.initialize(cfg)
    cid, _ = tss.create_connection("src")
    with pytest.raises(Exception):  # InvalidModeError on receive
        tss.receive_message(cid, timeout_ns=0)
    tss.destroy_connection(cid)
    tss.finalize()


def test_destroy_unknown_id_is_no_action_and_zero_is_invalid():
    tss = FaceTss()
    tss.initialize(TssConfig())
    assert tss.destroy_connection(999) == ReturnCode.NO_ACTION
    with pytest.raises(InvalidParamError):
        tss.destroy_connection(0)


def test_use_after_destroy_raises_closed(tcp_addr):
    cfg = TssConfigBuilder().add(
        "C", transport="bus", address=tcp_addr()).build()
    tss = FaceTss()
    tss.initialize(cfg)
    cid, _ = tss.create_connection("c")
    assert tss.destroy_connection(cid) == ReturnCode.NO_ERROR
    with pytest.raises(ConnectionClosedError):
        tss.send_message(cid, b"x")


def test_unregister_without_callback_is_no_action(tcp_addr):
    cfg = TssConfigBuilder().add(
        "C", direction="DESTINATION", transport="pubsub",
        role="subscriber", address=tcp_addr()).build()
    tss = FaceTss()
    tss.initialize(cfg)
    cid, _ = tss.create_connection("c")
    assert tss.unregister_callback(cid) == ReturnCode.NO_ACTION
    tss.destroy_connection(cid)
    tss.finalize()
