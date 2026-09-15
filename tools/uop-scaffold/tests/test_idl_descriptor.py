"""Tests for the ``idl:`` descriptor key (scaffold.model + generators)."""

import pytest

from scaffold.gen_uop import generate_tree
from scaffold.model import DescriptorError, load_descriptor


def _write(path, text):
    path.write_text(text)
    return str(path)


def _descriptor(tmp_path, idl_text, types_block):
    """Write types.idl + a uop.yaml whose uop.types is ``types_block``."""
    _write(tmp_path / "types.idl", idl_text)
    yaml_text = f"""\
uop:
  name: idl_uop
  language: c99
  profile: general_purpose

  types:
{types_block}
  connections:
    - name: CH
      type: Msg
      direction: source
      transport: pubsub
      role: publisher
"""
    return _write(tmp_path / "uop.yaml", yaml_text)


_MSG_ENTRY = """\
    - name: Msg
      idl: types.idl
"""


def test_idl_type_loads(tmp_path):
    desc = _descriptor(tmp_path, "struct Msg { long x; };\n", _MSG_ENTRY)
    model = load_descriptor(desc)
    assert len(model.types) == 1
    t = model.types[0]
    assert t.kind == "idl"
    assert t.name == "Msg"
    assert t.idl_qname == "Msg"
    assert t.idl_path.endswith("types.idl")


def test_idl_type_with_module_needs_idl_type(tmp_path):
    idl = ("module A { struct Msg { long x; }; };\n"
           "module B { struct Msg { long y; }; };\n")
    desc = _descriptor(tmp_path, idl, _MSG_ENTRY)
    with pytest.raises(DescriptorError, match="ambiguous"):
        load_descriptor(desc)


def test_idl_type_disambiguated(tmp_path):
    idl = ("module A { struct Msg { long x; }; };\n"
           "module B { struct Msg { long y; }; };\n")
    entry = """\
    - name: Msg
      idl: types.idl
      idl_type: B::Msg
"""
    desc = _descriptor(tmp_path, idl, entry)
    model = load_descriptor(desc)
    assert model.types[0].idl_qname == "B::Msg"


def test_idl_missing_file(tmp_path):
    desc = _descriptor(tmp_path, "struct Msg { long x; };\n", _MSG_ENTRY)
    bad = (tmp_path / "uop.yaml").read_text().replace("types.idl", "nope.idl")
    (tmp_path / "uop.yaml").write_text(bad)
    with pytest.raises(DescriptorError, match="idl file not found"):
        load_descriptor(desc)


def test_idl_rejects_fields_alongside(tmp_path):
    entry = """\
    - name: Msg
      idl: types.idl
      fields:
        - name: x
          type: uint32
"""
    desc = _descriptor(tmp_path, "struct Msg { long x; };\n", entry)
    with pytest.raises(DescriptorError, match="do not take 'fields'"):
        load_descriptor(desc)


def test_idl_rejects_bounded_string_loudly(tmp_path):
    desc = _descriptor(tmp_path, "struct Msg { string<32> s; };\n", _MSG_ENTRY)
    with pytest.raises(DescriptorError, match="bounded string"):
        load_descriptor(desc)


def test_idl_rejects_non_struct(tmp_path):
    desc = _descriptor(tmp_path, "enum Msg { A, B };\n", _MSG_ENTRY)
    with pytest.raises(DescriptorError, match="not a struct"):
        load_descriptor(desc)


def test_one_type_per_idl_file(tmp_path):
    idl = "struct A { long x; };\nstruct B { long y; };\n"
    types_block = """\
    - name: A
      idl: types.idl
      idl_type: A
    - name: B
      idl: types.idl
      idl_type: B
"""
    desc = _descriptor(tmp_path, idl, types_block)
    # Fix the connection to reference A (helper hardcodes Msg).
    fixed = (tmp_path / "uop.yaml").read_text().replace("type: Msg",
                                                        "type: A")
    (tmp_path / "uop.yaml").write_text(fixed)
    with pytest.raises(DescriptorError, match="one message type per IDL file"):
        load_descriptor(desc)


def test_scalar_idl_c_name_collision(tmp_path):
    # Scalar type 'Inner' + IDL file defining struct Inner -> no compile.
    idl = "struct Inner { long x; };\nstruct Msg { Inner f; };\n"
    types_block = """\
    - name: Inner
      fields:
        - name: x
          type: uint32
    - name: Msg
      idl: types.idl
"""
    desc = _descriptor(tmp_path, idl, types_block)
    with pytest.raises(DescriptorError, match="collides with scalar type"):
        load_descriptor(desc)


def test_mixed_scalar_and_idl_ok(tmp_path):
    types_block = """\
    - name: Msg
      idl: types.idl
    - name: Raw
      fields:
        - name: v
          type: uint32
"""
    desc = _descriptor(tmp_path, "struct Msg { long x; };\n", types_block)
    model = load_descriptor(desc)
    assert [t.name for t in model.types] == ["Msg", "Raw"]
    assert model.types[0].kind == "idl"
    assert model.types[1].kind == "scalar"


def test_generate_tree_emits_idl_codec(tmp_path):
    desc = _descriptor(tmp_path, "struct Msg { long x; };\n", _MSG_ENTRY)
    model = load_descriptor(desc)
    files, _ = generate_tree(model, {})
    names = set(files)
    assert "msg.fbs" in names
    assert "msg_typed.h" in names
    assert "msg_typed.c" in names
    assert "Msg_serialize" in files["msg_typed.h"]


def test_generate_tree_without_idl_has_no_codec(tmp_path):
    types_block = """\
    - name: Raw
      fields:
        - name: v
          type: uint32
"""
    desc = _descriptor(tmp_path, "struct Unused { long x; };\n", types_block)
    # Connection references Raw; rewrite the helper's Msg reference.
    fixed = (tmp_path / "uop.yaml").read_text().replace("type: Msg",
                                                        "type: Raw")
    (tmp_path / "uop.yaml").write_text(fixed)
    model = load_descriptor(desc)
    files, _ = generate_tree(model, {})
    assert not [n for n in files if n.endswith("_typed.c")]
