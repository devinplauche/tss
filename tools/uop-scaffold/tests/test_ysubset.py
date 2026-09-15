"""Tests for the restricted YAML-subset parser."""

from pathlib import Path

import pytest

from scaffold.ysubset import parse, YSubError

_HERE = Path(__file__).resolve().parent
SENSOR = (_HERE.parent / "examples" / "sensor" / "sensor_uop.yaml").read_text()


def test_parses_real_descriptor():
    doc = parse(SENSOR)
    assert doc["uop"]["name"] == "sensor_uop"
    assert doc["uop"]["language"] == "c99"
    assert [t["name"] for t in doc["uop"]["types"]] == [
        "raw_detection", "fused_track"]
    fused = doc["uop"]["types"][1]
    assert [f["name"] for f in fused["fields"]] == ["x", "y", "track_id"]
    assert [f["type"] for f in fused["fields"]] == ["int32"] * 3
    conns = doc["uop"]["connections"]
    assert [c["name"] for c in conns] == ["RAW_DETECTION", "FUSED_TRACK"]
    assert conns[0]["role"] == "subscriber"
    assert conns[0]["callback"] == "on_raw_detection"
    assert "callback" not in conns[1]


def test_scalars_comments_and_quotes():
    doc = parse("""
# a comment
name: hello # trailing
count: 42
neg: -7
quoted: "with: colon"
single: 'it # works'
""")
    assert doc == {"name": "hello", "count": 42, "neg": -7,
                   "quoted": "with: colon", "single": "it # works"}


def test_nested_and_empty_values():
    doc = parse("a:\n  b:\n    c: 1\nd:\n")
    assert doc == {"a": {"b": {"c": 1}}, "d": None}


def test_list_of_scalars_and_maps():
    doc = parse("l:\n  - one\n  - two\nm:\n  - k: 1\n    j: 2\n")
    assert doc == {"l": ["one", "two"], "m": [{"k": 1, "j": 2}]}


def test_rejects_tabs():
    with pytest.raises(YSubError):
        parse("a:\n\tb: 1\n")


def test_rejects_bad_indent():
    with pytest.raises(YSubError):
        parse("a:\n   b: 1\n  c: 2\n")


def test_rejects_duplicate_keys():
    with pytest.raises(YSubError):
        parse("a: 1\na: 2\n")


def test_rejects_flow_syntax():
    with pytest.raises(YSubError):
        parse("a: {b: c}\n")


def test_rejects_top_level_list():
    with pytest.raises(YSubError):
        parse("- a\n- b\n")


def test_rejects_empty():
    with pytest.raises(YSubError):
        parse("# nothing here\n")


def test_rejects_nested_list():
    with pytest.raises(YSubError):
        parse("uop:\n  name: x\n  items:\n    - - a\n")


def test_negative_int_list_item():
    # "- -5" is the integer -5, not a nested list.
    assert parse("items:\n  - -5\n") == {"items": [-5]}
