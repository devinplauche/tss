"""Typed message helpers: application FlatBuffers payloads.

The TSS data path is ``typed dataclass -> FlatBuffers bytes -> TSS envelope
-> nng``. This module shows the pattern with a ``PositionReport`` example
and gives applications a base class to copy for their own IDL-generated
types: implement ``serialize``/``deserialize`` with ``flatc``-generated code
(or hand-rolled Builder/Table code like below) and pass the bytes to
``FaceTss.send_message`` / read them back from ``ReceivedMessage.payload``.

The PositionReport wire layout here is intentionally simple and versioned:

    offset 0: magic u32 0x504F5331 ("POS1")
    offset 4: vehicle_id bytes (utf-8, prefixed by u16 length)
    ...:     latitude f64, longitude f64, altitude f32, heading f32, valid u8

It is built with plain ``struct`` (not the flatbuffers Builder) so the demo
has zero generated code; production types should use ``flatc`` output and
ride opaquely in ``TssEnvelope.payload``.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

__all__ = ["PositionReport", "TypedMessage"]

_MAGIC = 0x504F53_31
_HEADER = struct.Struct("<IH")  # magic u32, id-length u16
_BODY = struct.Struct("<ddffB")  # lat f64, lon f64, alt f32, hdg f32, valid u8


class TypedMessage:
    """Interface for application typed messages carried by the TSS."""

    def serialize(self) -> bytes:
        raise NotImplementedError

    @classmethod
    def deserialize(cls, data: bytes) -> "TypedMessage":
        raise NotImplementedError


@dataclass
class PositionReport(TypedMessage):
    vehicle_id: str = ""
    latitude_deg: float = 0.0
    longitude_deg: float = 0.0
    altitude_m: float = 0.0
    heading_deg: float = 0.0
    valid: bool = True

    def serialize(self) -> bytes:
        vid = self.vehicle_id.encode("utf-8")
        if len(vid) > 0xFFFF:
            raise ValueError("vehicle_id too long")
        return (
            _HEADER.pack(_MAGIC, len(vid))
            + vid
            + _BODY.pack(
                float(self.latitude_deg),
                float(self.longitude_deg),
                float(self.altitude_m),
                float(self.heading_deg),
                1 if self.valid else 0,
            )
        )

    @classmethod
    def deserialize(cls, data: bytes | bytearray | memoryview) -> "PositionReport":
        buf = bytes(data)
        if len(buf) < _HEADER.size:
            raise ValueError("position report too short")
        magic, id_len = _HEADER.unpack_from(buf, 0)
        if magic != _MAGIC:
            raise ValueError(f"bad position magic 0x{magic:08X}")
        off = _HEADER.size
        end = off + id_len
        if len(buf) < end + _BODY.size:
            raise ValueError("position report truncated")
        vehicle_id = buf[off:end].decode("utf-8")
        lat, lon, alt, hdg, valid = _BODY.unpack_from(buf, end)
        return cls(
            vehicle_id=vehicle_id,
            latitude_deg=lat,
            longitude_deg=lon,
            altitude_m=alt,
            heading_deg=hdg,
            valid=bool(valid),
        )
