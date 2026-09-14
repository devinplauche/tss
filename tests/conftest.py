"""Shared pytest fixtures: sys.path wiring + free TCP ports."""

from __future__ import annotations

import itertools
import socket
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

_port_counter = itertools.count(5700)


def free_port() -> int:
    """Find a currently-free loopback port (bind probe, then release)."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture()
def tcp_addr():
    """Factory fixture: returns a fresh tcp://127.0.0.1:PORT each call."""
    def _make() -> str:
        return f"tcp://127.0.0.1:{free_port()}"
    return _make
