"""Tests for UDP-broadcast peer discovery (face_tss.discovery).

Sandbox note: UDP broadcast and sendto() are blocked here, so tests
point the destination at 127.0.0.1. Same port-sharing caveat as the C
tests: the last-bound listener socket in a process receives each
datagram, so the announcer is always created before the listener.
"""

import time

import pytest

from face_tss import Discovery
from face_tss.discovery import DISCOVERY_PORT
from face_tss.errors import FaceTssError, InvalidParamError, TimedOutError
from face_tss.types import ReturnCode


def make_loopback() -> Discovery:
    d = Discovery.create()
    assert d.set_destination("127.0.0.1") == ReturnCode.NO_ERROR
    return d


def test_announce_lookup():
    announcer = make_loopback()
    listener = make_loopback()
    try:
        assert (
            announcer.announce("peer1", "tcp://127.0.0.1:5001", 100)
            == ReturnCode.NO_ERROR
        )
        assert listener.lookup("peer1", 5000) == "tcp://127.0.0.1:5001"
    finally:
        announcer.destroy()
        listener.destroy()


def test_poll_miss():
    d = make_loopback()
    try:
        with pytest.raises(TimedOutError):
            d.lookup("nobody", 0)
        t0 = time.monotonic()
        with pytest.raises(TimedOutError):
            d.lookup("nobody", 200)
        assert time.monotonic() - t0 >= 0.15
    finally:
        d.destroy()


def test_list_shows_peer():
    announcer = make_loopback()
    listener = make_loopback()
    try:
        announcer.announce("peer2", "tcp://127.0.0.1:5002", 100)
        assert listener.lookup("peer2", 5000) == "tcp://127.0.0.1:5002"
        peers = dict(listener.list())
        assert peers.get("peer2") == "tcp://127.0.0.1:5002"
    finally:
        announcer.destroy()
        listener.destroy()


def test_stop_expires():
    announcer = make_loopback()
    listener = make_loopback()
    try:
        announcer.announce("peer3", "tcp://127.0.0.1:5003", 100)
        assert listener.lookup("peer3", 5000) == "tcp://127.0.0.1:5003"
        assert announcer.stop_announce() == ReturnCode.NO_ERROR
        time.sleep(0.8)  # past 3x100ms expiry
        with pytest.raises(TimedOutError):
            listener.lookup("peer3", 0)
        assert "peer3" not in dict(listener.list())
    finally:
        announcer.destroy()
        listener.destroy()


def test_invalid_params():
    d = make_loopback()
    try:
        with pytest.raises(InvalidParamError):
            d.announce("", "a", 100)
        with pytest.raises(InvalidParamError):
            d.announce("n", "", 100)
        with pytest.raises(InvalidParamError):
            d.announce("n", "a", 99)
        with pytest.raises(InvalidParamError):
            d.announce("n", "a", 60001)
        with pytest.raises(InvalidParamError):
            d.announce("x" * 64, "a", 100)
        assert d.stop_announce() == ReturnCode.NO_ACTION
        with pytest.raises(InvalidParamError):
            d.lookup("", 0)
        with pytest.raises(InvalidParamError):
            d.lookup("n", -1)
        with pytest.raises(InvalidParamError):
            d.set_destination("")
        with pytest.raises(InvalidParamError):
            d.set_destination("not-an-ip")
        assert d.list() == []
    finally:
        d.destroy()


def test_double_announce():
    d = make_loopback()
    try:
        assert d.announce("me", "tcp://127.0.0.1:5004", 100) == ReturnCode.NO_ERROR
        assert d.announce("me", "tcp://127.0.0.1:5004", 200) == ReturnCode.NO_ACTION
        with pytest.raises(FaceTssError) as ei:
            d.announce("me", "tcp://127.0.0.1:5005", 100)
        assert ei.value.return_code == ReturnCode.NOT_AVAILABLE
        with pytest.raises(FaceTssError) as ei:
            d.announce("other", "tcp://127.0.0.1:5004", 100)
        assert ei.value.return_code == ReturnCode.NOT_AVAILABLE
        with pytest.raises(InvalidParamError):
            d.set_destination("127.0.0.1")
        assert d.stop_announce() == ReturnCode.NO_ERROR
        assert d.stop_announce() == ReturnCode.NO_ACTION
        assert d.announce("me2", "tcp://127.0.0.1:5006", 100) == ReturnCode.NO_ERROR
        assert d.stop_announce() == ReturnCode.NO_ERROR
    finally:
        d.destroy()


def test_context_manager():
    with make_loopback() as d:
        assert d.list() == []
    # destroy is idempotent
    d.destroy()


def test_wire_constants():
    assert DISCOVERY_PORT == 51970
