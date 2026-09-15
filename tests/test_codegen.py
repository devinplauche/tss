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
    # union with a non-table member
    """
    namespace X;
    table A { x:int; }
    union U { A, int }
    table R { u:U; }
    root_type R;
    """,
    # vector of union
    """
    namespace X;
    table A { x:int; }
    union U { A }
    table R { us:[U]; }
    root_type R;
    """,
    # nested vector of non-scalar
    """
    namespace X;
    table A { x:int; }
    table R { m:[[A]]; }
    root_type R;
    """,
    # nested vector of string
    """
    namespace X;
    table R { m:[[string]]; }
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
