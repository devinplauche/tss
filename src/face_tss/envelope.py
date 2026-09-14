"""FlatBuffers TSS envelope codec (hand-rolled, no flatc step).

Wire schema (see ``schemas/tss_envelope.fbs``), field slots are fixed:

    slot 0  connection_name : string
    slot 1  transaction_id  : long
    slot 2  source_id       : long
    slot 3  sequence_number : ulong
    slot 4  timestamp_ns    : long
    slot 5  payload         : [ubyte]  (opaque typed message bytes)

The typed payload is produced/consumed by generated FlatBuffers code for the
application's own schema; the TSS never interprets it, it only frames it.
``encode``/``decode`` use the ``flatbuffers`` runtime directly (Builder for
writing, Table for reading) so there is no ``flatc`` build dependency.
"""

from __future__ import annotations

from dataclasses import dataclass

import flatbuffers
from flatbuffers import encode
from flatbuffers import number_types as N
from flatbuffers.table import Table

__all__ = ["Envelope", "encode_envelope", "decode_envelope", "ENVELOPE_SLOTS"]

#: vtable offsets used with Table.GetSlot/Offset (4 + 2*slot).
ENVELOPE_SLOTS = {
    "connection_name": 0,
    "transaction_id": 1,
    "source_id": 2,
    "sequence_number": 3,
    "timestamp_ns": 4,
    "payload": 5,
}


def _voff(slot: int) -> int:
    return 4 + slot * 2


@dataclass(frozen=True)
class Envelope:
    connection_name: str
    transaction_id: int
    source_id: int
    sequence_number: int
    timestamp_ns: int
    payload: bytes


def encode_envelope(env: Envelope) -> bytes:
    """Serialize an envelope to FlatBuffers bytes ready for nng send.

    NOTE: the Builder's ``*Slot`` writers take the plain field index
    (0..5); only the Table *readers* use vtable offsets (4 + 2*slot).
    """
    builder = flatbuffers.Builder(256)
    name_off = builder.CreateString(env.connection_name)
    payload_off = builder.CreateByteVector(bytes(env.payload))
    builder.StartObject(6)
    # Prepend in reverse field order (highest slot first).
    builder.PrependUOffsetTRelativeSlot(5, payload_off, 0)
    builder.PrependInt64Slot(4, int(env.timestamp_ns), 0)
    builder.PrependUint64Slot(3, int(env.sequence_number), 0)
    builder.PrependInt64Slot(2, int(env.source_id), 0)
    builder.PrependInt64Slot(1, int(env.transaction_id), 0)
    builder.PrependUOffsetTRelativeSlot(0, name_off, 0)
    builder.Finish(builder.EndObject())
    return bytes(builder.Output())


def decode_envelope(buf: bytes | bytearray | memoryview) -> Envelope:
    """Parse FlatBuffers bytes received from nng into an Envelope."""
    bb = bytearray(buf)
    if len(bb) < 4:
        raise ValueError(f"envelope too short ({len(bb)} bytes)")
    root = encode.Get(N.UOffsetTFlags.packer_type, bb, 0)
    if root < 0 or root >= len(bb):
        raise ValueError(f"envelope root {root} out of range (len {len(bb)})")
    tab = Table(bb, root)

    off = tab.Offset(_voff(0))
    if off == 0:
        raise ValueError("envelope is missing connection_name")
    connection_name = tab.String(off + tab.Pos).decode("utf-8")

    transaction_id = int(tab.GetSlot(_voff(1), 0, N.Int64Flags))
    source_id = int(tab.GetSlot(_voff(2), 0, N.Int64Flags))
    sequence_number = int(tab.GetSlot(_voff(3), 0, N.Uint64Flags))
    timestamp_ns = int(tab.GetSlot(_voff(4), 0, N.Int64Flags))

    off = tab.Offset(_voff(5))
    if off == 0:
        payload = b""
    else:
        start = tab.Vector(off)
        length = tab.VectorLen(off)
        if start < 0 or length < 0 or start + length > len(bb):
            raise ValueError("envelope payload range is corrupt")
        payload = bytes(tab.Bytes[start : start + length])

    return Envelope(
        connection_name=connection_name,
        transaction_id=transaction_id,
        source_id=source_id,
        sequence_number=sequence_number,
        timestamp_ns=timestamp_ns,
        payload=payload,
    )
