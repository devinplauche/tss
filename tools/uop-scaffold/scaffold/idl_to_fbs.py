"""Lower the OMG IDL subset AST to a FlatBuffers schema.

This is the bridge between the FACE data-model language (OMG IDL) and the
already-built, already-tested ``tools/face_tss_codegen.py`` pipeline: the
IDL frontend parses user-supplied IDL, this module emits an equivalent
``.fbs`` schema, and the existing codegen produces the C codec
(``<T>_serialize`` / ``<T>_deserialize`` / ``<T>_fini``).

Wire format note: the FACE standard leaves serialization pluggable (the
CTS's ``Serialization.idl`` defines it as an injectable per-type
interface), so using FlatBuffers as the wire encoding violates nothing in
the standard.

MVP limitations (all rejected loudly, never silently dropped):

- bounded ``string<N>`` / ``sequence<T,N>`` and fixed ``typedef T[N]``
  arrays: ``.fbs`` has no bound concept (bound enforcement is Phase B);
- IDL ``union`` members must be structs, ``string`` or integral scalars --
  ``boolean``/``float``/``double`` members are rejected (their ``.fbs``
  names are C keywords/macros and would not compile as field names), as
  are enum or sequence members (the ``.fbs`` codegen cannot express them);
- all struct/union/enum simple names in one IDL file must be unique (the
  ``.fbs`` schema is flat);
- the descriptor's message type must be an IDL ``struct`` (the codegen's
  public API is per root table).
"""

from . import idl_parse
from .idl_parse import IdlError  # noqa: F401  (re-exported)

__all__ = [
    "IdlError",
    "find_type",
    "all_c_names",
    "lower_to_fbs",
]

# IDL primitive -> .fbs scalar. Two traps avoided:
#  1. IDL 'long' is 32-bit (.fbs 'int'), IDL 'long long' is 64-bit
#     (.fbs 'long'); IDL 'char' has no .fbs counterpart (-> 'ubyte').
#  2. .fbs union members become C field names verbatim, so the aliases
#     below deliberately avoid C keywords: 'int32' not 'int', 'int16' not
#     'short', 'int64' not 'long' (the codegen would emit `int32_t int;`).
_PRIM_FBS = {
    "boolean": "bool",
    "char": "ubyte",
    "octet": "ubyte",
    "short": "int16",
    "unsigned short": "uint16",
    "long": "int32",
    "unsigned long": "uint32",
    "long long": "int64",
    "unsigned long long": "uint64",
    "float": "float",
    "double": "double",
    "string": "string",
}

# C99 keywords + bool/true/false macros: an IDL type with one of these
# simple names would not compile once lowered.
_C_KEYWORDS = {
    "auto", "break", "case", "char", "const", "continue", "default",
    "do", "double", "else", "enum", "extern", "float", "for", "goto",
    "if", "inline", "int", "long", "register", "restrict", "return",
    "short", "signed", "sizeof", "static", "struct", "switch",
    "typedef", "union", "unsigned", "void", "volatile", "while",
    "_Bool", "_Complex", "_Imaginary", "bool", "true", "false",
}

# Union members that would become a C keyword (or bool macro) as a field
# name: rejected loudly. (Fixing this needs a naming tweak in
# face_tss_codegen.py itself; follow-up, not MVP.)
_BAD_UNION_PRIMS = {"bool", "float", "double"}

_NAMED_DEFS = (
    idl_parse.Struct, idl_parse.Union, idl_parse.Enum,
    idl_parse.Typedef, idl_parse.Const,
)


class _SymTab:
    """Qualified-name symbol table over one IDL file."""

    def __init__(self, defs, path):
        self.by_qname = {}
        self.path = path
        self._walk(defs, ())

    def _walk(self, defs, scope):
        for d in defs:
            if isinstance(d, idl_parse.Module):
                self._walk(d.defs, scope + (d.name,))
            elif isinstance(d, _NAMED_DEFS):
                qn = scope + (d.name,)
                if qn in self.by_qname:
                    raise IdlError(
                        f"{self.path}: duplicate definition of "
                        f"'{'::'.join(qn)}'")
                self.by_qname[qn] = d
            else:  # pragma: no cover - parser only produces known nodes
                raise AssertionError(f"unknown AST node {d!r}")

    def resolve(self, name, scope):
        """Resolve a ScopedName to a qualified name tuple."""
        label = ("::" if name.absolute else "") + "::".join(name.parts)
        if name.absolute:
            if name.parts in self.by_qname:
                return name.parts
            raise IdlError(f"{self.path}: unknown type '{label}'")
        for i in range(len(scope), -1, -1):
            qn = scope[:i] + name.parts
            if qn in self.by_qname:
                return qn
        raise IdlError(f"{self.path}: unknown type '{label}'")


