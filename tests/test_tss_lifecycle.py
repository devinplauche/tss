"""TSS lifecycle tests (no network beyond loopback binds)."""

import pytest

from face_tss import (
    ConnectionClosedError,
    Direction,
    FaceTss,
    InvalidParamError,
    NotInitializedError,
    ReturnCode,
    TssConfig,
    TssConfigBuilder,
)


def _cfg(**kw):
    kw.setdefault("address", "tcp://127.0.0.1:1")  # never actually bound here
    return TssConfigBuilder().add("C", **kw).build()


def test_send_before_init_raises():
    tss = FaceTss()
    with pytest.raises(NotInitializedError):
        tss.create_connection("C")
    with pytest.raises(NotInitializedError):
        tss.send_message(1, b"x")


def test_initialize_idempotent():
    tss = FaceTss()
    assert tss.initialize(_cfg()) == ReturnCode.NO_ERROR
    assert tss.initialize(_cfg()) == ReturnCode.NO_ACTION


def test_create_unknown_connection():
    tss = FaceTss()
    tss.initialize(TssConfig())
    with pytest.raises(InvalidParamError):
        tss.create_connection("NOPE")


def test_direction_enforcement(tcp_addr):
    addr = tcp_addr()
    # SOURCE-only: subscriber side would need a publisher; use bus instead so
    # a single connection can be created without a peer.
    cfg = (
        TssConfigBuilder()
        .add("SRC", direction=Direction.SOURCE, transport="bus", address=addr)
        .build()
    )
    tss = FaceTss()
    tss.initialize(cfg)
    cid, _ = tss.create_connection("src")
    with pytest.raises(Exception):  # InvalidModeError on receive
        tss.receive_message(cid, timeout_ns=0)
    tss.destroy_connection(cid)
    tss.finalize()


def test_destroy_unknown_id_is_no_action_and_zero_is_invalid():
    tss = FaceTss()
    tss.initialize(TssConfig())
    assert tss.destroy_connection(999) == ReturnCode.NO_ACTION
    with pytest.raises(InvalidParamError):
        tss.destroy_connection(0)


def test_use_after_destroy_raises_closed(tcp_addr):
    cfg = TssConfigBuilder().add(
        "C", transport="bus", address=tcp_addr()).build()
    tss = FaceTss()
    tss.initialize(cfg)
    cid, _ = tss.create_connection("c")
    assert tss.destroy_connection(cid) == ReturnCode.NO_ERROR
    with pytest.raises(ConnectionClosedError):
        tss.send_message(cid, b"x")


def test_unregister_without_callback_is_no_action(tcp_addr):
    cfg = TssConfigBuilder().add(
        "C", direction="DESTINATION", transport="pubsub",
        role="subscriber", address=tcp_addr()).build()
    tss = FaceTss()
    tss.initialize(cfg)
    cid, _ = tss.create_connection("c")
    assert tss.unregister_callback(cid) == ReturnCode.NO_ACTION
    tss.destroy_connection(cid)
    tss.finalize()


def test_face32_return_code_and_constants():
    """FACE 3.2: RESOURCE_LIMIT_REACHED is the 15th return code (value 14),
    plus the new TSS/Common.idl transaction/GUID constants."""
    from face_tss import (
        CALLEE_PROVIDES_GUID,
        CALLEE_PROVIDES_TID,
        MAX_CONNECTIONS,
        TID_NOT_APPLICABLE,
        ResourceLimitError,
    )

    assert ReturnCode.RESOURCE_LIMIT_REACHED == 14
    assert len(ReturnCode) == 15
    assert TID_NOT_APPLICABLE == -1
    assert CALLEE_PROVIDES_TID == 0
    assert CALLEE_PROVIDES_GUID == 0
    assert MAX_CONNECTIONS == 64
    assert ResourceLimitError().return_code == ReturnCode.RESOURCE_LIMIT_REACHED


def test_connection_limit_raises_resource_limit(tcp_addr):
    """FACE 3.2: Create_Connection past MAX_CONNECTIONS -> RESOURCE_LIMIT."""
    from face_tss import MAX_CONNECTIONS, ResourceLimitError

    cfg = TssConfigBuilder().add(
        "C", direction="DESTINATION", transport="pubsub",
        role="subscriber", address=tcp_addr()).build()
    tss = FaceTss()
    tss.initialize(cfg)
    ids = [tss.create_connection("c")[0] for _ in range(MAX_CONNECTIONS)]
    with pytest.raises(ResourceLimitError) as exc:
        tss.create_connection("c")
    assert exc.value.return_code == ReturnCode.RESOURCE_LIMIT_REACHED
    # Freeing one slot lets creation succeed again.
    tss.destroy_connection(ids.pop(0))
    tss.create_connection("c")
    for cid in ids:
        tss.destroy_connection(cid)
    tss.finalize()


def test_configuration_interface_set_reference():
    """FACE Set_Reference injects a Configuration provider (issue #3)."""
    from face_tss import ConfigurationProvider, InvalidConfigError

    seen = {}

    class MyProvider(ConfigurationProvider):
        def load(self, resource):
            seen["resource"] = resource
            return _cfg()

    other = ConfigurationProvider()
    tss = FaceTss("t")
    provider = MyProvider()

    with pytest.raises(InvalidParamError):
        tss.set_reference("NotAConfiguration", provider, 1)
    with pytest.raises(InvalidParamError):
        tss.set_reference("Configuration", object(), 1)

    assert tss.set_reference("Configuration", provider, 7) == ReturnCode.NO_ERROR
    assert tss.set_reference("Configuration", provider, 7) == ReturnCode.NO_ACTION
    assert tss.set_reference("Configuration", other, 8) == ReturnCode.NOT_AVAILABLE

    # Initialize(CONFIGURATION_RESOURCE) goes through the injected provider.
    assert tss.initialize_from_resource("service://my-config") == ReturnCode.NO_ERROR
    assert seen["resource"] == "service://my-config"
    cid, _ = tss.create_connection("c")
    assert cid != 0
    # Steady state: no more Set_Reference; Initialize is idempotent.
    assert tss.set_reference("Configuration", provider, 7) == ReturnCode.INVALID_MODE
    assert tss.initialize_from_resource("service://my-config") == ReturnCode.NO_ACTION
    tss.finalize()


def test_initialize_from_resource_json_adapter():
    """Built-in JSON adapter: inline json:{...} and file paths (issue #3)."""
    from face_tss import InvalidConfigError

    tss = FaceTss("t")
    resource = (
        'json:{"instance_name": "r", "connections": ['
        '{"name": "C", "transport": "bus", "role": "bus",'
        ' "address": "inproc://cfg-json"}]}'
    )
    assert tss.initialize_from_resource(resource) == ReturnCode.NO_ERROR
    cid, _ = tss.create_connection("c")
    assert cid != 0
    tss.finalize()

    tss2 = FaceTss("t2")
    with pytest.raises(InvalidParamError):
        tss2.initialize_from_resource(None)
    with pytest.raises(InvalidConfigError):
        tss2.initialize_from_resource("/nonexistent/x.json")
    with pytest.raises(InvalidParamError):
        tss2.initialize_from_resource("r" * 256)
    tss2.finalize()
