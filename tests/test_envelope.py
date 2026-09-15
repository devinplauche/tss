"""Envelope codec tests: round-trip incl. binary payloads, empty payloads,
defaults, unicode names, corrupt-input rejection."""

import pytest

from face_tss.envelope import Envelope, decode_envelope, encode_envelope


def _env(**kw):
    base = dict(
        connection_name="HELLO",
        transaction_id=7,
        source_id=123456789,
        sequence_number=42,
        timestamp_ns=1720000000000000000,
        payload=bytes([1, 2, 3, 250, 251, 0, 255]),
    )
    base.update(kw)
    return Envelope(**base)


def test_round_trip_binary_payload():
    env = _env()
    back = decode_envelope(encode_envelope(env))
    assert back == env


def test_round_trip_empty_payload_and_defaults():
    env = _env(connection_name="X", transaction_id=0, source_id=0,
               sequence_number=0, timestamp_ns=0, payload=b"")
    back = decode_envelope(encode_envelope(env))
    assert back == env
    assert back.payload == b""


def test_round_trip_large_payload():
    blob = bytes(range(256)) * 64  # 16 KiB
    back = decode_envelope(encode_envelope(_env(payload=blob)))
    assert back.payload == blob


def test_round_trip_unicode_connection():
    back = decode_envelope(encode_envelope(_env(connection_name="TÉLÉMÉTRIE")))
    assert back.connection_name == "TÉLÉMÉTRIE"


@pytest.mark.parametrize("bad", [b"", b"\x01\x02", b"\xff" * 64])
def test_corrupt_inputs_rejected(bad):
    with pytest.raises(ValueError):
        decode_envelope(bad)


def test_accepts_bytearray_and_memoryview():
    raw = encode_envelope(_env())
    assert decode_envelope(bytearray(raw)).payload == _env().payload
    assert decode_envelope(memoryview(raw)).payload == _env().payload


def test_round_trip_priority():
    """Priority round-trips on the wire (field 8, signed 64-bit)."""
    env = _env(priority=7)
    back = decode_envelope(encode_envelope(env))
    assert back == env
    assert back.priority == 7
    env = _env(priority=-1)
    back = decode_envelope(encode_envelope(env))
    assert back.priority == -1


def test_absent_priority_decodes_as_zero():
    """Envelopes encoded without field 8 decode priority as 0."""
    env = _env()
    assert env.priority == 0
    assert decode_envelope(encode_envelope(env)).priority == 0
