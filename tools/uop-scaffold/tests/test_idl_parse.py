"""Tests for the OMG IDL subset parser (scaffold.idl_parse)."""

import pytest

from scaffold import idl_parse
from scaffold.idl_parse import (
    BoundedString,
    IdlError,
    Primitive,
    ScopedName,
    Sequence,
    parse_idl,
)


def _write(tmp_path, name, text):
    p = tmp_path / name
    p.write_text(text)
    return str(p)


def test_struct_with_all_primitives(tmp_path):
    f = _write(tmp_path, "a.idl", """\
struct All {
    boolean b;
    char c;
    octet o;
    short s;
    unsigned short us;
    long l;
    unsigned long ul;
    long long ll;
    unsigned long long ull;
    float fl;
    double d;
    string str;
};
""")
    defs = parse_idl(f)
    assert len(defs) == 1
    s = defs[0]
    assert isinstance(s, idl_parse.Struct)
    assert s.name == "All"
    kinds = [m.type for m in s.members]
    assert Primitive("long long") in kinds
    assert Primitive("unsigned short") in kinds
    assert Primitive("string") in kinds
    assert len(kinds) == 12


def test_module_scoping_and_typedef(tmp_path):
    f = _write(tmp_path, "a.idl", """\
module Outer {
    module Inner {
        typedef long Counter;
        struct Point {
            double x;
            double y;
        };
    };
};
""")
    defs = parse_idl(f)
    assert len(defs) == 1  # modules nest; only the outer is top-level
    outer = defs[0]
    assert isinstance(outer, idl_parse.Module)
    inner = outer.defs[0]
    assert isinstance(inner, idl_parse.Module)
    names = [d.name for d in inner.defs]
    assert names == ["Counter", "Point"]
    td = inner.defs[0]
    assert isinstance(td, idl_parse.Typedef)
    assert td.type == Primitive("long")


def test_enum_and_union(tmp_path):
    f = _write(tmp_path, "a.idl", """\
enum Color { Red, Green, Blue };
struct Circle { double r; };
struct Square { double side; };
union Shape switch(long) {
    case 1: Circle circle;
    case 2: Square square;
    default: string label;
};
""")
    defs = parse_idl(f)
    e = next(d for d in defs if isinstance(d, idl_parse.Enum))
    assert e.members == ("Red", "Green", "Blue")
    u = next(d for d in defs if isinstance(d, idl_parse.Union))
    assert u.discr == Primitive("long")
    assert [c.member.name for c in u.cases] == ["circle", "square", "label"]
    assert u.cases[0].labels == (1,)
    assert u.cases[2].labels == ("default",)
    assert u.cases[0].member.type == ScopedName(("Circle",), False)


def test_sequences_and_nesting(tmp_path):
    f = _write(tmp_path, "a.idl", """\
struct M {
    sequence<long> flat;
    sequence<sequence<double>> nested;
};
""")
    s = parse_idl(f)[0]
    assert s.members[0].type == Sequence(Primitive("long"), None)
    assert s.members[1].type == Sequence(
        Sequence(Primitive("double"), None), None)


def test_sequence_typedef_chain(tmp_path):
    f = _write(tmp_path, "a.idl", """\
typedef sequence<long> LongSeq;
typedef LongSeq Renamed;
struct M { Renamed r; };
""")
    defs = parse_idl(f)
    m = next(d for d in defs if isinstance(d, idl_parse.Struct))
    assert m.members[0].type == ScopedName(("Renamed",), False)
    td = next(d for d in defs if d.name == "LongSeq")
    assert td.type == Sequence(Primitive("long"), None)


def test_constants_and_expressions(tmp_path):
    f = _write(tmp_path, "a.idl", """\
const long A = 5;
const long B = A * 2 + 3;
typedef long Vec[16];
struct M {
    Vec v;
    string<64> name;
};
""")
    defs = parse_idl(f)
    consts = [d for d in defs if isinstance(d, idl_parse.Const)]
    assert {c.name: c.value for c in consts} == {"A": 5, "B": 13}
    td = next(d for d in defs if isinstance(d, idl_parse.Typedef))
    assert td.array == 16
    m = next(d for d in defs if isinstance(d, idl_parse.Struct))
    assert m.members[0].type == ScopedName(("Vec",), False)
    # bounded string parses (the lowering rejects it, loudly)
    assert m.members[1].type == BoundedString(64)


