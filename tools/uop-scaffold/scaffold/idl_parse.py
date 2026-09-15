"""OMG IDL subset parser for uop-scaffold's IDL frontend.

Parses the data-definition subset of OMG IDL (the language of the FACE
data model): ``module``, ``struct``, ``union``, ``enum``, ``typedef``
(including fixed arrays and sequences), ``sequence<T>`` / ``sequence<T,N>``,
``const`` and ``#include "..."``. Anything else is rejected with a clear
:class:`IdlError` -- never silently misparsed.

Deliberately out of scope: ``interface``, ``valuetype``, ``any``,
``fixed``, ``wchar``/``wstring``, ``readonly``/``attribute``, preprocessor
conditionals. ``#include`` is followed (cycle-safe); all other ``#``
directives are ignored.

The AST uses frozen dataclasses; see the classes below.
"""

import re
from dataclasses import dataclass
from pathlib import Path

__all__ = [
    "IdlError",
    "Primitive",
    "Sequence",
    "ScopedName",
    "Member",
    "Struct",
    "Union",
    "UnionCase",
    "Enum",
    "Typedef",
    "Const",
    "Module",
    "BoundedString",
    "parse_idl",
]


class IdlError(ValueError):
    """An IDL file could not be parsed or used unsupported constructs."""


# ---------------------------------------------------------------------------
# AST
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Primitive:
    """A primitive type name, e.g. 'long', 'string', 'boolean'."""
    name: str


@dataclass(frozen=True)
class Sequence:
    elem: object  # Primitive | ScopedName | Sequence
    bound: int | None  # None = unbounded


@dataclass(frozen=True)
class BoundedString:
    """A bounded string, e.g. string<32>. Parsed but rejected by the MVP
    lowering (FlatBuffers has no bound concept); kept as a distinct node
    so the error message is clear rather than a syntax error."""
    bound: int


@dataclass(frozen=True)
class ScopedName:
    parts: tuple  # e.g. ("Sensor", "FusedTrack")
    absolute: bool  # leading ::


@dataclass(frozen=True)
class Member:
    type: object  # Primitive | Sequence | ScopedName
    name: str


@dataclass(frozen=True)
class Struct:
    name: str
    members: tuple  # tuple[Member, ...]


@dataclass(frozen=True)
class UnionCase:
    labels: tuple  # ints, or ("default",)
    member: Member


@dataclass(frozen=True)
class Union:
    name: str
    discr: object  # Primitive | ScopedName (not used by the lowering)
    cases: tuple  # tuple[UnionCase, ...]


@dataclass(frozen=True)
class Enum:
    name: str
    members: tuple  # tuple[str, ...]


@dataclass(frozen=True)
class Typedef:
    name: str
    type: object  # Primitive | Sequence | ScopedName
    array: int | None  # fixed array size, e.g. typedef long V[4]


@dataclass(frozen=True)
class Const:
    name: str
    type: object
    value: int


@dataclass(frozen=True)
class Module:
    name: str
    defs: tuple


# ---------------------------------------------------------------------------
# Tokenizer
# ---------------------------------------------------------------------------

_KEYWORDS = {
    "module", "struct", "union", "enum", "typedef", "sequence", "const",
    "switch", "case", "default",
}

_TOKEN_RE = re.compile(r"""
    (?P<ws>\s+)
  | (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<include>\#\s*include\s*"(?P<incpath>[^"]+)")
  | (?P<directive>\#[^\n]*)
  | (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<hex>0[xX][0-9a-fA-F]+)
  | (?P<int>\d+)
  | (?P<str>"(?:[^"\\]|\\.)*")
  | (?P<op><<|>>|::)
  | (?P<sym>.)
""", re.DOTALL | re.VERBOSE)


@dataclass(frozen=True)
class _Tok:
    kind: str  # 'ident', 'keyword', 'int', 'str', 'sym', 'op', 'include', 'eof'
    value: object
    line: int


