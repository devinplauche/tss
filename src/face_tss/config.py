"""FACE TSS connection configuration.

A configuration names every connection the TSS instance may create and
describes how each one moves data:

* ``direction`` - SOURCE / DESTINATION / BI_DIRECTIONAL (FACE semantics).
* ``transport`` - which nng pattern carries it (``pubsub`` fan-out or
  ``bus`` mesh). Both run over the ``address`` URL (e.g.
  ``tcp://127.0.0.1:5555``).
* ``role`` - which end of the socket this instance owns. For ``pubsub`` one
  side must be ``publisher`` and the other ``subscriber``; for ``bus`` both
  sides use ``bus``.
* ``max_message_size`` - largest typed payload accepted (FACE
  MAX_MESSAGE_SIZE semantics); larger sends are rejected with
  BUFFER_TOO_SMALL.

Configuration is plain data (JSON/TOML/dict) so it can be generated from a
FACE Data Model / UoP tooling chain. ``normalize_name`` implements the FACE
rule that connection names match case-insensitively by uppercasing.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping

from .errors import InvalidConfigError, InvalidParamError
from .types import FACE_MAX_CONNECTION_NAME_LENGTH, Direction


def normalize_name(name: str) -> str:
    """Uppercase + validate a FACE connection name."""
    if not isinstance(name, str) or not name:
        raise InvalidParamError("connection name must be a non-empty string")
    upper = name.upper()
    if len(upper) > FACE_MAX_CONNECTION_NAME_LENGTH:
        raise InvalidParamError(
            f"connection name {name!r} exceeds {FACE_MAX_CONNECTION_NAME_LENGTH} chars"
        )
    return upper


@dataclass(frozen=True)
class ConnectionConfig:
    name: str
    direction: Direction = Direction.BI_DIRECTIONAL
    transport: str = "pubsub"
    role: str = "subscriber"
    address: str = "tcp://127.0.0.1:5555"
    max_message_size: int = 65536
    queue_depth: int = 64

    def __post_init__(self) -> None:
        object.__setattr__(self, "name", normalize_name(self.name))
        if self.transport not in ("pubsub", "bus"):
            raise InvalidConfigError(
                f"connection {self.name}: unknown transport {self.transport!r} "
                "(expected 'pubsub' or 'bus')"
            )
        # role defaults to "subscriber" for ergonomics; bus ignores it (any
        # value is coerced to "bus") so callers need not spell it out.
        if self.transport == "bus":
            object.__setattr__(self, "role", "bus")
        if self.transport == "pubsub" and self.role not in ("publisher", "subscriber"):
            raise InvalidConfigError(
                f"connection {self.name}: pubsub role must be "
                f"'publisher' or 'subscriber', got {self.role!r}"
            )
        if self.max_message_size <= 0:
            raise InvalidConfigError(
                f"connection {self.name}: max_message_size must be positive"
            )
        if self.queue_depth <= 0:
            raise InvalidConfigError(
                f"connection {self.name}: queue_depth must be positive"
            )

    @property
    def can_send(self) -> bool:
        return self.direction in (Direction.SOURCE, Direction.BI_DIRECTIONAL)

    @property
    def can_receive(self) -> bool:
        return self.direction in (Direction.DESTINATION, Direction.BI_DIRECTIONAL)


@dataclass(frozen=True)
class TssConfig:
    """Whole-TSS configuration: the set of creatable connections."""

    connections: tuple[ConnectionConfig, ...] = ()
    instance_name: str = "face-tss"

    def lookup(self, name: str) -> ConnectionConfig:
        key = normalize_name(name)
        for conn in self.connections:
            if conn.name == key:
                return conn
        raise InvalidParamError(f"no configured connection named {name!r}")

    def names(self) -> tuple[str, ...]:
        return tuple(c.name for c in self.connections)


def _coerce_direction(value: Any, conn_name: str) -> Direction:
    if isinstance(value, Direction):
        return value
    if isinstance(value, str):
        try:
            return Direction[value.upper()]
        except KeyError:
            raise InvalidConfigError(
                f"connection {conn_name}: bad direction {value!r}"
            ) from None
    try:
        return Direction(int(value))
    except (ValueError, TypeError):
        raise InvalidConfigError(
            f"connection {conn_name}: bad direction {value!r}"
        ) from None


def connection_from_mapping(data: Mapping[str, Any]) -> ConnectionConfig:
    try:
        name = data["name"]
    except KeyError:
        raise InvalidConfigError("connection entry is missing 'name'") from None
    kwargs: dict[str, Any] = {"name": name}
    if "direction" in data:
        kwargs["direction"] = _coerce_direction(data["direction"], str(name))
    for key in ("transport", "role", "address", "max_message_size", "queue_depth"):
        if key in data:
            kwargs[key] = data[key]
    return ConnectionConfig(**kwargs)


def config_from_mapping(data: Mapping[str, Any]) -> TssConfig:
    conns = data.get("connections", [])
    if not isinstance(conns, (list, tuple)):
        raise InvalidConfigError("'connections' must be a list")
    parsed = tuple(connection_from_mapping(c) for c in conns)
    seen: set[str] = set()
    for c in parsed:
        if c.name in seen:
            raise InvalidConfigError(f"duplicate connection name {c.name!r}")
        seen.add(c.name)
    return TssConfig(
        connections=parsed,
        instance_name=str(data.get("instance_name", "face-tss")),
    )


def config_from_json(text: str) -> TssConfig:
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise InvalidConfigError(f"invalid JSON config: {exc}") from exc
    if not isinstance(data, Mapping):
        raise InvalidConfigError("top-level JSON config must be an object")
    return config_from_mapping(data)


def config_from_file(path: str | Path) -> TssConfig:
    p = Path(path)
    suffix = p.suffix.lower()
    text = p.read_text(encoding="utf-8")
    if suffix == ".json":
        return config_from_json(text)
    if suffix == ".toml":
        try:
            import tomllib
        except ModuleNotFoundError as exc:
            raise InvalidConfigError(
                "TOML config needs Python 3.11+ (tomllib)"
            ) from exc
        try:
            data = tomllib.loads(text)
        except tomllib.TOMLDecodeError as exc:
            raise InvalidConfigError(f"invalid TOML config: {exc}") from exc
        return config_from_mapping(data)
    raise InvalidConfigError(
        f"unsupported config file type {suffix!r} (use .json or .toml)"
    )


@dataclass
class TssConfigBuilder:
    """Fluent helper for building configs in code / tests."""

    _connections: list[ConnectionConfig] = field(default_factory=list)
    _instance_name: str = "face-tss"

    def instance(self, name: str) -> "TssConfigBuilder":
        self._instance_name = name
        return self

    def add(
        self,
        name: str,
        direction: Direction | str = Direction.BI_DIRECTIONAL,
        transport: str = "pubsub",
        role: str = "subscriber",
        address: str = "tcp://127.0.0.1:5555",
        max_message_size: int = 65536,
        queue_depth: int = 64,
    ) -> "TssConfigBuilder":
        if isinstance(direction, str):
            direction = _coerce_direction(direction, name)
        self._connections.append(
            ConnectionConfig(
                name=name,
                direction=direction,
                transport=transport,
                role=role,
                address=address,
                max_message_size=max_message_size,
                queue_depth=queue_depth,
            )
        )
        return self

    def build(self) -> TssConfig:
        return config_from_mapping(
            {
                "instance_name": self._instance_name,
                "connections": [
                    {
                        "name": c.name,
                        "direction": c.direction,
                        "transport": c.transport,
                        "role": c.role,
                        "address": c.address,
                        "max_message_size": c.max_message_size,
                        "queue_depth": c.queue_depth,
                    }
                    for c in self._connections
                ],
            }
        )


__all__ = [
    "ConnectionConfig",
    "TssConfig",
    "TssConfigBuilder",
    "normalize_name",
    "config_from_mapping",
    "config_from_json",
    "config_from_file",
    "connection_from_mapping",
]
