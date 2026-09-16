# Two-UoP round-trip demo

The full end-system path from the concept sketch: a test-data file flows
into one FACE UoP, across TSS, through a second FACE UoP that transforms
it, and back across TSS to the first UoP, which verifies every value.

```
vectors.txt ──► feeder_uop ──TSS──► processor_uop ──TSS──► feeder_uop ──► check
   (file)       (FACE UoP,          (FACE UoP, dummy       (compares each
                 mock stimulus       transform 3x+7)        value vs file)
                 source)
```

Both UoPs are **generated** by the scaffolder from their descriptors; the
only hand-written code is the USER CODE regions:

| UoP | Region | Does |
|---|---|---|
| `feeder_uop` | `startup` | reads `vectors.txt`, publishes each `(seq, input)` as STIMULUS |
| `feeder_uop` | `on_result` | checks each RESULT against the file, logs it, exits 0 when all N are in |
| `feeder_uop` | `shutdown` | writes a `feeder.shutdown.marker` file (proof the region ran) |
| `processor_uop` | `on_stimulus` | `out = 3*in + 7`, publishes RESULT |
| `processor_uop` | `shutdown` | writes a `processor.shutdown.marker` file (proof the region ran) |

The two descriptors declare their connections in the same order
(`STIMULUS`, `RESULT`) with complementary roles, so one shared
`<base_port>` lines up all four `tcp://127.0.0.1` addresses: each
publisher listens, each subscriber dials (non-blocking, with background
retry — handled inside the TSS transport).

## Run it

```bash
./demo.sh                  # all defaults; picks a free port pair
BASE_PORT=5200 ./demo.sh   # fixed ports instead
```

`demo.sh` generates both UoPs, applies the user-code overlays, regenerates
(proving preservation), builds with `-Werror`, then orchestrates: the
processor starts first and the script waits for its `running` banner; the
feeder then publishes its vectors (after a 2 s slow-joiner guard so the
subscriber's dial completes), collects the results, and shuts itself down
once every vector has round-tripped. The script fails loudly on any
mismatch, missing result, or non-zero exit.

Expected output ends with:

```
ROUNDTRIP PASS: 5/5 vectors file -> feeder_uop -> TSS -> processor_uop -> TSS -> feeder_uop, all values match
```

## Notes

- The `startup` region (new in the generator for this demo) is what makes
  the feeder possible: it runs once in `main()` after the connections are
  open and the subscription callbacks are registered, before the run loop.
  `ctx` is the struct (not a pointer) there; `exit_code = 1; goto cleanup;`
  aborts startup on error.
- The feeder terminates itself with `raise(SIGTERM)` after the last
  result — the generated handler shuts the TSS instance down gracefully.
- The `shutdown` region runs in both UoPs after the run loop exits but
  before teardown: the feeder reaches it via its self-`raise(SIGTERM)`,
  the processor via the orchestrator's `kill -TERM`. Each writes a
  marker file (`feeder.shutdown.marker` / `processor.shutdown.marker`
  under `gen/`); `demo.sh` fails if either is missing. This is the
  lifecycle slot where a real component would offload state to a
  storage UoP while its connections are still open.
- `TSS_ROOT` defaults to the tss repo checkout containing this tool
  (`../..` from the scaffolder root); override it when running from a
  standalone copy of the scaffolder.
- `gen/` holds the generated trees (not committed); `*.log` and
  `feeder_results.txt` there are the run artifacts to inspect.
