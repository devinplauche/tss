"""Tests for descriptor validation."""

from pathlib import Path

import pytest

from scaffold.model import load_descriptor, DescriptorError

_HERE = Path(__file__).resolve().parent
SENSOR = str(_HERE.parent / "examples" / "sensor" / "sensor_uop.yaml")

BASE = """\
uop:
  name: demo
  language: c99
  profile: general_purpose
  types:
    - name: msg
      fields:
        - name: a
          type: int32
  connections:
    - name: IN
      direction: destination
      transport: pubsub
      role: subscriber
      type: msg
      callback: on_msg
    - name: OUT
      direction: source
      transport: pubsub
      role: publisher
      type: msg
"""

TYPES_BLOCK = """\
  types:
    - name: msg
      fields:
        - name: a
          type: int32
"""


def write_tmp(tmp_path, text):
    p = tmp_path / "d.yaml"
    p.write_text(text)
    return p


def test_loads_sensor_descriptor():
    m = load_descriptor(SENSOR)
    assert m.name == "sensor_uop"
    assert m.language == "c99"
    assert m.profile == "general_purpose"
    assert [t.name for t in m.types] == ["raw_detection", "fused_track"]
    assert m.type_by_name("fused_track").wire_size == 12
    assert [c.name for c in m.connections] == ["RAW_DETECTION", "FUSED_TRACK"]
    sub = m.connections[0]
    assert (sub.direction, sub.role, sub.callback) == (
        "destination", "subscriber", "on_raw_detection")
    assert m.connections[1].callback is None


def test_loads_minimal(tmp_path):
    m = load_descriptor(write_tmp(tmp_path, BASE))
    assert m.name == "demo"
    assert len(m.types) == 1 and len(m.connections) == 2


def bad(tmp_path, text):
    with pytest.raises(DescriptorError):
        load_descriptor(write_tmp(tmp_path, text))


def test_rejects_unknown_type_ref(tmp_path):
    bad(tmp_path, BASE.replace("type: msg", "type: nope", 1))


def test_rejects_role_direction_mismatch(tmp_path):
    bad(tmp_path, BASE.replace("direction: destination",
                               "direction: source", 1))


def test_rejects_subscriber_without_callback(tmp_path):
    bad(tmp_path, BASE.replace("      callback: on_msg\n", ""))


def test_rejects_publisher_with_callback(tmp_path):
    bad(tmp_path, BASE.replace("      role: publisher",
                               "      role: publisher\n      callback: on_x"))


def test_rejects_bad_identifier(tmp_path):
    bad(tmp_path, BASE.replace("name: demo", "name: 9demo"))


def test_rejects_unknown_scalar(tmp_path):
    bad(tmp_path, BASE.replace("type: int32", "type: string"))


def test_rejects_duplicate_connection(tmp_path):
    bad(tmp_path, BASE.replace("name: OUT", "name: IN"))


def test_rejects_duplicate_field(tmp_path):
    dup = ("        - name: a\n          type: int32\n"
           "        - name: a\n          type: int32\n")
    bad(tmp_path, BASE.replace(
        "        - name: a\n          type: int32\n", dup))


def test_rejects_missing_uop(tmp_path):
    bad(tmp_path, "nonsense: 1\n")


def test_rejects_empty_types(tmp_path):
    bad(tmp_path, BASE.replace(TYPES_BLOCK, "  types:\n"))
