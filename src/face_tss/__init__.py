"""face_tss: a FACE Transport Services Segment over nng + FlatBuffers."""

from .config import (
    ConnectionConfig,
    TssConfig,
    TssConfigBuilder,
    config_from_file,
    config_from_json,
    config_from_mapping,
    normalize_name,
)
from .envelope import Envelope, decode_envelope, encode_envelope
from .errors import (
    BufferTooSmallError,
    ConnectionClosedError,
    FaceTssError,
    InvalidConfigError,
    InvalidModeError,
    InvalidParamError,
    MessageStaleError,
    NotInitializedError,
    TimedOutError,
    TransportError,
)
from .transport import BusTransport, PubSubTransport, open_transport
from .tss import ConnectionId, FaceTss, ReceivedMessage, TransactionId, TssStats
from .typed import PositionReport, TypedMessage
from .types import (
    CONNECTION_ID_INVALID,
    TIMEOUT_INFINITE,
    TRANSACTION_ID_UNSPECIFIED,
    Direction,
    Header,
    MessageValidity,
    ReturnCode,
    now_ns,
)

__all__ = [
    "ConnectionConfig",
    "TssConfig",
    "TssConfigBuilder",
    "config_from_file",
    "config_from_json",
    "config_from_mapping",
    "normalize_name",
    "Envelope",
    "decode_envelope",
    "encode_envelope",
    "FaceTssError",
    "BufferTooSmallError",
    "ConnectionClosedError",
    "InvalidConfigError",
    "InvalidModeError",
    "InvalidParamError",
    "MessageStaleError",
    "NotInitializedError",
    "TimedOutError",
    "TransportError",
    "BusTransport",
    "PubSubTransport",
    "open_transport",
    "ConnectionId",
    "FaceTss",
    "ReceivedMessage",
    "TransactionId",
    "TssStats",
    "PositionReport",
    "TypedMessage",
    "CONNECTION_ID_INVALID",
    "TIMEOUT_INFINITE",
    "TRANSACTION_ID_UNSPECIFIED",
    "Direction",
    "Header",
    "MessageValidity",
    "ReturnCode",
    "now_ns",
]

__version__ = "0.1.0"
