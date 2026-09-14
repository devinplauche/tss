# faceTSS - FACE Transport Services Segment over nng + FlatBuffers

A Python implementation of the [FACE (Future Airborne Capability Environment)](https://www.opengroup.org/face)
**Transport Services Segment (TSS)** interface, with data movement provided by
[**nng**](https://nng.nanomsg.org/) (via `pynng`) and message framing provided by
[**FlatBuffers**](https://flatbuffers.dev/) (via the `flatbuffers` runtime, no `flatc` step).

Implements the FACE TS interface shape - `Initialize` / `Create_Connection` /
`Destroy_Connection` / `Send_Message` / `Receive_Message` / `Register_Callback` /
`Unregister_Callback` - on top of two nng patterns:

| Transport | nng pattern | Use for |
|-----------|-------------|---------|
| `pubsub`  | Pub0/Sub0 fan-out, topic = `CONNECTION_NAME + \0` | telemetry fan-out, many readers |
| `bus`     | Bus0 mesh, raw envelopes | small peer groups, bidirectional command nets |

Every message on the wire is a FlatBuffers `TssEnvelope`
(`schemas/tss_envelope.fbs`): connection name, transaction id, source id,
per-connection sequence number, send timestamp, plus the **opaque typed payload**
built from the application's own schema. The TSS never interprets payload bytes.

## Layout

```
src/face_tss/      the package
  types.py         FACE primitives: Direction, ReturnCode, Header, timeouts
  errors.py        one exception per FACE return-code failure
  config.py        connection configuration (JSON/TOML/dict/builder)
  envelope.py      FlatBuffers TSS-envelope codec (hand-rolled, no flatc)
  transport.py     nng PubSubTransport / BusTransport (+ callbacks)
  tss.py           FaceTss: the FACE TS interface
  broker.py        mesh anchor for bus demos (python -m face_tss.broker)
  typed.py         TypedMessage pattern + PositionReport example type
schemas/           .fbs sources (envelope wire format + example app type)
configs/           example node configs (publisher / subscriber / bus)
examples/          runnable demos (pubsub, bus, callback)
tests/             pytest suite (codec, config, lifecycle, live loopback)
```

## Requirements

- Python 3.10+
- `pynng`, `flatbuffers` (`pip install pynng flatbuffers`), `pytest` for tests

## Quick start

```python
from face_tss import FaceTss, TssConfigBuilder, Direction, PositionReport

pub_cfg = (TssConfigBuilder()
           .add("POSITION", direction="BI_DIRECTIONAL", transport="pubsub",
                role="publisher", address="tcp://127.0.0.1:5561")
           .build())
sub_cfg = (TssConfigBuilder()
           .add("POSITION", direction="BI_DIRECTIONAL", transport="pubsub",
                role="subscriber", address="tcp://127.0.0.1:5561")
           .build())

pub, sub = FaceTss("tx"), FaceTss("rx")
pub.initialize(pub_cfg)
sub.initialize(sub_cfg)
tx_id, _ = pub.create_connection("position")   # names match case-insensitively
rx_id, _ = sub.create_connection("POSITION")

pub.send_message(tx_id, PositionReport("N123", 37.5, -122.25, 1500, 270, True).serialize(),
                 transaction_id=1)
msg = sub.receive_message(rx_id, timeout_ns=5_000_000_000)
print(PositionReport.deserialize(msg.payload), msg.header.sequence_number)
```

FACE timeouts are int64 nanoseconds; `TIMEOUT_INFINITE` (-1) blocks forever,
`0` polls. A receive that times out raises `TimedOutError` (FACE `TIMED_OUT`);
sends on a `DESTINATION`-only connection raise `InvalidModeError`
(FACE `INVALID_MODE`); oversize payloads raise `BufferTooSmallError`.

## Demos

Two processes, publisher first (it owns the listen side):

```
python examples/pubsub_demo.py pub
python examples/pubsub_demo.py sub
```

Bus mesh (broker anchors the mesh, then two peers):

```
python -m face_tss.broker configs/bus_demo.json
python examples/bus_demo.py A
python examples/bus_demo.py B
```

Callback (push instead of poll) - needs the publisher running:

```
python examples/callback_demo.py
```

## Tests

```
python -m pytest tests/ -q
```

`test_transport_live.py` moves real bytes over loopback nng sockets
(round-trip, timeouts, topic isolation, bus exchange, oversize rejection,
callback delivery).

## Design notes

- **One FACE connection = one nng socket.** Connection names are uppercased per
  the FACE case-insensitive rule and double as the Pub/Sub topic
  (`NAME + \0`; the NUL keeps `HELLO` from matching `HELLO2`).
- **Subscribers dial non-blocking**, so start order never matters; publishers
  listen. Exactly one listener per address (second listener gets `AddressInUse`).
- **Bus0 is a mesh without topic filtering** - every peer hears every peer, and
  a socket never receives its own sends. One process must listen per address
  (see `broker.py`); the rest dial.
- `flatc` is intentionally not required: the envelope codec uses `Builder` /
  `Table` directly, and app types plug in as opaque bytes (see `typed.py`).
- `BusTransport.open()` listens and falls back to dial on `AddressInUse`, so a
  bus node can start before or after the anchor.
