# faceTSS - FACE Transport Services Segment over nng + FlatBuffers

A C implementation of the [FACE (Future Airborne Capability Environment)](https://www.opengroup.org/face)
**Transport Services Segment (TSS)** interface, with data movement provided by
[**nng**](https://nng.nanomsg.org/) and message framing provided by
[**FlatBuffers**](https://flatbuffers.dev/) (via the [flatcc](https://github.com/dvidelabs/flatcc) C runtime).

Implements the FACE TS interface shape - `Initialize` / `Create_Connection` /
`Destroy_Connection` / `Send_Message` / `Receive_Message` / `Register_Callback` /
`Unregister_Callback` (see `c/include/face_tss/tss.h`) - on top of two nng patterns:

| Transport | nng pattern | Use for |
|-----------|-------------|---------|
| `pubsub`  | Pub0/Sub0 fan-out, topic = `CONNECTION_NAME + \0` | telemetry fan-out, many readers |
| `bus`     | Bus0 mesh, raw envelopes | small peer groups, bidirectional command nets |

Every message on the wire is a FlatBuffers `TssEnvelope`
(`c/schemas/tss_envelope.fbs`): connection name, transaction id, source id,
per-connection sequence number, send timestamp, plus the **opaque typed payload**
built from the application's own schema. The TSS never interprets payload bytes.

A Python implementation of the same design lives in `src/face_tss/` (see below).

## Build: three .so files

The CMake build produces exactly three shared libraries:

| File | What |
|------|------|
| `libTSS.so` | the FACE TSS (`face_tss_*` C API) |
| `libnng.so` | nng v1.12.3 (vendored via FetchContent, shared build) |
| `libflatccrt.so` | flatcc C runtime v0.6.3 (builder/verifier, RTONLY) |

Prerequisites: Linux, CMake >= 3.16, Ninja (or Make), gcc, network access to
fetch nng + flatcc (or `-DFACETSS_USE_SYSTEM_NNG=ON` / `-DFACETSS_USE_SYSTEM_FLATCC=ON`
to link system copies instead).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build          # envelope, config, lifecycle, live loopback
ls build/*.so*                  # libTSS.so libnng.so libflatccrt.so
```

nng carries a versioned soname, so the build tree holds `libnng.so.1.12.3`
with `libnng.so.1` / `libnng.so` symlinks (same for `libTSS.so.0`).

Useful options: `-DFACETSS_BUILD_TESTS=OFF`, `-DFACETSS_BUILD_EXAMPLES=OFF`,
`-DFACETSS_ENABLE_WERROR=ON`.

## Layout

```
c/include/face_tss/   public C API
  types.h             FACE primitives: directions, return codes, header, timeouts
  config.h            connection configuration (JSON file / programmatic)
  envelope.h          FlatBuffers TSS-envelope codec (flatcc runtime)
  transport.h         nng PubSubTransport / BusTransport (+ callbacks)
  tss.h               FaceTss: the FACE TS interface
c/src/                implementation (config, envelope, transport, tss)
c/schemas/            .fbs sources (envelope wire format + example app type)
c/tests/              CTest suite (codec, config, lifecycle, live loopback)
c/examples/           face_tss_pubsub demo (publisher streams, subscriber prints)
configs/              example node configs (publisher / subscriber / bus)
```

## Quick start (C)

```c
#include "face_tss/tss.h"

FACE_TSS_CONFIG cfg;
FACE_TSS *pub, *sub;
FACE_TSS_CONNECTION_ID_TYPE tx, rx;
FACE_TSS_MESSAGE_SIZE_TYPE mx;
FACE_TSS_MESSAGE m;

face_tss_config_from_file("configs/pubsub_publisher.json", &cfg);
pub = face_tss_create("tx");
face_tss_initialize(pub, &cfg);
face_tss_create_connection(pub, "POSITION", &tx, &mx, 0);

face_tss_send_message(pub, tx, (const uint8_t *)"hello", 5, 1);
/* ... on the subscriber (role=subscriber, same address): */
face_tss_receive_message(sub, rx, 5000000000LL, 0, &m);
face_tss_message_fini(&m);
```

FACE timeouts are int64 nanoseconds; `FACE_TSS_TIMEOUT_INFINITE` (-1) blocks
forever, `0` polls. A receive that times out returns `FACE_TSS_RC_TIMED_OUT`;
sends on a `DESTINATION`-only connection return `FACE_TSS_RC_INVALID_MODE`;
oversize payloads return `FACE_TSS_RC_BUFFER_TOO_SMALL`.

## Demos (C)

Two processes, publisher first (it owns the listen side):

```sh
./build/c/examples/face_tss_pubsub pub configs/pubsub_publisher.json
./build/c/examples/face_tss_pubsub sub configs/pubsub_subscriber.json
```

## Tests

```sh
ctest --test-dir build --output-on-failure   # C: envelope, config, lifecycle, live
python -m pytest tests/ -q                   # Python mirror implementation
```

`c/tests/test_live.c` moves real bytes over loopback nng sockets
(round-trip with header/sequence validation, timeouts, topic isolation,
bus exchange, oversize rejection, callback delivery).

## Python mirror (`src/face_tss/`)

The same design exists as a Python package (`pynng` + `flatbuffers`,
no `flatc` step): `types.py`, `errors.py`, `config.py`, `envelope.py`,
`transport.py`, `tss.py`, `broker.py`, `typed.py` (+ `PositionReport`
example type). See `examples/` for `pubsub_demo.py`, `bus_demo.py`,
`callback_demo.py`, and install with `pip install -e .` / `pynng flatbuffers`.

## Design notes

- **One FACE connection = one nng socket.** Connection names are uppercased per
  the FACE case-insensitive rule and double as the Pub/Sub topic
  (`NAME + \0`; the NUL keeps `HELLO` from matching `HELLO2`).
- **Subscribers dial non-blocking**, so start order never matters; publishers
  listen. Exactly one listener per address (second listener gets `AddressInUse`).
- **Bus0 is a mesh without topic filtering** - every peer hears every peer, and
  a socket never receives its own sends.
- The envelope codec uses the flatcc `Builder` / raw-layout reads directly
  (no generated code); app types ride opaquely in `TssEnvelope.payload`.
- `BusTransport` listen falls back to dial on `AddressInUse`, so a bus node
  can start before or after the anchor.
