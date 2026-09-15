"""Codegen tool tests: schema parsing, validation, and rejection cases."""

import importlib.util
import textwrap

import pytest

_SPEC = importlib.util.spec_from_file_location(
    "face_tss_codegen", "tools/face_tss_codegen.py")
codegen = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(codegen)


def parse(schema, tmp_path):
    p = tmp_path / "case.fbs"
    p.write_text(textwrap.dedent(schema))
    return codegen.parse_fbs(p)


def test_union_vector_nested_ok(tmp_path):
    ns, root, qname, tables, enums, unions = parse("""
        namespace X;
        table A { x:int; }
        table B { y:string; }
        union U { A, B }
        table R {
          u:U;
          items:[A];
          tags:[string];
          m:[[float]];
        }
        root_type R;
        """, tmp_path)
    assert root == "R"
    assert "U" in unions
    assert unions["U"].members == ["A", "B"]
    by_name = {t.name: t for t in tables}
    r = by_name["R"]
    kinds = {f.name: f.kind for f in r.fields}
    assert kinds == {"u": "union", "items": "vector", "tags": "vector",
                     "m": "vecvec"}
    # union takes two vtable slots: u=0, value=1, items=2, tags=3, m=4
    assert r.fields[0].fid == 0 and r.fields[1].fid == 2
    assert by_name["R"].nslots == 5


def test_explicit_ids_ok(tmp_path):
    _, _, _, tables, _, _ = parse("""
        namespace X;
        table R { b:string (id: 5); a:int (id: 0); }
        root_type R;
        """, tmp_path)
    fids = {f.name: f.fid for f in tables[0].fields}
    assert fids == {"b": 5, "a": 0}
    assert tables[0].nslots == 6


def test_union_explicit_id_two_slots(tmp_path):
    _, _, _, tables, _, unions = parse("""
        namespace X;
        table A { x:int; }
        union U { A }
        table R { u:U (id: 3); other:int (id: 0); }
        root_type R;
        """, tmp_path)
    r = tables[[t.name for t in tables].index("R")]
    u = next(f for f in r.fields if f.name == "u")
    assert u.fid == 3
    assert r.nslots == 5  # discriminator 3, value 4


@pytest.mark.parametrize("schema", [
    # vector of union with scalar member (not supported)
    """
    namespace X;
    table A { x:int; }
    union U { A, int }
    table R { us:[U]; }
    root_type R;
    """,
    # nested vector of tables (not supported)
    """
    namespace X;
    table A { x:int; }
    table R { m:[[A]]; }
    root_type R;
    """,
    # nested vector depth 4 (exceeds max 3)
    """
    namespace X;
    table R { m:[[[[int]]]]; }
    root_type R;
    """,
    # nested string vector depth 3 (max 2 for strings)
    """
    namespace X;
    table R { m:[[[string]]]; }
    root_type R;
    """,
    # duplicate explicit id
    """
    namespace X;
    table R { a:int (id: 1); b:int (id: 1); }
    root_type R;
    """,
    # partial explicit ids (all-or-none)
    """
    namespace X;
    table R { a:int (id: 1); b:int; }
    root_type R;
    """,
    # id out of range
    """
    namespace X;
    table R { a:int (id: 40000); }
    root_type R;
    """,
    # union value slot collides with another field's id
    """
    namespace X;
    table A { x:int; }
    union U { A }
    table R { u:U (id: 2); b:int (id: 3); }
    root_type R;
    """,
    # unknown field type
    """
    namespace X;
    table R { a:Nope; }
    root_type R;
    """,
])
def test_rejects_bad_schema(schema, tmp_path):
    with pytest.raises(ValueError):
        parse(schema, tmp_path)


def test_missing_root_defaults_to_last_table(tmp_path):
    # Pre-existing leniency: no root_type falls back to the last table.
    _, root, _, _, _, _ = parse("namespace X;\ntable R { a:int; }\n",
                                tmp_path)
    assert root == "R"


def test_enum_without_base_defaults_int32(tmp_path):
    _, _, _, tables, enums, _ = parse("""
    namespace X;
    enum E { A, B }
    table R { e:E; }
    root_type R;
    """, tmp_path)
    assert enums["E"].utype == "int32"
    e = enums["E"]
    assert [n for n, v in e.members] == ["A", "B"]
    assert [v for n, v in e.members] == [0, 1]


def test_nested_string_vector_ok(tmp_path):
    _, _, _, tables, _, _ = parse("""
    namespace X;
    table R { m:[[string]]; }
    root_type R;
    """, tmp_path)
    f = tables[0].fields[0]
    assert f.kind == "vecvec"
    assert f.velem == "string"
    assert f.vdepth == 2


def test_triple_nested_scalar_vector_ok(tmp_path):
    _, _, _, tables, _, _ = parse("""
    namespace X;
    table R { m:[[[int]]]; }
    root_type R;
    """, tmp_path)
    f = tables[0].fields[0]
    assert f.kind == "vecvec"
    assert f.velem == "scalar"
    assert f.ftype == "int"
    assert f.vdepth == 3


def test_mixed_union_ok(tmp_path):
    _, _, _, tables, _, unions = parse("""
    namespace X;
    table A { x:int; }
    union U { string, int32, A }
    table R { u:U; }
    root_type R;
    """, tmp_path)
    u = unions["U"]
    assert u.kinds == {"string": "string", "int32": "scalar", "A": "table"}
    assert u.stypes["int32"] == "int32"


def test_union_vector_ok(tmp_path):
    _, _, _, tables, _, unions = parse("""
    namespace X;
    table A { x:int; }
    union U { A, string }
    table R { us:[U]; }
    root_type R;
    """, tmp_path)
    r = tables[1]
    f = r.fields[0]
    assert f.kind == "vector"
    assert f.velem == "union"
    assert f.ref == "U"
    # vector of union occupies two slots (types, values)
    assert r.nslots == 2