def _resolve_type(sym, t, scope):
    """Resolve typedefs; return (kind, payload).

    kind is one of 'prim' (fbs scalar name), 'struct' / 'union' / 'enum'
    (qualified name tuple), 'seq' ((elem, bound)), and raises IdlError for
    anything the MVP lowering cannot express.
    """
    while True:
        if isinstance(t, idl_parse.Primitive):
            if t.name not in _PRIM_FBS:
                raise IdlError(
                    f"{sym.path}: primitive '{t.name}' is not supported "
                    f"by this MVP (no wchar/wstring/fixed/any)")
            return ("prim", _PRIM_FBS[t.name])
        if isinstance(t, idl_parse.BoundedString):
            raise IdlError(
                f"{sym.path}: bounded string<string<{t.bound}>> is not "
                f"supported in this MVP (bounds are Phase B); use "
                f"unbounded 'string'")
        if isinstance(t, idl_parse.Sequence):
            if t.bound is not None:
                raise IdlError(
                    f"{sym.path}: bounded sequence<T,{t.bound}> is not "
                    f"supported in this MVP (bounds are Phase B); use "
                    f"unbounded 'sequence<T>'")
            return ("seq", (_resolve_type(sym, t.elem, scope), None))
        if isinstance(t, idl_parse.ScopedName):
            qn = sym.resolve(t, scope)
            d = sym.by_qname[qn]
            if isinstance(d, idl_parse.Typedef):
                if d.array is not None:
                    raise IdlError(
                        f"{sym.path}: fixed array typedef '{d.name}"
                        f"[{d.array}]' is not supported in this MVP "
                        f"(bounds are Phase B)")
                t = d.type
                scope = qn[:-1]
                continue
            if isinstance(d, idl_parse.Struct):
                return ("struct", qn)
            if isinstance(d, idl_parse.Union):
                return ("union", qn)
            if isinstance(d, idl_parse.Enum):
                return ("enum", qn)
            if isinstance(d, idl_parse.Const):
                raise IdlError(
                    f"{sym.path}: constant '{d.name}' used as a type")
            raise AssertionError(f"unreachable: {d!r}")  # pragma: no cover
        raise AssertionError(f"unknown type node {t!r}")  # pragma: no cover


def _fbs_type(sym, t, scope):
    """Render a resolved type as a .fbs field type string."""
    kind, payload = _resolve_type(sym, t, scope)
    if kind == "prim":
        return payload
    if kind in ("struct", "union", "enum"):
        return payload[-1]  # simple name; .fbs is flat (uniqueness checked)
    if kind == "seq":
        (elem, _bound) = payload
        return "[" + _fbs_of_resolved(sym, elem, scope) + "]"
    raise AssertionError(f"unreachable: {kind}")  # pragma: no cover


def _fbs_of_resolved(sym, resolved, scope):
    kind, payload = resolved
    if kind == "prim":
        return payload
    if kind in ("struct", "union", "enum"):
        return payload[-1]
    if kind == "seq":
        (elem, _bound) = payload
        return "[" + _fbs_of_resolved(sym, elem, scope) + "]"
    raise AssertionError(f"unreachable: {kind}")  # pragma: no cover


def _emit_struct(sym, qn):
    d = sym.by_qname[qn]
    scope = qn[:-1]
    lines = [f"table {qn[-1]} {{"]
    for m in d.members:
        lines.append(f"  {m.name}: {_fbs_type(sym, m.type, scope)};")
    lines.append("}")
    return "\n".join(lines), _referenced(sym, d, scope)


def _emit_union(sym, qn):
    d = sym.by_qname[qn]
    scope = qn[:-1]
    members = []
    refs = []
    for case in d.cases:
        kind, payload = _resolve_type(sym, case.member.type, scope)
        if kind == "prim":
            if payload in _BAD_UNION_PRIMS:
                raise IdlError(
                    f"{sym.path}: union '{qn[-1]}' member "
                    f"'{case.member.name}' has type '{payload}', which "
                    f"cannot be a C field name in this MVP; use an "
                    f"integral scalar, string, or struct member")
            members.append(payload)
        elif kind == "struct":
            members.append(payload[-1])
            refs.append(payload)
        else:
            what = {"union": "a union", "enum": "an enum",
                    "seq": "a sequence"}[kind]
            raise IdlError(
                f"{sym.path}: union '{qn[-1]}' cannot have {what} member "
                f"'{case.member.name}' in this MVP (only structs, "
                f"'string' and scalars)")
    # .fbs unions discriminate by member type: dedupe, keep first order.
    seen = set()
    uniq = []
    for m in members:
        if m not in seen:
            seen.add(m)
            uniq.append(m)
    lines = [f"union {qn[-1]} {{"]
    lines.extend(f"  {m}," for m in uniq)
    lines.append("}")
    return "\n".join(lines), refs


