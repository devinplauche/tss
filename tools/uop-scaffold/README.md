# uop-scaffold

Developer tooling that scaffolds a FACE Technical Standard 3.2 **Unit of
Portability (UoP)** from a small YAML descriptor: a C source skeleton wired
to the TSS C API, zero-dependency scalar message codecs, and a mirrored
loopback harness registered with CTest.

This is **pre-verification scaffolding and developer tooling**. It is not
a conformance claim, and it is not FACE certification; formal verification
is left to an approved Verification Authority.

## Quick start

```bash
cd tools/uop-scaffold
python3 -m scaffold.gen_uop examples/sensor/sensor_uop.yaml --out /tmp/sensor_gen
```

Fill in the USER CODE regions in the generated `<uop>.c` (application
logic) and `<uop>_harness.c` (the `drive` test), then:

```bash
cmake -S /tmp/sensor_gen -B /tmp/sensor_gen/build -DTSS_ROOT=/path/to/tss
cmake --build /tmp/sensor_gen/build
cd /tmp/sensor_gen/build && ctest --output-on-failure
```

Or run the whole pipeline at once — see `examples/sensor/demo.sh`.

## The descriptor

```yaml
uop:
  name: sensor_uop            # C identifier; names the files and binaries
  types:                      # scalar message types (structs of scalars)
    - name: raw_detection
      fields:
        - name: x
          type: int32
        - name: y
          type: int32
    - name: FusedTrack        # IDL-defined message type (see below)
      idl: sensor.idl
      idl_type: SensorMsgs::FusedTrack   # optional; required if ambiguous
  connections:                # one TSS connection each
    - name: RAW_DETECTION
      type: raw_detection
      role: subscriber        # subscriber | publisher
      direction: destination  # subscriber->destination, publisher->source
      callback: on_raw_detection   # required for subscribers, banned for publishers
```

Rules, enforced with line-numbered errors:

- Names are C identifiers; connection names must additionally be unique
  case-insensitively (they become lowercase C symbols).
- `role: subscriber` requires `direction: destination` and a `callback`;
  `role: publisher` requires `direction: source` and forbids `callback`.
- Scalar field types: `int8/16/32/64`, `uint8/16/32/64`, `float`,
  `double`, `bool`. Every connection's `type` must name a type in
  `types:`.
- The parser accepts a deliberately small YAML subset (mappings, lists,
  bare/integer/quoted scalars, comments). Tabs, flow `{...}`/`[...]`
  syntax, anchors, and duplicate keys are rejected.

## IDL-defined types

A type can come from OMG IDL instead of inline scalar fields:

```yaml
    - name: FusedTrack
      idl: sensor.idl              # path relative to the descriptor
      idl_type: SensorMsgs::FusedTrack   # optional qualifier
```

The scaffolder parses a restricted IDL subset (modules, structs, enums,
unions, typedefs, sequences, constants, `#include`), lowers it to a
FlatBuffers schema, and reuses `tools/face_tss_codegen.py` for the C
codec (`FusedTrack_serialize` / `FusedTrack_deserialize` /
`FusedTrack_fini`). Scalar and IDL types coexist freely in one
descriptor; the generated CMake links the FlatBuffers runtime only when
an IDL type is present.

MVP limitations, all rejected loudly at load time (never silently
dropped):

- Bounded `string<N>`, bounded `sequence<T,N>`, and fixed `typedef T[N]`
  arrays are rejected (bounds enforcement is future work).
- IDL `union` members must be structs, `string`, or integral scalars.
  `boolean`/`float`/`double` members are rejected (their lowered names
  would not compile as C field names), as are enum and sequence members.
- One message type per IDL file (each file's codec is generated
  independently).
- IDL type names must not collide with scalar type names or with C
  keywords.

Ownership: strings, nested structs, and vectors in an IDL message are
heap-owned. Allocate them with `malloc`/`strdup` (never point them at
stack memory) and release with `<T>_fini` when done. The generated
publish path serializes, sends, and frees; the callback path
deserializes, runs your USER CODE, then finalizes — do not return early
from a subscriber callback before cleanup.