def test_bounded_sequence_parses(tmp_path):
    f = _write(tmp_path, "a.idl", "struct M { sequence<long, 8> s; };\n")
    s = parse_idl(f)[0]
    assert s.members[0].type == Sequence(Primitive("long"), 8)


def test_anonymous_array_member_rejected(tmp_path):
    f = _write(tmp_path, "a.idl", "struct M { long arr[4]; };\n")
    with pytest.raises(IdlError, match="anonymous arrays"):
        parse_idl(f)


def test_include(tmp_path):
    _write(tmp_path, "base.idl", "struct Base { long x; };\n")
    f = _write(tmp_path, "top.idl",
               '#include "base.idl"\nstruct Top { Base b; };\n')
    defs = parse_idl(f)
    assert {d.name for d in defs} == {"Base", "Top"}


def test_include_cycle_ok(tmp_path):
    _write(tmp_path, "a.idl", '#include "b.idl"\nstruct A { long x; };\n')
    _write(tmp_path, "b.idl", '#include "a.idl"\nstruct B { long y; };\n')
    defs = parse_idl(str(tmp_path / "a.idl"))
    assert {d.name for d in defs} == {"A", "B"}


def test_include_missing_is_error(tmp_path):
    f = _write(tmp_path, "a.idl", '#include "nope.idl"\n')
    with pytest.raises(IdlError, match="not found"):
        parse_idl(f)


def test_include_duplicate_across_files_rejected(tmp_path):
    _write(tmp_path, "base.idl", "struct Dup { long x; };\n")
    f = _write(tmp_path, "top.idl",
               '#include "base.idl"\nstruct Dup { long y; };\n')
    with pytest.raises(IdlError, match="duplicate definition of 'Dup'"):
        parse_idl(f)


def test_syntax_error_has_line(tmp_path):
    f = _write(tmp_path, "a.idl", "struct M {\n    long x\n};\n")
    with pytest.raises(IdlError, match=r"a\.idl:3"):
        parse_idl(f)


def test_unexpected_eof(tmp_path):
    f = _write(tmp_path, "a.idl", "struct M { long x;")
    with pytest.raises(IdlError, match=r"got <EOF>"):
        parse_idl(f)


def test_comments_ignored(tmp_path):
    f = _write(tmp_path, "a.idl",
               "/* block */\n// line\nstruct M { /* x */ long v; };\n")
    s = parse_idl(f)[0]
    assert s.members[0].name == "v"


def test_duplicate_definition_rejected(tmp_path):
    f = _write(tmp_path, "a.idl",
               "struct M { long x; };\nstruct M { long y; };\n")
    with pytest.raises(IdlError, match="duplicate definition of 'M'"):
        parse_idl(f)


def test_duplicate_in_module_reports_qualified_name(tmp_path):
    f = _write(tmp_path, "a.idl",
               "module N { struct M { long x; }; struct M { long y; }; };\n")
    with pytest.raises(IdlError, match="duplicate definition of 'N::M'"):
        parse_idl(f)


def test_empty_file_rejected(tmp_path):
    f = _write(tmp_path, "a.idl", "// nothing here\n")
    with pytest.raises(IdlError, match="no definitions"):
        parse_idl(f)


def test_bitmask_rejected(tmp_path):
    f = _write(tmp_path, "a.idl", "bitmask Flags { Flag0, Flag1 };\n")
    with pytest.raises(IdlError, match="only module/struct/union/enum"):
        parse_idl(f)


def test_absolute_scoped_name(tmp_path):
    f = _write(tmp_path, "a.idl", """\
module N { struct Inner { long x; }; };
struct M { ::N::Inner f; };
""")
    defs = parse_idl(f)
    m = next(d for d in defs if d.name == "M")
    assert m.members[0].type == ScopedName(("N", "Inner"), True)
