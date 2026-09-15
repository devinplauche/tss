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
# Richer models come from OMG IDL via an ``idl:`` key on the type, which
# lowers to .fbs and delegates to face_tss_codegen.py (see idl_parse,
# idl_to_fbs, idl_emit).
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
    fields: tuple = ()  # scalar flavor only
    # kind is "scalar" (packed little-endian codec, zero-dep) or "idl"
    # (OMG IDL-defined type, FlatBuffers wire format via face_tss_codegen).
    kind: str = "scalar"
    # IDL flavor only: absolute path of the .idl file and the qualified
    # IDL name, e.g. "Sensor::FusedTrack". The C type name is the
    # descriptor's ``name`` (validated equal to the IDL struct's name).
    idl_path: str = ""
    idl_qname: str = ""

    @property
    def wire_size(self):
        if self.kind != "scalar":  # pragma: no cover - misuse, not input
            raise AttributeError("IDL types have no fixed wire size")
        return sum(f.wire_size for f in self.fields)

    @property
    def is_idl(self):
        return self.kind == "idl"

    @property
    def typed_stem(self):
        """File stem of the generated codec: '<name>_typed' (lowercased)."""
        return f"{self.name.lower()}_typed"


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


def _load_idl_type(entry, ectx, desc_dir):
    """Load a type defined by an OMG IDL file (``idl:`` key)."""
    from . import idl_parse, idl_to_fbs

    name = _ident(entry["name"], f"{ectx}.name")
    if "fields" in entry:
        raise DescriptorError(
            f"{ectx}: 'idl' types do not take 'fields' (the IDL defines them)")
    rel = entry["idl"]
    if not isinstance(rel, str) or not rel:
        raise DescriptorError(f"{ectx}: 'idl' must be a file path string")
    idl_path = desc_dir / rel
    if not idl_path.is_file():
        raise DescriptorError(f"{ectx}: idl file not found: {rel}")
    idl_type = entry.get("idl_type")
    if idl_type is not None and not isinstance(idl_type, str):
        raise DescriptorError(f"{ectx}: 'idl_type' must be a string")
    try:
        defs = idl_parse.parse_idl(idl_path)
        qn = idl_to_fbs.find_type(defs, name, idl_type, str(idl_path))
        # Validate the lowering now so descriptor errors surface here,
        # not halfway through code generation.
        idl_to_fbs.lower_to_fbs(defs, qn, str(idl_path))
        c_names = idl_to_fbs.all_c_names(defs, str(idl_path))
    except idl_parse.IdlError as e:
        raise DescriptorError(f"{ectx}: {e}") from e
    return TypeDef(name=name, kind="idl", idl_path=str(idl_path),
                   idl_qname="::".join(qn)), c_names


def _load_types(raw, ctx, desc_dir):
    if not isinstance(raw, list) or not raw:
        raise DescriptorError(f"{ctx}: 'types' must be a non-empty list")
    types = []
    seen = set()
    pending_idl = []  # (ectx, TypeDef, c_names) for cross-checks below
    for i, entry in enumerate(raw):
        ectx = f"{ctx}.types[{i}]"
        if not isinstance(entry, dict):
            raise DescriptorError(f"{ectx}: must be a mapping")
        name = _ident(_req(entry, "name", ectx), f"{ectx}.name")
        if name in seen:
            raise DescriptorError(f"{ectx}: duplicate type '{name}'")
        seen.add(name)
        if "idl" in entry:
            tdef, c_names = _load_idl_type(entry, ectx, desc_dir)
            pending_idl.append((ectx, tdef, c_names))
            types.append(tdef)
            continue
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
    # Cross-flavor C-name checks. The combined <uop>_types.h includes every
    # generated codec header, so any two definitions of 'struct Foo' would
    # not compile. Each IDL type lowers its file with itself as root, so
    # the MVP allows one message type per IDL file.
    by_cname = {}
    for t in types:
        if t.kind == "scalar":
            by_cname[t.name] = f"scalar type '{t.name}'"
    seen_idl_paths = {}
    for ectx, tdef, c_names in pending_idl:
        if tdef.idl_path in seen_idl_paths:
            raise DescriptorError(
                f"{ectx}: IDL file '{tdef.idl_path}' already provides type "
                f"'{seen_idl_paths[tdef.idl_path]}': one message type per "
                f"IDL file in this MVP")
        seen_idl_paths[tdef.idl_path] = tdef.name
        for cn in sorted(c_names - {tdef.name}):
            if cn in by_cname:
                raise DescriptorError(
                    f"{ectx}: IDL C name '{cn}' from '{tdef.idl_path}' "
                    f"collides with {by_cname[cn]}: the generated headers "
                    f"would redefine it")
        for cn in c_names:
            by_cname[cn] = f"IDL file '{tdef.idl_path}'"
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
    types = _load_types(_req(uop, "types", ctx), ctx, path.parent)
    conns = _load_connections(_req(uop, "connections", ctx),
                              {t.name for t in types}, ctx)
    return UopModel(name, language, profile, types, conns)
