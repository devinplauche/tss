"""Tests for the FACE Configuration service mirror (face_tss.config_service)."""

import os

import pytest

from face_tss import (
    FaceTssError,
    InvalidConfigError,
    InvalidParamError,
    ReturnCode,
)
from face_tss.config_service import ConfigService, SeekWhence


def test_memory_round_trip():
    svc = ConfigService()
    svc.initialize("test")
    h = svc.open("memory:test")
    payload = b"memory-container-payload"
    svc.write(h, "greeting", payload)
    assert svc.get_size(h, "greeting") == len(payload)
    assert svc.read(h, "greeting", 1024) == payload
    # past end -> NOT_AVAILABLE
    with pytest.raises(FaceTssError) as exc:
        svc.read(h, "greeting", 1024)
    assert exc.value.return_code == ReturnCode.NOT_AVAILABLE
    # unknown set -> INVALID_CONFIG
    with pytest.raises(InvalidConfigError):
        svc.get_size(h, "nope")
    svc.close(h)
    with pytest.raises(InvalidConfigError):
        svc.get_size(h, "greeting")
    with pytest.raises(InvalidConfigError):
        svc.close(h)


def test_file_backend_seek(tmp_path):
    blob = tmp_path / "blob.bin"
    content = b"0123456789abcdef"
    blob.write_bytes(content)
    svc = ConfigService()
    h = svc.open(f"file:{tmp_path}")
    assert svc.get_size(h, "blob.bin") == len(content)
    assert svc.read(h, "blob.bin", 4) == b"0123"
    svc.seek(h, SeekWhence.SEEK_FROM_START, 0)
    assert svc.read(h, "blob.bin", 1024) == content
    svc.seek(h, SeekWhence.SEEK_FROM_END, -4)
    assert svc.read(h, "blob.bin", 4) == b"cdef"
    svc.seek(h, SeekWhence.SEEK_FROM_CURRENT, -8)
    assert svc.read(h, "blob.bin", 4) == b"89ab"
    # write() on a file session -> INVALID_CONFIG
    with pytest.raises(InvalidConfigError):
        svc.write(h, "x", b"y")
    svc.close(h)


def test_errors():
    svc = ConfigService()
    with pytest.raises(InvalidParamError):
        svc.initialize(None)
    with pytest.raises(InvalidParamError):
        svc.open("")
    with pytest.raises(InvalidConfigError):
        svc.open("nfs:/x")
    with pytest.raises(InvalidConfigError):
        svc.open("file:/no/such/dir")
    h = svc.open("memory:e")
    bad = 123456789
    with pytest.raises(InvalidConfigError):
        svc.get_size(bad, "s")
    with pytest.raises(InvalidConfigError):
        svc.read(bad, "s", 4)
    with pytest.raises(InvalidConfigError):
        svc.seek(bad, SeekWhence.SEEK_FROM_START, 0)
    with pytest.raises(InvalidConfigError):
        svc.close(bad)
    with pytest.raises(InvalidConfigError):
        svc.write(bad, "s", b"d")
    with pytest.raises(InvalidParamError):
        svc.get_size(h, "")
    with pytest.raises(InvalidParamError):
        svc.read(h, "s", -1)
    with pytest.raises(InvalidParamError):
        svc.seek(h, 99, 0)
    with pytest.raises(InvalidParamError):
        svc.seek(h, SeekWhence.SEEK_FROM_START, -1)
    with pytest.raises(InvalidParamError):
        svc.seek(h, SeekWhence.SEEK_FROM_END, 1)
    with pytest.raises(InvalidParamError):
        svc.write(h, "s", "not-bytes")
    # SEEK_FROM_END with no set selected -> INVALID_CONFIG
    with pytest.raises(InvalidConfigError):
        svc.seek(h, SeekWhence.SEEK_FROM_END, -1)
    svc.close(h)


def test_read_partial_and_zero_length():
    svc = ConfigService()
    h = svc.open("memory:p")
    svc.write(h, "s", b"abcdef")
    assert svc.read(h, "s", 2) == b"ab"
    assert svc.read(h, "s", 2) == b"cd"
    assert svc.read(h, "s", 0) == b""
    svc.seek(h, SeekWhence.SEEK_FROM_START, 0)
    assert svc.read(h, "s", 100) == b"abcdef"
    svc.close(h)