## USER CODE regions

Every hand-editable spot in the generated code is a region:

```c
/* USER CODE BEGIN: on_raw_detection */
...your code here...
/* USER CODE END: on_raw_detection */
```

- Region names: `ctx_fields` (extra context struct fields), `startup`
  (one-shot logic in `main()` after the connections are open and the
  subscription callbacks are registered, before the run loop — e.g.
  publish stimulus or read a config file; `ctx` is the struct, not a
  pointer, there, and `exit_code = 1; goto cleanup;` aborts startup),
  plus one per subscriber callback, named by the descriptor's `callback:`
  value. The harness has one region: `drive`.
- Regeneration extracts region bodies from the existing files and
  re-inserts them **verbatim** — only the marker lines are re-indented,
  so repeated regeneration is byte-stable (no indentation creep).
- Regions that no longer exist (e.g. a callback you removed from the
  descriptor) are reported as warnings, not silently kept or dropped.

## The generated tree

```
<out>/
  <uop>.c               # TSS lifecycle, connections, callbacks, run loop
  <uop>_types.h         # message structs + encode/decode
  <uop>_harness.c       # loopback rig (see below)
  CMakeLists.txt        # C99, -Wall -Wextra -Werror, TSS_ROOT, CTest
```

`<uop>.c` wires each connection from a base port
(`tcp://127.0.0.1:<base_port + index>`), registers one callback per
subscriber with exact-length decoding, exposes `publish_<CONN>` helpers,
runs until SIGTERM/SIGINT, and tears down in reverse order. It prints
`rx`/`tx`/`err` counters on shutdown.

`<uop>_harness.c` takes the **mirror image** of the descriptor: your UoP's
subscribers become its publishers (`send_<CONN>` helpers) and vice versa
(`recv_<CONN>` helpers). Usage:

```
<uop>_harness <path_to_uop> <base_port>
```

It spawns the UoP, runs your `drive` region, SIGTERMs the child, and
requires a clean exit. The default `drive` region fails loudly rather
than silently passing, so an unimplemented harness can never report a
false PASS. CMake registers it as `ctest` test `<uop>_loopback`.

## Regeneration workflow

1. Edit the descriptor (new connection, new field, ...).
2. Re-run the generator over the same `--out` directory.
3. Your USER CODE is preserved; new regions appear with TODO defaults.
4. Rebuild, re-run `ctest`.

## Troubleshooting

- `descriptor error (line N): ...` — the validator tells you exactly what
  and where; fix the YAML and re-run.
- `warning: orphaned USER CODE region 'X'` — the descriptor no longer
  emits region X (renamed/removed callback). Move the code or delete it.
- `FAIL: drive region not implemented` — you generated the harness but
  haven't written the `drive` test yet.
- Harness reports `TIMED_OUT` — the UoP side likely isn't publishing, or
  the two processes disagree on the base port.
- `ctest` fails with address-in-use errors — the loopback base port
  (default 48601, set in the generated `CMakeLists.txt`) is taken; stop
  the other process or change the port and rebuild.

## Layout

- `scaffold/ysubset.py` — restricted YAML-subset parser.
- `scaffold/model.py` — descriptor model + validation.
- `scaffold/types_emit.py` — C99 scalar codecs.
- `scaffold/idl_parse.py` — OMG IDL subset parser.
- `scaffold/idl_to_fbs.py` — IDL AST to FlatBuffers schema lowering.
- `scaffold/idl_emit.py` — IDL codec generation via face_tss_codegen.py.
- `scaffold/gen_uop.py` — UoP + CMake generator (`generate_tree()`).
- `scaffold/gen_harness.py` — harness + CTest generator.
- `scaffold/util.py` — shared region helpers.
- `tests/` — pytest suite (104 tests).
- `examples/sensor/` — the worked demo (`demo.sh`).

## Trademark note

"FACE" is a trademark of The Open Group. This tool's name deliberately
omits it; the phrase "targeting FACE Technical Standard 3.2 TSS
interfaces" is descriptive use only. No endorsement, conformance, or
certification is claimed.
