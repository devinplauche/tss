"""Validated component model for uop-scaffold descriptors.

``load_descriptor(path)`` parses the restricted-YAML descriptor and returns
an immutable :class:`UopModel`, or raises :class:`DescriptorError` with a
human-readable message naming the offending field.
"""

import re
from dataclasses import dataclass, field
from pathlib import Path

from . import ysubset

__all__ = [
    "UopModel",
    "TypeDef",
    "FieldDef",
    "ConnectionDef",
    "DescriptorError",
    "SCALAR_TYPES",
    "load_descriptor",
]


class DescriptorError(Exception):
    """A descriptor failed validation."""


_IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

# MVP scalar language: fixed-width integers, bool, IEEE-754 floats.
# Wire format is always little-endian (see types_emit).
# Future: delegate richer models (.fbs with strings/tables/unions) to
# face_tss_codegen.py via a ``schema:`` key on the type.
SCALAR_TYPES = {
    "int8": ("int8_t", 1),
    "uint8": ("uint8_t", 1),
    "int16": ("int16_t", 2),
    "uint16": ("uint16_t", 2),
    "int32": ("int32_t", 4),
    "uint32": ("uint32_t", 4),
    "int64": ("int64_t", 8),
    "uint64": ("uint64_t", 8),
    "bool": ("bool", 1),
    "float": ("float", 4),
    "double": ("double", 8),
}

LANGUAGES = {"c99"}
PROFILES = {"general_purpose"}
DIRECTIONS = {"source", "destination"}
TRANSPORTS = {"pubsub"}
ROLES = {"publisher", "subscriber"}

# The only sane pairing in the MVP: publishers send, subscribers receive.
ROLE_DIRECTION = {"publisher": "source", "subscriber": "destination"}


@dataclass(frozen=True)
class FieldDef:
    name: str
    type: str

    @property
    def ctype(self):
        return SCALAR_TYPES[self.type][0]

    @property
    def wire_size(self):
        return SCALAR_TYPES[self.type][1]


@dataclass(frozen=True)
class TypeDef:
    name: str
    fields: tuple

    @property
    def wire_size(self):
        return sum(f.wire_size for f in self.fields)


@dataclass(frozen=True)
class ConnectionDef:
    name: str
    direction: str
    transport: str
    role: str
    type: str  # message type name
    callback: str | None  # subscriber callback; None for publishers


@dataclass(frozen=True)
class UopModel:
    name: str
    language: str
    profile: str
    types: tuple
    connections: tuple

    def type_by_name(self, name):
        for t in self.types:
            if t.name == name:
                return t
        raise KeyError(name)


def _req(mapping, key, ctx):
    if not isinstance(mapping, dict) or key not in mapping:
        raise DescriptorError(f"{ctx}: missing required key '{key}'")
    return mapping[key]


def _ident(value, ctx):
    if not isinstance(value, str) or not _IDENT_RE.fullmatch(value):
        raise DescriptorError(f"{ctx}: {value!r} is not a valid C identifier")
    return value


def _check_allowed(value, allowed, ctx):
    if value not in allowed:
        raise DescriptorError(
            f"{ctx}: {value!r} not in {{{', '.join(sorted(allowed))}}}"
        )
    return value


def _load_types(raw, ctx):
    if not isinstance(raw, list) or not raw:
        raise DescriptorError(f"{ctx}: 'types' must be a non-empty list")
    types = []
    seen = set()
    for i, entry in enumerate(raw):
        ectx = f"{ctx}.types[{i}]"
        if not isinstance(entry, dict):
            raise DescriptorError(f"{ectx}: must be a mapping")
        name = _ident(_req(entry, "name", ectx), f"{ectx}.name")
        if name in seen:
            raise DescriptorError(f"{ectx}: duplicate type '{name}'")
        seen.add(name)
        fraw = _req(entry, "fields", ectx)
        if not isinstance(fraw, list) or not fraw:
            raise DescriptorError(f"{ectx}: 'fields' must be a non-empty list")
        fields = []
        fseen = set()
        for j, fentry in enumerate(fraw):
            fctx = f"{ectx}.fields[{j}]"
            if not isinstance(fentry, dict):
                raise DescriptorError(f"{fctx}: must be a mapping")
            fname = _ident(_req(fentry, "name", fctx), f"{fctx}.name")
            if fname in fseen:
                raise DescriptorError(f"{fctx}: duplicate field '{fname}'")
            fseen.add(fname)
            ftype = _check_allowed(_req(fentry, "type", fctx), SCALAR_TYPES,
                                   f"{fctx}.type")
            fields.append(FieldDef(fname, ftype))
        types.append(TypeDef(name, tuple(fields)))
    return tuple(types)


def _load_connections(raw, type_names, ctx):
    if not isinstance(raw, list) or not raw:
        raise DescriptorError(
            f"{ctx}: 'connections' must be a non-empty list")
    conns = []
    seen = set()
    for i, entry in enumerate(raw):
        ectx = f"{ctx}.connections[{i}]"
        if not isinstance(entry, dict):
            raise DescriptorError(f"{ectx}: must be a mapping")
        name = _ident(_req(entry, "name", ectx), f"{ectx}.name")
        if name in seen:
            raise DescriptorError(f"{ectx}: duplicate connection '{name}'")
        seen.add(name)
        direction = _check_allowed(_req(entry, "direction", ectx), DIRECTIONS,
                                   f"{ectx}.direction")
        transport = _check_allowed(_req(entry, "transport", ectx), TRANSPORTS,
                                   f"{ectx}.transport")
        role = _check_allowed(_req(entry, "role", ectx), ROLES,
                              f"{ectx}.role")
        if ROLE_DIRECTION[role] != direction:
            raise DescriptorError(
                f"{ectx}: role '{role}' requires direction "
                f"'{ROLE_DIRECTION[role]}', got '{direction}'")
        typename = _req(entry, "type", ectx)
        if typename not in type_names:
            raise DescriptorError(
                f"{ectx}: unknown type '{typename}' "
                f"(defined: {', '.join(sorted(type_names))})")
        callback = entry.get("callback")
        if role == "subscriber":
            if callback is None:
                raise DescriptorError(
                    f"{ectx}: subscriber connections require a 'callback'")
            callback = _ident(callback, f"{ectx}.callback")
        elif callback is not None:
            raise DescriptorError(
                f"{ectx}: publisher connections do not take a 'callback'")
        conns.append(ConnectionDef(name, direction, transport, role,
                                   typename, callback))
    return tuple(conns)


def load_descriptor(path):
    """Parse and validate a descriptor file. Returns UopModel."""
    path = Path(path)
    try:
        raw = ysubset.parse(path.read_text())
    except ysubset.YSubError as e:
        raise DescriptorError(f"{path}: {e}") from e
    if not isinstance(raw, dict) or "uop" not in raw:
        raise DescriptorError(f"{path}: top level must have a 'uop' mapping")
    uop = raw["uop"]
    if not isinstance(uop, dict):
        raise DescriptorError(f"{path}: 'uop' must be a mapping")
    ctx = f"{path}:uop"
    name = _ident(_req(uop, "name", ctx), f"{ctx}.name")
    language = _check_allowed(_req(uop, "language", ctx), LANGUAGES,
                               f"{ctx}.language")
    profile = _check_allowed(_req(uop, "profile", ctx), PROFILES,
                             f"{ctx}.profile")
    types = _load_types(_req(uop, "types", ctx), ctx)
    conns = _load_connections(_req(uop, "connections", ctx),
                              {t.name for t in types}, ctx)
    return UopModel(name, language, profile, types, conns)
