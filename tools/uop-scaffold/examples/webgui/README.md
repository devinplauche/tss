# Web-GUI demo — a real browser front end on the TSS fabric

```
vectors.txt -> feeder_uop --TSS--> webgui bridge --HTTP--> browser GUI
                                 <--TSS--   (Apply button -> RAM state)
                                     |  ^
                            STORE_REQ | PUT / STORE_RESP
                                     v  |
                              storage_uop -> app_state.txt
```

This is the `fullapp` demo with the scripted C frontend replaced by a
**real browser GUI**. The bridge (`webgui/bridge.py`) *is* the frontend
UoP on the TSS fabric — the same five connections and wire formats as
the C frontend — and it additionally serves a GUI page over HTTP. The
browser is purely view + controller:

- live parameters (`mult`/`add`) live in the bridge's RAM;
- the **Apply** button POSTs new values, which take effect immediately;
- each stimulus is transformed with the current parameters and the
  result (echoing the parameters used) publishes back over TSS;
- the event log streams to the page over server-sent events.

Persistence still goes through the **storage UoP** over TSS: the bridge
loads `mult`/`add` via `STORE_REQ`/`STORE_RESP` on startup and offloads
via `STORE_PUT` on shutdown. The bridge never touches the state file
directly — only `storage_uop` does.

## The bridge is hand-written, not generated

The scaffolder generates C. The bridge is hand-written Python (using the
`tss` Python bindings) the same way USER CODE regions are hand-written:
it is the application layer on top of the generated-style UoP contract.
Its TSS footprint — connection names, directions, addresses, and every
wire format — matches the descriptor contract in `fullapp`, so the C
feeder and storage UoPs interop with it unchanged.

## Running it

```sh
./demo.sh                    # all defaults
BASE_PORT=5200 TIMEOUT=150 ./demo.sh
```

`demo.sh` generates + builds the C feeder and storage UoPs (`-Werror`),
seeds external storage with `7 3`, starts storage, the bridge, and the
feeder (which publishes one vector every 4 seconds so there is time to
interact), then runs the Playwright suite, then verifies:

- the bridge loaded the seed (`ready (mult=7 add=3, loaded from storage)`);
- all 5 vectors round-tripped with **zero mismatches** (the feeder
  recomputes expectations from the parameters each result echoes);
- shutdown markers exist for all three processes;
- storage holds `9 -3` after the shutdown offload (the GUI's last Apply).

## The Playwright tests

`tests/test_webgui.py` drives a real Chromium against the live page:

1. the GUI shows the storage-loaded state (`mult=7 add=3`);
2. a first result arrives transformed with the seed parameters;
3. clicking **Apply** with `5`/`1` updates the live state panel;
4. the next result echoes `(mult=5 add=1)` — the click travelled
   browser → bridge RAM → TSS `RESULT`;
5. a second Apply (`9`/`-3`) flows back the same way.

They need a Chromium binary: `CHROME_PATH` (default
`/opt/meta-chromium/chrome`) and `GUI_URL` (default
`http://127.0.0.1:18090/`).

## Layout

- `feeder_uop.yaml`, `storage_uop.yaml` — same descriptors as `fullapp`
- `user_code/feeder/` — feeder logic; the startup publishes slowly
  (`FEEDER_DELAY_SEC`, default 4) and gates on `webgui: ready`; the
  result callback only ends the run after startup completes (results
  can arrive mid-publish)
- `user_code/storage/` — same storage logic as `fullapp`
- `webgui/bridge.py` — the TSS frontend + HTTP/SSE server
- `webgui/static/index.html` — the GUI page
- `tests/test_webgui.py` — Playwright end-to-end tests
- `vectors.txt`, `seed_state.txt` — stimulus vectors, storage seed