def _tokenize(text, path):
    toks = []
    line = 1
    pos = 0
    while pos < len(text):
        m = _TOKEN_RE.match(text, pos)
        if not m:
            raise IdlError(f"{path}:{line}: cannot tokenize here")
        pos = m.end()
        line += m.group(0).count("\n")
        kind = m.lastgroup
        if kind in ("ws", "comment", "directive"):
            continue
        if kind == "include":
            toks.append(_Tok("include", m.group("incpath"), line))
        elif kind == "ident":
            word = m.group(0)
            toks.append(_Tok("keyword" if word in _KEYWORDS else "ident",
                             word, line))
        elif kind == "hex":
            toks.append(_Tok("int", int(m.group(0), 16), line))
        elif kind == "int":
            toks.append(_Tok("int", int(m.group(0)), line))
        elif kind == "str":
            toks.append(_Tok("str", m.group(0), line))
        else:
            toks.append(_Tok(kind, m.group(0), line))
    toks.append(_Tok("eof", "", line))
    return toks


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------

_PRIMITIVES = {
    "boolean", "char", "wchar", "octet",
    "short", "long", "unsigned",
    "float", "double", "string", "wstring",
    "fixed", "any", "void",
}


class _Parser:
    def __init__(self, path, text, seen):
        self.path = path
        self.toks = _tokenize(text, path)
        self.pos = 0
        self.seen = seen  # set of resolved include paths (cycle guard)
        self.consts = {}  # qualified name -> int value
        self.defined = {}  # qualified name -> line of first definition
        self.scope = ()  # current module path, tuple[str, ...]

    # -- token helpers --------------------------------------------------
    def peek(self):
        return self.toks[self.pos]

    def next(self):
        t = self.toks[self.pos]
        self.pos += 1
        return t

    def err(self, t, msg):
        return IdlError(f"{self.path}:{t.line}: {msg}")

    @staticmethod
    def _show(t):
        return "<EOF>" if t.kind == "eof" else repr(t.value)

    def expect(self, kind, value=None):
        t = self.next()
        if t.kind != kind or (value is not None and t.value != value):
            want = f"{kind} {value!r}" if value else kind
            raise self.err(t, f"expected {want}, got {self._show(t)}")
        return t

    def accept(self, kind, value=None):
        t = self.peek()
        if t.kind == kind and (value is None or t.value == value):
            return self.next()
        return None

    def _qname(self, name):
        return "::".join(self.scope + (name,))

    def _define(self, t, name):
        """Record a definition; reject duplicates with line info."""
        key = self._qname(name)
        if key in self.defined:
            raise self.err(
                t, f"duplicate definition of '{key}' "
                   f"(first defined at line {self.defined[key]})")
        self.defined[key] = t.line

    # -- top level ------------------------------------------------------
    def parse(self):
        """Parse a top-level file; an empty file is an error."""
        defs = self.parse_rest()
        if not defs:
            raise IdlError(f"{self.path}: no definitions found")
        return defs

    def parse_rest(self):
        """Parse definitions; shared by top-level and included files."""
        defs = []
        while self.peek().kind != "eof":
            if self.accept("sym", ";"):
                continue
            t = self.peek()
            if t.kind == "include":
                defs.extend(self._do_include(t))
                continue
            if t.kind != "keyword":
                raise self.err(t, f"unexpected {self._show(t)}; "
                                  "only module/struct/union/enum/typedef/"
                                  "const definitions are supported")
            kw = t.value
            if kw == "module":
                defs.append(self._parse_module())
            elif kw == "struct":
                defs.append(self._parse_struct())
            elif kw == "union":
                defs.append(self._parse_union())
            elif kw == "enum":
                defs.append(self._parse_enum())
            elif kw == "typedef":
                defs.extend(self._parse_typedef())
            elif kw == "const":
                defs.append(self._parse_const())
            else:
                raise self.err(t, f"unsupported definition '{kw}'")
        return tuple(defs)

    def _do_include(self, t):
        self.next()  # consume the include token
        inc = (Path(self.path).parent / t.value).resolve()
        if inc in self.seen:
            return []
        if not inc.is_file():
            raise self.err(t, f"include not found: {t.value}")
        self.seen.add(inc)
        sub = _Parser(str(inc), inc.read_text(), self.seen)
        # Included defs live at this file's scope level (like C #include).
        sub.scope = self.scope
        sub.consts = self.consts
        sub.defined = self.defined
        return sub.parse_rest()

    # -- definitions ----------------------------------------------------
    def _parse_module(self):
        self.expect("keyword", "module")
        t = self.expect("ident")
        name = t.value
        self._define(t, name)
        self.expect("sym", "{")
        outer = self.scope
        self.scope = outer + (name,)
        defs = []
        while not self.accept("sym", "}"):
            if self.peek().kind == "eof":
                raise self.err(self.peek(), "unterminated module")
            if self.accept("sym", ";"):
                continue
            t = self.peek()
            if t.kind == "include":
                defs.extend(self._do_include(t))
                continue
            if t.kind != "keyword":
                raise self.err(t, f"unexpected {self._show(t)} in module")
            kw = t.value
            if kw == "module":
                defs.append(self._parse_module())
            elif kw == "struct":
                defs.append(self._parse_struct())
            elif kw == "union":
                defs.append(self._parse_union())
            elif kw == "enum":
                defs.append(self._parse_enum())
            elif kw == "typedef":
                defs.extend(self._parse_typedef())
            elif kw == "const":
                defs.append(self._parse_const())
            else:
                raise self.err(t, f"unsupported definition '{kw}'")
        self.accept("sym", ";")
        self.scope = outer
        return Module(name, tuple(defs))

    def _parse_struct(self):
        self.expect("keyword", "struct")
        t = self.expect("ident")
        name = t.value
        self._define(t, name)
        self.expect("sym", "{")
        members = []
        while not self.accept("sym", "}"):
            mtype = self._parse_type_spec()
            mname = self.expect("ident").value
            if self.accept("sym", "["):
                raise self.err(self.peek(), "anonymous arrays are not "
                                            "valid IDL; use a typedef")
            self.expect("sym", ";")
            members.append(Member(mtype, mname))
        self.expect("sym", ";")
        if not members:
            raise self.err(t, f"struct '{name}' has no members")
        return Struct(name, tuple(members))

    def _parse_union(self):
        self.expect("keyword", "union")
        t = self.expect("ident")
        name = t.value
        self._define(t, name)
        self.expect("keyword", "switch")
        self.expect("sym", "(")
        discr = self._parse_type_spec()
        self.expect("sym", ")")
        self.expect("sym", "{")
        cases = []
        while not self.accept("sym", "}"):
            labels = []
            while True:
                lt = self.peek()
                if lt.kind == "keyword" and lt.value == "case":
                    self.next()
                    labels.append(self._parse_const_expr())
                    self.expect("sym", ":")
                elif lt.kind == "keyword" and lt.value == "default":
                    self.next()
                    labels.append("default")
                    self.expect("sym", ":")
                else:
                    break
            if not labels:
                raise self.err(self.peek(), "union case needs a label")
            mtype = self._parse_type_spec()
            mname = self.expect("ident").value
            self.expect("sym", ";")
            cases.append(UnionCase(tuple(labels), Member(mtype, mname)))
        self.expect("sym", ";")
        if not cases:
            raise self.err(t, f"union '{name}' has no cases")
        return Union(name, discr, tuple(cases))

    def _parse_enum(self):
        self.expect("keyword", "enum")
        t = self.expect("ident")
        name = t.value
        self._define(t, name)
        self.expect("sym", "{")
        members = [self.expect("ident").value]
        while self.accept("sym", ","):
            if self.peek().kind == "sym" and self.peek().value == "}":
                break  # trailing comma
            members.append(self.expect("ident").value)
        self.expect("sym", "}")
        self.expect("sym", ";")
        return Enum(name, tuple(members))

    def _parse_typedef(self):
        self.expect("keyword", "typedef")
        ttype = self._parse_type_spec()
        out = []
        while True:
            t = self.expect("ident")
            array = None
            if self.accept("sym", "["):
                array = self._parse_const_expr()
                if array <= 0:
                    raise self.err(t, "array size must be positive")
                self.expect("sym", "]")
            self._define(t, t.value)
            out.append(Typedef(t.value, ttype, array))
            if not self.accept("sym", ","):
                break
        self.expect("sym", ";")
        return out

    def _parse_const(self):
        self.expect("keyword", "const")
        ctype = self._parse_type_spec()
        t = self.expect("ident")
        self._define(t, t.value)
        self.expect("sym", "=")
        value = self._parse_const_expr()
        self.expect("sym", ";")
        self.consts[self._qname(t.value)] = value
        return Const(t.value, ctype, value)

    # -- types ----------------------------------------------------------
    def _parse_type_spec(self):
        t = self.peek()
        if t.kind == "keyword" and t.value == "sequence":
            return self._parse_sequence()
        if t.kind == "ident" and t.value in ("string", "wstring"):
            la = (self.toks[self.pos + 1]
                  if self.pos + 1 < len(self.toks) else None)
            if la is not None and la.kind == "sym" and la.value == "<":
                # Bounded string: string<N>. Parsed here so the lowering
                # can reject it with a clear MVP message.
                self.next()  # string
                self.next()  # <
                bound = self._parse_const_expr()
                if bound <= 0:
                    raise self.err(t, "string bound must be positive")
                self._expect_gt()
                return BoundedString(bound)
        if t.kind == "ident" or (t.kind == "op" and t.value == "::"):
            return self._parse_scoped_or_primitive()
        raise self.err(t, f"expected a type, got {self._show(t)}")

    def _parse_scoped_or_primitive(self):
        t = self.peek()
        absolute = False
        parts = []
        if t.kind == "op" and t.value == "::":
            self.next()
            absolute = True
        # 'unsigned short' / 'unsigned long' / 'unsigned long long'
        # (all tokenize as ident: none of these words are keywords)
        if self.peek().kind == "ident" and self.peek().value == "unsigned":
            self.next()
            nxt = self.peek()
            if nxt.kind == "ident" and nxt.value in ("short", "long"):
                self.next()
                if nxt.value == "long" and self.peek().kind == "ident" \
                        and self.peek().value == "long":
                    self.next()
                    return Primitive("unsigned long long")
                return Primitive("unsigned " + nxt.value)
            raise self.err(nxt, "expected 'short' or 'long' after 'unsigned'")
        # 'long long'
        if self.peek().kind == "ident" and self.peek().value == "long":
            self.next()
            if self.peek().kind == "ident" and self.peek().value == "long":
                self.next()
                return Primitive("long long")
            return Primitive("long")
        t = self.next()
        if t.kind == "keyword" and t.value in _PRIMITIVES:
            return Primitive(t.value)
        if t.kind != "ident":
            raise self.err(t, f"expected a type name, got {self._show(t)}")
        parts.append(t.value)
        while self.accept("op", "::"):
            parts.append(self.expect("ident").value)
        if len(parts) == 1 and not absolute and parts[0] in _PRIMITIVES:
            return Primitive(parts[0])
        for p in parts:
            if p in _PRIMITIVES and p not in ("long",):
                # e.g. someone wrote 'string::x' -- nonsense
                raise self.err(t, f"invalid scoped name part {p!r}")
        return ScopedName(tuple(parts), absolute)

    def _parse_sequence(self):
        self.expect("keyword", "sequence")
        t0 = self.expect("sym", "<")
        elem = self._parse_type_spec()
        bound = None
        if self.accept("sym", ","):
            bound = self._parse_const_expr()
            if bound <= 0:
                raise self.err(t0, "sequence bound must be positive")
        self._expect_gt()
        return Sequence(elem, bound)

    def _expect_gt(self):
        """Expect '>', splitting a '>>' token for nested sequences."""
        t = self.peek()
        if t.kind == "sym" and t.value == ">":
            self.next()
            return
        if t.kind == "op" and t.value == ">>":
            # sequence<sequence<T>>: split '>>' into two '>' tokens.
            self.next()
            self.toks.insert(self.pos, _Tok("sym", ">", t.line))
            return
        raise self.err(t, f"expected '>', got {t.value!r}")

    # -- constant expressions -------------------------------------------
    def _parse_const_expr(self):
        return self._parse_or()

    def _parse_or(self):
        v = self._parse_xor()
        while self.accept("sym", "|"):
            v |= self._parse_xor()
        return v

    def _parse_xor(self):
        v = self._parse_and()
        while self.accept("sym", "^"):
            v ^= self._parse_and()
        return v

    def _parse_and(self):
        v = self._parse_shift()
        while self.accept("sym", "&"):
            v &= self._parse_shift()
        return v

    def _parse_shift(self):
        v = self._parse_add()
        while True:
            if self.accept("op", "<<"):
                v <<= self._parse_add()
            elif self.accept("op", ">>"):
                v >>= self._parse_add()
            else:
                return v

    def _parse_add(self):
        v = self._parse_mul()
        while True:
            if self.accept("sym", "+"):
                v += self._parse_mul()
            elif self.accept("sym", "-"):
                v -= self._parse_mul()
            else:
                return v

    def _parse_mul(self):
        v = self._parse_unary()
        while True:
            if self.accept("sym", "*"):
                v *= self._parse_unary()
            elif self.accept("sym", "/"):
                rhs = self._parse_unary()
                if rhs == 0:
                    raise self.err(self.peek(), "division by zero")
                v //= rhs
            elif self.accept("sym", "%"):
                v %= self._parse_unary()
            else:
                return v

    def _parse_unary(self):
        if self.accept("sym", "-"):
            return -self._parse_unary()
        if self.accept("sym", "+"):
            return self._parse_unary()
        if self.accept("sym", "~"):
            return ~self._parse_unary()
        return self._parse_primary()

    def _parse_primary(self):
        t = self.peek()
        if t.kind == "int":
            self.next()
            return t.value
        if t.kind == "sym" and t.value == "(":
            self.next()
            v = self._parse_const_expr()
            self.expect("sym", ")")
            return v
        if t.kind == "ident" or (t.kind == "op" and t.value == "::"):
            name = self._parse_scoped_or_primitive()
            if not isinstance(name, ScopedName):
                raise self.err(t, "cannot use a primitive as a constant")
            return self._lookup_const(name)
        raise self.err(t, f"expected a constant expression, got {t.value!r}")

    def _lookup_const(self, name):
        parts = name.parts
        if name.absolute:
            key = "::".join(parts)
            if key in self.consts:
                return self.consts[key]
            raise IdlError(f"{self.path}: unknown constant "
                           f"'::{key}'")
        for i in range(len(self.scope), -1, -1):
            key = "::".join(self.scope[:i] + parts)
            if key in self.consts:
                return self.consts[key]
        raise IdlError(f"{self.path}: unknown constant "
                       f"'{'::'.join(parts)}'")


def parse_idl(path):
    """Parse an IDL file. Returns a tuple of top-level definitions.

    Raises :class:`IdlError` on any syntax or unsupported-construct error.
    """
    path = Path(path)
    if not path.is_file():
        raise IdlError(f"{path}: file not found")
    seen = {path.resolve()}
    parser = _Parser(str(path), path.read_text(), seen)
    return parser.parse()