def _referenced(sym, d, scope):
    """Qualified names of named types referenced by a struct's members."""
    refs = []

    def walk(resolved):
        kind, payload = resolved
        if kind in ("struct", "union", "enum"):
            refs.append(payload)
        elif kind == "seq":
            walk(payload[0])

    for m in d.members:
        walk(_resolve_type(sym, m.type, scope))
    return refs


def lower_to_fbs(defs, root_qname, path="<idl>"):
    """Lower the transitive closure of an IDL struct to .fbs text.

    ``root_qname`` is the qualified name tuple of the message struct
    (see :func:`find_type`). Raises :class:`IdlError` for unsupported
    constructs.
    """
    sym = _SymTab(defs, path)
    root_d = sym.by_qname.get(root_qname)
    if root_d is None or not isinstance(root_d, idl_parse.Struct):
        raise IdlError(
            f"{path}: '{'::'.join(root_qname)}' is not an IDL struct")
    for cname in all_c_names(defs, path):
        if cname in _C_KEYWORDS:
            raise IdlError(
                f"{path}: IDL type '{cname}' would not compile as a C "
                f"identifier; rename it")

    chunks = []
    emitted = set()
    queue = [root_qname]
    while queue:
        qn = queue.pop(0)
        if qn in emitted:
            continue
        emitted.add(qn)
        d = sym.by_qname[qn]
        if isinstance(d, idl_parse.Struct):
            text, refs = _emit_struct(sym, qn)
        elif isinstance(d, idl_parse.Union):
            text, refs = _emit_union(sym, qn)
        elif isinstance(d, idl_parse.Enum):
            text = (f"enum {qn[-1]} {{ " +
                    ", ".join(d.members) + " }")
            refs = []
        else:  # pragma: no cover - only named types enter the queue
            raise AssertionError(f"unexpected {d!r}")
        chunks.append(text)
        queue.extend(r for r in refs if r not in emitted)

    ns = ".".join(root_qname[:-1])
    head = (f"// Generated by uop-scaffold from {path} -- DO NOT EDIT.\n"
            + (f"namespace {ns};\n\n" if ns else "\n"))
    body = "\n\n".join(chunks)
    return f"{head}{body}\n\nroot_type {root_qname[-1]};\n"


def find_type(defs, name, idl_type=None, path="<idl>"):
    """Locate the IDL struct for a descriptor type entry.

    ``name`` is the descriptor's type name (also the C type name).
    ``idl_type`` is an optional explicit qualifier like
    ``"Sensor::FusedTrack"``. Returns the qualified name tuple.
    Raises :class:`IdlError` when missing, ambiguous, or not a struct.
    """
    sym = _SymTab(defs, path)
    if idl_type is not None:
        parts = tuple(p for p in idl_type.split("::") if p)
        if not parts:
            raise IdlError(f"{path}: idl_type {idl_type!r} is empty")
        qn = sym.resolve(idl_parse.ScopedName(parts, idl_type.startswith("::")),
                         ())
        d = sym.by_qname.get(qn)
        if d is None or not isinstance(d, idl_parse.Struct):
            raise IdlError(
                f"{path}: idl_type '{idl_type}' is not an IDL struct")
        if qn[-1] != name:
            raise IdlError(
                f"{path}: descriptor type '{name}' does not match IDL "
                f"struct '{qn[-1]}' (rename one of them)")
        return qn
    matches = [qn for qn, d in sym.by_qname.items()
               if qn[-1] == name and isinstance(d, idl_parse.Struct)]
    if not matches:
        # Maybe it exists but is not a struct: give a better error.
        non_struct = [qn for qn, d in sym.by_qname.items()
                      if qn[-1] == name]
        if non_struct:
            raise IdlError(
                f"{path}: '{name}' is an IDL "
                f"{type(sym.by_qname[non_struct[0]]).__name__.lower()}, "
                f"not a struct: message types must be structs")
        raise IdlError(f"{path}: no IDL struct named '{name}' found")
    if len(matches) > 1:
        where = ", ".join("'::'.join(q)" for q in matches)
        raise IdlError(
            f"{path}: IDL struct '{name}' is ambiguous ({where}); "
            f"disambiguate with 'idl_type:'")
    return matches[0]


def all_c_names(defs, path="<idl>"):
    """Simple names of every struct/union/enum in the file.

    Used to keep generated C symbols unique across a descriptor: the
    combined ``<uop>_types.h`` would not compile with two ``struct Foo``.
    """
    sym = _SymTab(defs, path)
    return {qn[-1] for qn, d in sym.by_qname.items()
            if isinstance(d, (idl_parse.Struct, idl_parse.Union,
                              idl_parse.Enum))}
