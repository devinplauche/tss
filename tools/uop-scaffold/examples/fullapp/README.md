# Full-app demo

The end-system path with a front end and external storage, all wired
through standard UoP interfaces over TSS:

```
vectors.txt ──► feeder_uop ──TSS──► frontend_uop ──TSS──► feeder_uop ──► check
   (file)       (FACE UoP,          (FACE UoP, GUI        (recomputes each
                 mock stimulus       front end: live       expectation from
                 source)             RAM state, scripted   the echoed
                                     GUI actions)          mult/add)
                                          |  ^
                                 STORE_REQ | PUT / STORE_RESP
                                          v  |
                                     storage_uop ──► app_state.txt
                                      (FACE UoP, owns the
                                       backing file)
```

All three UoPs are **generated** by the scaffolder from their
descriptors; the only hand-written code is the USER CODE regions.

## The pieces

| UoP | Role | Hand-written regions |
|---|---|---|
| `feeder_uop` | test-data source + checker | `startup` (reads `vectors.txt`, waits for the frontend's `ready` line, publishes STIMULUS), `on_result` (recomputes `input*mult+add` from the echoed parameters, logs, exits when all N are in), `shutdown` (marker) |
| `frontend_uop` | the processing system / GUI front end | `startup` (reads `gui_actions.txt`, loads `mult`/`add` from the storage UoP), `on_stimulus` (transforms with live RAM state, applies due GUI actions, publishes RESULT echoing the parameters used), `on_store_resp` (adopts persisted state), `shutdown` (offloads state via STORE_PUT, marker) |
| `storage_uop` | external-storage backend | `on_store_req` (reads the backing file, answers), `on_store_put` (writes the backing file), `shutdown` (marker) |

**Persistence is an interface, not file I/O.** The frontend never opens
`app_state.txt` itself: on startup it publishes `STORE_REQ` and waits for
`STORE_RESP`; on shutdown it publishes `STORE_PUT` with the live
parameters. Only the storage UoP touches the backing file (in deployment:
network storage). Swap the backend by swapping the storage UoP — the
frontend doesn't change.

**The GUI is a headless stand-in.** This VM has no display server, so a
real toolkit event loop can't run here. Instead, `gui_actions.txt` lists
scripted "Apply" clicks (`after_seq mult add`); the assignments in
`on_stimulus` that apply them *are* the Apply-button handler — a toolkit
callback would invoke exactly that logic. What the demo proves is the
architecture: user actions mutate RAM state, the next stimulus is
transformed with the new parameters, and the RESULT flows back through
the UoP chain.

## Address plan

One base port `P`; connection index `i` → `tcp://127.0.0.1:(P+i)`:

| Port | Connection | Publisher (listens) | Subscriber (dials) |
|---|---|---|---|
| P+0 | STIMULUS | feeder | frontend |
| P+1 | RESULT | frontend | feeder |
| P+2 | STORE_REQ | frontend | storage |
| P+3 | STORE_RESP | storage | frontend |
| P+4 | STORE_PUT | frontend | storage |

The storage UoP runs with base port `P+2` and declares its connections in
the order `[STORE_REQ, STORE_RESP, STORE_PUT]`, so every address has
exactly one listener and one dialer. `demo.sh` picks 5 consecutive free
ports automatically.

## Run it

```bash
./demo.sh                  # all defaults; picks a free 5-port block
BASE_PORT=5200 ./demo.sh   # fixed ports instead
```

`demo.sh` generates all three UoPs, applies the user-code overlays,
regenerates (proving preservation), builds with `-Werror`, then
orchestrates: storage first, frontend second, feeder last. Before
starting, it seeds `app_state.txt` with `7 3` — the frontend must load
exactly those parameters (load proof), and after the scripted GUI actions
(`2 → 5 1`, `4 → 9 -3`) its shutdown offload must leave `9 -3` in the
file (offload proof). The script fails loudly on any mismatch, missing
result, missing shutdown marker, or wrong storage content.

Expected output ends with:

```
FULLAPP PASS: 5/5 vectors file -> feeder_uop -> TSS -> frontend_uop (+GUI actions, +storage load/offload) -> TSS -> feeder_uop, all values match
```

## Notes

- The feeder's `startup` polls the frontend's log for the `ready` line
  (passed via `FRONTEND_LOG`) before publishing: no stimulus may be
  transformed with uninitialized RAM state. Without `FRONTEND_LOG` it
  falls back to a fixed delay.
- The frontend's `STORE_REQ` uses a retry loop: nng pub/sub drops messages
  published before the storage side's dial completes (slow-joiner), the
  same reason the feeder sleeps before publishing stimuli.
- The frontend's `shutdown` sleeps 1 s after publishing `STORE_PUT` so the
  message flushes before teardown, then writes its marker.
- `TSS_ROOT` defaults to the tss repo checkout containing this tool
  (`../..` from the scaffolder root); override it when running from a
  standalone copy of the scaffolder.
- `gen/` holds the generated trees (not committed); `*.log`,
  `feeder_results.txt`, `app_state.txt`, and `*.shutdown.marker` there
  are the run artifacts to inspect.
