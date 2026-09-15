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
        - { name: x, type: int32 }
        - { name: y, type: int32 }
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
- Field types are scalars: `int8/16/32/64`, `uint8/16/32/64`, `float`,
  `double`, `bool`, `char`. Every connection's `type` must name a type in
  `types:`.
- The parser accepts a deliberately small YAML subset (mappings, lists,
  bare/integer/quoted scalars, comments). Tabs, flow `{...}`/`[...]`
  syntax, anchors, and duplicate keys are rejected.

Scalar-only types stay dependency-free on purpose: the codecs are
shift-based little-endian encode/decode with exact-length checks, no
FlatBuffers runtime. A future `schema:` key on a type is the designed
seam for delegating richer types (strings, nested tables, unions) to
`tools/face_tss_codegen.py`.

## USER CODE regions

Every hand-editable spot in the generated code is a region:

```c
/* USER CODE BEGIN: on_raw_detection */
...your code here...
/* USER CODE END: on_raw_detection */
```

- Region names: `ctx_fields` (extra context struct fields) plus one per
  subscriber callback, named by the descriptor's `callback:` value.
  The harness has one region: `drive`.
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

## Layout

- `scaffold/ysubset.py` — restricted YAML-subset parser.
- `scaffold/model.py` — descriptor model + validation.
- `scaffold/types_emit.py` — C99 scalar codecs.
- `scaffold/gen_uop.py` — UoP + CMake generator (`generate_tree()`).
- `scaffold/gen_harness.py` — harness + CTest generator.
- `scaffold/util.py` — shared region helpers.
- `tests/` — pytest suite (41 tests).
- `examples/sensor/` — the worked demo (`demo.sh`).

## Trademark note

"FACE" is a trademark of The Open Group. This tool's name deliberately
omits it; the phrase "targeting FACE Technical Standard 3.2 TSS
interfaces" is descriptive use only. No endorsement, conformance, or
certification is claimed.
