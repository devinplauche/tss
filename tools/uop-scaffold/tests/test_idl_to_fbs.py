"""Tests for the IDL -> .fbs lowering (scaffold.idl_to_fbs)."""

import pytest

from scaffold.idl_parse import IdlError, parse_idl
from scaffold.idl_to_fbs import all_c_names, find_type, lower_to_fbs


def _defs(text, tmp_path, name="t.idl"):
    p = tmp_path / name
    p.write_text(text)
    return parse_idl(str(p))


def _lower(text, tmp_path, root, **kw):
    defs = _defs(text, tmp_path)
    return lower_to_fbs(defs, root, **kw)


def test_primitive_widths(tmp_path):
    fbs = _lower("""\
struct W {
    long a;
    long long b;
    unsigned short c;
    char d;
    octet e;
    boolean f;
    float g;
    double h;
    string i;
};
""", tmp_path, ("W",))
    for frag in ("a: int32;", "b: int64;", "c: uint16;", "d: ubyte;",
                 "e: ubyte;", "f: bool;", "g: float;", "h: double;",
                 "i: string;"):
        assert frag in fbs, frag
    assert "root_type W;" in fbs


def test_namespace_from_modules(tmp_path):
    fbs = _lower("""\
module A { module B {
    struct Inner { long x; };
    struct Outer { Inner f; };
} };
""", tmp_path, ("A", "B", "Outer"))
    assert "namespace A.B;" in fbs
    assert "table Inner" in fbs
    assert "table Outer" in fbs
    assert "f: Inner;" in fbs
    assert "root_type Outer;" in fbs


def test_transitive_closure_only(tmp_path):
    fbs = _lower("""\
struct Used { long x; };
struct Unused { long y; };
struct Root { Used u; };
""", tmp_path, ("Root",))
    assert "table Used" in fbs
    assert "table Unused" not in fbs


def test_typedef_chain_resolves(tmp_path):
    fbs = _lower("""\
typedef sequence<long> LongSeq;
typedef LongSeq Renamed;
struct Root { Renamed r; sequence<sequence<double>> n; };
""", tmp_path, ("Root",))
    assert "r: [int32];" in fbs
    assert "n: [[double]];" in fbs


def test_enum_lowering(tmp_path):
    fbs = _lower("""\
enum Fix { None, RTK, PPP };
struct Root { Fix f; };
""", tmp_path, ("Root",))
    assert "enum Fix { None, RTK, PPP }" in fbs
    assert "f: Fix;" in fbs


def test_union_lowering(tmp_path):
    fbs = _lower("""\
struct Circle { double r; };
struct Square { double s; };
union Shape switch(long) {
    case 1: Circle c;
    case 2: Square q;
    case 3: string label;
    case 4: long code;
};
struct Root { Shape sh; };
""", tmp_path, ("Root",))
    assert "union Shape {" in fbs
    assert "Circle," in fbs
    assert "string," in fbs
    assert "int32," in fbs
    assert "table Circle" in fbs  # struct refs are transitively emitted


def test_bounded_string_rejected_loudly(tmp_path):
    defs = _defs("struct M { string<32> s; };\n", tmp_path)
    with pytest.raises(IdlError, match="bounded string.*not supported"):
        lower_to_fbs(defs, ("M",))


def test_bounded_sequence_rejected_loudly(tmp_path):
    defs = _defs("struct M { sequence<long, 8> s; };\n", tmp_path)
    with pytest.raises(IdlError, match="bounded sequence.*not supported"):
        lower_to_fbs(defs, ("M",))


def test_fixed_array_rejected_loudly(tmp_path):
    defs = _defs("typedef long V[4];\nstruct M { V v; };\n", tmp_path)
    with pytest.raises(IdlError, match="fixed array.*not supported"):
        lower_to_fbs(defs, ("M",))


def test_union_bool_member_rejected(tmp_path):
    defs = _defs("union U switch(long) { case 1: boolean b; };\n"
                 "struct M { U u; };\n", tmp_path)
    with pytest.raises(IdlError, match="cannot be a C field name"):
        lower_to_fbs(defs, ("M",))


def test_union_enum_member_rejected(tmp_path):
    defs = _defs("enum E { A, B };\n"
                 "union U switch(long) { case 1: E e; };\n"
                 "struct M { U u; };\n", tmp_path)
    with pytest.raises(IdlError, match="cannot have an enum member"):
        lower_to_fbs(defs, ("M",))


def test_union_sequence_member_rejected(tmp_path):
    defs = _defs("union U switch(long) { case 1: sequence<long> s; };\n"
                 "struct M { U u; };\n", tmp_path)
    with pytest.raises(IdlError, match="cannot have a sequence member"):
        lower_to_fbs(defs, ("M",))


def test_non_struct_root_rejected(tmp_path):
    defs = _defs("enum E { A };\n", tmp_path)
    with pytest.raises(IdlError, match="not an IDL struct"):
        lower_to_fbs(defs, ("E",))


def test_c_keyword_type_rejected(tmp_path):
    defs = _defs("struct int { long x; };\n", tmp_path)
    # 'int' is not an IDL keyword, so it parses; the lowering must refuse it.
    with pytest.raises(IdlError, match="would not compile as a C identifier"):
        lower_to_fbs(defs, ("int",))


def test_find_type_simple(tmp_path):
    defs = _defs("module N { struct M { long x; }; };\n", tmp_path)
    assert find_type(defs, "M") == ("N", "M")


def test_find_type_ambiguous(tmp_path):
    defs = _defs("module A { struct M { long x; }; };\n"
                 "module B { struct M { long y; }; };\n", tmp_path)
    with pytest.raises(IdlError, match="ambiguous.*idl_type"):
        find_type(defs, "M")


def test_find_type_qualified(tmp_path):
    defs = _defs("module A { struct M { long x; }; };\n"
                 "module B { struct M { long y; }; };\n", tmp_path)
    assert find_type(defs, "M", idl_type="B::M") == ("B", "M")


def test_find_type_absolute(tmp_path):
    defs = _defs("module A { struct M { long x; }; };\n", tmp_path)
    assert find_type(defs, "M", idl_type="::A::M") == ("A", "M")


def test_find_type_missing(tmp_path):
    defs = _defs("struct M { long x; };\n", tmp_path)
    with pytest.raises(IdlError, match="no IDL struct named 'Nope'"):
        find_type(defs, "Nope")


def test_find_type_not_a_struct(tmp_path):
    defs = _defs("enum E { A, B };\n", tmp_path)
    with pytest.raises(IdlError, match="'E' is an IDL enum, not a struct"):
        find_type(defs, "E")


def test_find_type_name_mismatch(tmp_path):
    defs = _defs("struct M { long x; };\n", tmp_path)
    with pytest.raises(IdlError, match="does not match IDL struct"):
        find_type(defs, "Other", idl_type="M")


def test_all_c_names(tmp_path):
    defs = _defs("""\
module N {
    struct S { long x; };
    union U switch(long) { case 1: S s; };
    enum E { A };
};
""", tmp_path)
    assert all_c_names(defs) == {"S", "U", "E"}
