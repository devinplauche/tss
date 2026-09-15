# Sensor example — the worked demo

A tiny fusion UoP: subscribes to `RAW_DETECTION` (x/y coordinates),
assigns incrementing track IDs, publishes `FUSED_TRACK`. The generated
loopback harness drives five synthetic detections and checks all five
fused tracks come back with matching coordinates and `track_id == i + 1`.

## Run it

```bash
./demo.sh
```

That single command: generates the tree from `sensor_uop.yaml`, applies
the hand-written logic in `user_code/`, regenerates (proving USER CODE
regions survive), builds with `-Wall -Wextra -Werror`, and runs `ctest`
— the generated harness testing the generated UoP.

`TSS_ROOT` defaults to the tss checkout containing this tool; override it
to point at another build:

```bash
TSS_ROOT=/path/to/tss ./demo.sh /tmp/sensor-gen
```

## Files

- `sensor_uop.yaml` — the component descriptor (the only input).
- `user_code/uop.ctx_fields.c` — context field added by the developer
  (`next_track_id`).
- `user_code/uop.on_raw_detection.c` — the actual application logic:
  copy x/y, assign the next track ID, publish.
- `user_code/harness.drive.c` — the harness test: five detections in,
  five verified fused tracks out.
- `apply_user_code.py` — demo glue that pastes each overlay into its
  USER CODE region, exactly as a developer would by hand.
- `demo.sh` — the one-command pipeline described above.

In real use there is no `user_code/` directory: you edit the regions
directly in the generated `<uop>.c` / `<uop>_harness.c`. The overlays
exist so the demo is reproducible without an editor.
