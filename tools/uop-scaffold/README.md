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
python3 -m scaffold.gen_uop examples/sensor_uop.yaml --out /tmp/sensor_gen
```

Edit the generated `<uop>.c` where marked:

```c
/* USER CODE BEGIN: on_raw_detection */
...
/* USER CODE END: on_raw_detection */
```

and the harness's `drive` region. Re-running the generator preserves USER
CODE regions byte-for-byte. Then:

```bash
cmake -S /tmp/sensor_gen -B /tmp/sensor_gen/build -DTSS_ROOT=/path/to/tss
cmake --build /tmp/sensor_gen/build
cd /tmp/sensor_gen/build && ctest --output-on-failure
```

`sensor_uop_loopback` spawns the generated UoP, drives its inputs through
the harness's mirror-image connections, checks the outputs, SIGTERMs the
child, and requires a clean exit.

## Layout

- `scaffold/ysubset.py` — deliberately restricted YAML-subset parser
  (mappings, lists, scalars; rejects tabs, flow syntax, duplicate keys).
- `scaffold/model.py` — descriptor model + validation (C identifiers,
  type references, publisher→source / subscriber→destination consistency).
- `scaffold/types_emit.py` — C99 scalar message structs with shift-based
  little-endian encode/decode; exact-length decode (no flatcc runtime).
- `scaffold/gen_uop.py` — UoP skeleton generator (`<uop>.c`,
  `<uop>_types.h`, `CMakeLists.txt`).
- `scaffold/gen_harness.py` — mirrored loopback harness generator
  (`<uop>_harness.c`) plus `enable_testing()` / `add_test()` wiring.
- `scaffold/util.py` — shared region-rendering helpers.
- `tests/` — pytest suite (41 tests: parser, model, codecs, generators,
  `-Werror` compile checks).
- `examples/sensor_uop.yaml` — the sensor fusion example descriptor.

## Design notes

- Scalar-only message types stay dependency-free on purpose. A future
  `schema:` descriptor key is the seam for delegating richer types
  (strings, nested tables, unions) to `tools/face_tss_codegen.py`.
- Publish helpers are non-static so `-Wunused-function` stays quiet
  before USER CODE calls them; the default harness `drive` region fails
  loudly so an unimplemented harness can never report a false PASS.

## Trademark note

"FACE" is a trademark of The Open Group. This tool's name deliberately
omits it; the phrase "targeting FACE Technical Standard 3.2 TSS
interfaces" is descriptive use only. No endorsement, conformance, or
certification is claimed.
