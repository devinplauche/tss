"""Typed message tests: PositionReport struct-codec round-trips."""

import pytest

from face_tss import PositionReport


def test_round_trip():
    r = PositionReport("N123AB", 37.7749, -122.4194, 16.5, 180.25, True)
    assert PositionReport.deserialize(r.serialize()) == r


def test_invalid_flag_and_unicode_id():
    r = PositionReport("TÉST-1", -33.86, 151.2, 0.0, 0.0, False)
    back = PositionReport.deserialize(r.serialize())
    assert back == r
    assert back.valid is False


@pytest.mark.parametrize("bad", [b"", b"\x00" * 4, b"POS1garbage"])
def test_rejects_garbage(bad):
    with pytest.raises(ValueError):
        PositionReport.deserialize(bad)


def test_rejects_truncated():
    r = PositionReport("ABC", 1.0, 2.0, 3.0, 4.0, True).serialize()
    with pytest.raises(ValueError):
        PositionReport.deserialize(r[:-1])
