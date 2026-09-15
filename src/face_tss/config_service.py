"""FACE Configuration service (FACE 3.2 IDL, FACE/Configuration.idl).

Pure-Python mirror of ``face_tss/config_service.h``: Initialize / Open /
Get_Size / Read / Seek / Close over named configuration containers, with
``memory:`` and ``file:`` backends. ``write()`` is an implementation
extension for populating memory containers (the IDL has no write op).

Errors are raised as :mod:`face_tss.errors` exceptions:

* ``InvalidParamError``  - null/invalid arguments (C: INVALID_PARAM)
* ``InvalidConfigError`` - bad handle, unknown container/set (C: INVALID_CONFIG)
* ``FaceTssError(ReturnCode.NOT_AVAILABLE)`` - read past end of a set
  (C: NOT_AVAILABLE)

Threading: one RLock guards the session table; it is never held across
blocking I/O (file reads snapshot the path/position first).
"""

from __future__ import annotations

import enum
import os
import threading
from typing import Dict, Optional

from .errors import FaceTssError, InvalidConfigError, InvalidParamError
from .types import ReturnCode


class SeekWhence(enum.IntEnum):
    """FACE::Configuration::WHENCE_TYPE, in IDL order."""

    SEEK_FROM_START = 0
    SEEK_FROM_CURRENT = 1
    SEEK_FROM_END = 2


class _Session:
    __slots__ = ("handle", "kind", "sets", "dir", "pos", "last_set")

    def __init__(self, handle: int, kind: str, directory: Optional[str] = None):
        self.handle = handle
        self.kind = kind  # "memory" or "file"
        self.sets: Dict[str, bytes] = {}
        self.dir = directory
        self.pos = 0
        self.last_set: Optional[str] = None


def _not_available(message: str) -> FaceTssError:
    return FaceTssError(ReturnCode.NOT_AVAILABLE, message)


class ConfigService:
    """FACE::Configuration service implementation."""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._sessions: Dict[int, _Session] = {}
        self._next_handle = 1

    # -- lifecycle ----------------------------------------------------
    def initialize(self, initialization_information: str) -> None:
        """FACE::Configuration::Initialize (the string is accepted as-is)."""
        if not isinstance(initialization_information, str):
            raise InvalidParamError(
                "initialization_information must be a string")

    def open(self, container_name: str) -> int:
        """FACE::Configuration::Open; returns the session handle."""
        if not isinstance(container_name, str) or not container_name:
            raise InvalidParamError("container_name must be a non-empty string")
        if container_name.startswith("memory:"):
            kind, directory = "memory", None
        elif container_name.startswith("file:"):
            directory = container_name[len("file:"):]
            if not directory or not os.path.isdir(directory):
                raise InvalidConfigError(
                    f"not a directory: {directory!r}")
            kind = "file"
        else:
            raise InvalidConfigError(
                f"unknown container backend: {container_name!r}")
        with self._lock:
            handle = self._next_handle
            self._next_handle += 1
            self._sessions[handle] = _Session(handle, kind, directory)
            return handle

    def close(self, handle: int) -> None:
        """FACE::Configuration::Close."""
        with self._lock:
            try:
                del self._sessions[handle]
            except KeyError:
                raise InvalidConfigError(f"bad handle: {handle!r}") from None

    # -- helpers ------------------------------------------------------
    def _session(self, handle: int) -> _Session:
        try:
            return self._sessions[handle]
        except KeyError:
            raise InvalidConfigError(f"bad handle: {handle!r}") from None

    @staticmethod
    def _check_set_name(set_name: str, for_file: bool) -> None:
        if not isinstance(set_name, str) or not set_name:
            raise InvalidParamError("set_name must be a non-empty string")
        if for_file and (
            "/" in set_name or set_name in (".", "..")
        ):
            raise InvalidParamError(f"invalid set name: {set_name!r}")

    def _size_of(self, session: _Session, set_name: str) -> int:
        if session.kind == "memory":
            try:
                return len(session.sets[set_name])
            except KeyError:
                raise InvalidConfigError(
                    f"no such set: {set_name!r}") from None
        path = os.path.join(session.dir, set_name)
        if not os.path.isfile(path):
            raise InvalidConfigError(f"no such set: {set_name!r}")
        return os.path.getsize(path)

    # -- data access --------------------------------------------------
    def get_size(self, handle: int, set_name: str) -> int:
        """FACE::Configuration::Get_Size."""
        with self._lock:
            session = self._session(handle)
            self._check_set_name(set_name, session.kind == "file")
            size = self._size_of(session, set_name)
            session.last_set = set_name
            return size

    def read(self, handle: int, set_name: str, buffer_size: int) -> bytes:
        """FACE::Configuration::Read; returns up to ``buffer_size`` bytes.

        Raises ``FaceTssError(NOT_AVAILABLE)`` when the session position
        is at or past the end of the set.
        """
        if not isinstance(buffer_size, int) or buffer_size < 0:
            raise InvalidParamError("buffer_size must be a non-negative int")
        with self._lock:
            session = self._session(handle)
            self._check_set_name(set_name, session.kind == "file")
            size = self._size_of(session, set_name)
            session.last_set = set_name
            if session.pos >= size:
                raise _not_available(
                    f"end of set {set_name!r} already reached")
            want = min(buffer_size, size - session.pos)
            if session.kind == "memory":
                data = session.sets[set_name][session.pos:session.pos + want]
            else:
                path = os.path.join(session.dir, set_name)
                with open(path, "rb") as f:
                    f.seek(session.pos)
                    data = f.read(want)
            session.pos += len(data)
            return data

    def seek(self, handle: int, whence: SeekWhence, offset: int) -> None:
        """FACE::Configuration::Seek."""
        if not isinstance(offset, int):
            raise InvalidParamError("offset must be an int")
        try:
            whence = SeekWhence(whence)
        except ValueError:
            raise InvalidParamError(f"invalid whence: {whence!r}") from None
        with self._lock:
            session = self._session(handle)
            if whence is SeekWhence.SEEK_FROM_START:
                if offset < 0:
                    raise InvalidParamError(
                        "SEEK_FROM_START requires offset >= 0")
                newpos = offset
            elif whence is SeekWhence.SEEK_FROM_CURRENT:
                newpos = session.pos + offset
                if newpos < 0:
                    raise InvalidParamError(
                        "seek would place position before the start")
            else:  # SEEK_FROM_END
                if offset > 0:
                    raise InvalidParamError(
                        "SEEK_FROM_END requires offset <= 0")
                if session.last_set is None:
                    raise InvalidConfigError(
                        "SEEK_FROM_END needs a set selected by read/get_size")
                size = self._size_of(session, session.last_set)
                newpos = size + offset
                if newpos < 0:
                    raise InvalidParamError(
                        "seek would place position before the start")
            session.pos = newpos

    # -- implementation extension --------------------------------------
    def write(self, handle: int, set_name: str, data: bytes) -> None:
        """Store ``data`` as ``set_name`` in a ``memory:`` container.

        Implementation extension (the IDL has no write operation).
        """
        if not isinstance(data, (bytes, bytearray)):
            raise InvalidParamError("data must be bytes")
        with self._lock:
            session = self._session(handle)
            self._check_set_name(set_name, for_file=False)
            if session.kind != "memory":
                raise InvalidConfigError(
                    "write() is only valid on memory: containers")
            session.sets[set_name] = bytes(data)


__all__ = [
    "ConfigService",
    "SeekWhence",
]
