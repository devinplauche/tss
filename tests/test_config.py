"""Config tests: parsing, validation, case-insensitive lookup."""

import pytest

from face_tss import (
    Direction,
    InvalidConfigError,
    InvalidParamError,
    TssConfigBuilder,
    config_from_json,
    normalize_name,
)


def test_normalize_uppercases():
    assert normalize_name("hello") == "HELLO"


def test_normalize_rejects_empty_and_long():
    with pytest.raises(InvalidParamError):
        normalize_name("")
    with pytest.raises(InvalidParamError):
        normalize_name("X" * 65)


def test_lookup_case_insensitive():
    cfg = TssConfigBuilder().add("Position", address="tcp://127.0.0.1:1").build()
    assert cfg.lookup("position").name == "POSITION"
    assert cfg.lookup("POSITION").name == "POSITION"


def test_lookup_unknown_raises_invalid_param():
    cfg = TssConfigBuilder().add("A", address="tcp://127.0.0.1:1").build()
    with pytest.raises(InvalidParamError):
        cfg.lookup("B")


def test_duplicate_names_rejected():
    with pytest.raises(InvalidConfigError):
        TssConfigBuilder().add("A", address="tcp://127.0.0.1:1").add(
            "a", address="tcp://127.0.0.1:2").build()


def test_bad_transport_role_combos_rejected():
    with pytest.raises(InvalidConfigError):
        TssConfigBuilder().add("A", transport="pubsub", role="bus",
                               address="tcp://127.0.0.1:1").build()
    # bus coerces any role to "bus" (validated in test_bus_role_coerced)
    with pytest.raises(InvalidConfigError):
        TssConfigBuilder().add("A", transport="carrier-pigeon",
                               address="tcp://127.0.0.1:1").build()


def test_bus_role_coerced():
    cfg = TssConfigBuilder().add("A", transport="bus", role="subscriber",
                                 address="tcp://127.0.0.1:1").build()
    assert cfg.lookup("A").role == "bus"


def test_direction_strings_and_modes():
    cfg = (
        TssConfigBuilder()
        .add("S", direction="SOURCE", transport="pubsub", role="publisher",
             address="tcp://127.0.0.1:1")
        .add("D", direction="DESTINATION", transport="pubsub", role="subscriber",
             address="tcp://127.0.0.1:2")
        .build()
    )
    assert cfg.lookup("s").can_send and not cfg.lookup("s").can_receive
    assert cfg.lookup("d").can_receive and not cfg.lookup("d").can_send


def test_json_round_trip_and_bad_json():
    cfg = config_from_json(
        '{"connections": [{"name": "X", "direction": "SOURCE", '
        '"transport": "pubsub", "role": "publisher", '
        '"address": "tcp://127.0.0.1:9"}]}'
    )
    assert cfg.lookup("x").direction == Direction.SOURCE
    with pytest.raises(InvalidConfigError):
        config_from_json("{nope")
    with pytest.raises(InvalidConfigError):
        config_from_json('{"connections": [{"direction": "SOURCE"}]}')
