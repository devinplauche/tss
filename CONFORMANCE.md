# FACE Conformance Tracker

Branch: `face-conformance`. This document tracks how closely the TSS
implementation matches the FACE Technical Standard v3.1 Transport Services
interface, what was changed, and what remains.

> **Certification status: NOT CERTIFIED.** Interface-shape alignment is not
> conformance. Formal FACE conformance requires the FACE Conformance Test
> Suite (CTS) and certification process, which this project has not run.

## What was aligned (this branch)

### Primitive types (`types.h` / `src/face_tss/types.py`)
- Full 14-value FACE 3.1 `RETURN_CODE_TYPE` enumeration, in standard order:
  `NO_ERROR, NO_ACTION, NOT_AVAILABLE, INVALID_PARAM, INVALID_CONFIG,
  INVALID_MODE, TIMED_OUT, ADDR_IN_USE, PERMISSION_DENIED, MESSAGE_STALE,
  IN_PROGRESS, CONNECTION_CLOSED, DATA_BUFFER_TOO_SMALL, DATA_OVERFLOW`.
- Renamed `BUFFER_TOO_SMALL` -> `DATA_BUFFER_TOO_SMALL` (standard name).
- `HEADER_TYPE` projection: `instance_uid` / `source_uid` / `timestamp`.
- `QoS_EVENT_TYPE` projection: fixed-capacity (8) list of QoS elements.
  Currently always reported empty.
- `MESSAGE_GUID_TYPE` (uint64) with `MESSAGE_GUID_INVALID = 0`.

### Operation signatures (`tss.h` / `src/face_tss/tss.py`)
- `Send_Message` now takes a **timeout** (ns, `TIMEOUT_INFINITE` = -1 blocks
  forever, 0 polls); threaded to the nng socket send timeout.
- Transaction ID is **in/out**: pass `TRANSACTION_ID_UNSPECIFIED` (0) and the
  TSS assigns a per-instance ID; the ID used is returned/written back.
- `Receive_Message` writes out the **transaction ID** and a **QoS event**.
- Callback signature now receives **connection ID, transaction ID, message
  GUID, payload, standard header, QoS event, and user context**, and returns
  a **callback return code** (FACE `CALLBACK_RETURN_CODE_TYPE`).
- nng address-in-use now maps to `ADDR_IN_USE` instead of a generic error.
- Oversize sends map to `DATA_BUFFER_TOO_SMALL` (was `BUFFER_TOO_SMALL`).

### Wire envelope (`tss_envelope.fbs`, codec, both languages)
- Added field 6 `message_guid` (application type identity) and field 7
  `instance_uid` (unique per send, per TSS instance). Fields 0-5 unchanged,
  so old field IDs are stable but old binaries cannot parse the new fields.

### Typed interface (`typed.h` / `typed.c`, `tools/face_tss_codegen.py`)
- `TypedTS<DATATYPE>` pattern: `FACE_TSS_TYPE_SUPPORT` descriptor
  (message GUID + qualified type name + serializer/deserializer/finalizer),
  registration/lookup, typed send/receive with wire GUID checking, typed
  callback bridge.
- Code generator: flat FlatBuffers `.fbs` table (scalar + string fields) ->
  C value struct + codec + descriptor. Deterministic 63-bit FNV-1a message
  GUID from the qualified type name.
- Generated + checked in: `c/generated/positionreport_typed.*`
  (`FaceTSS.PositionReport`, GUID `7542349876525629205`).
- New CTest suite `c-typed` and `face_tss_typed_pubsub` demo.

### Python mirror
- Same signature/header/return-code/envelope alignment as the C API
  (`src/face_tss/` + `tests/` + `examples/`).

## Verification (this branch, 2026-09-14)
- CMake build: clean (only pre-existing flatcc sign-compare notes).
- CTest: 5/5 suites pass (envelope, config, lifecycle, live, typed).
- Python: 36/36 tests pass.
- C untyped pub/sub across processes: late subscriber 17/20 (first 3 missed
  before dial completed - normal pub/sub behavior).
- C typed pub/sub across processes: 17/20, all struct fields decoded.
- C++17 proof vs new headers: 10/10, FACE header intact, `-Wall -Wextra` clean.
- Python pub/sub across processes: 16/20 with new header fields.

## Partially implemented / known divergences
- **Receive buffer semantics**: FACE `Receive_Message` takes a caller-owned
  data buffer + `DATA_BUFFER_TOO_SMALL` when it doesn't fit. This
  implementation still returns an allocated payload and raises/maps the code
  only on the `min_message_size` / `max_message_size` checks.
- **QoS**: the event carries one honest element per message
  (`message_age_ns`) on receive and callback paths. No QoS policy
  management, no staleness enforcement, no `MESSAGE_STALE` production —
  unsupported FACE QoS guarantees are documented, not emulated.
- **Receive buffers**: caller-owned receive is `face_tss_receive_message_into`
  (C) / `receive_into` (Python) with full `DATA_BUFFER_TOO_SMALL` + required
  size semantics. The allocating `receive_message` remains as a documented
  convenience extension.
- **Configuration**: `Initialize` takes a config object, not a FACE
  `CONFIGURATION_RESOURCE`, and there is no Configuration interface /
  `Set_Reference`.
- **Connection scope**: `Create_Connection` takes a bare name; no
  `CONNECTION_ID` typedef plumbing beyond the integer ID.
- **Codegen limits**: unions, vectors of tables/strings, nested vectors,
  and explicit field IDs are still rejected. Supported: multiple tables per
  file, nested tables, scalar vectors, enums with explicit integral base.
- **Late subscriber**: a subscriber started before its publisher misses
  early messages (verified pre-existing in the original code; README notes
  the publisher-first ordering requirement).
- **Threading**: callback stop/destroy locking follows the original design;
  not audited against FACE threading requirements.

## Remaining gaps (not started)
- FACE Configuration interface + `CONFIGURATION_RESOURCE` handling.
- Caller-owned receive buffers (full `DATA_BUFFER_TOO_SMALL` semantics).
- Real QoS management (policies, events, staleness).
- TSS distribution / multi-instance discovery beyond static config.
- Type abstraction beyond the codegen subset.
- TPM support.
- Any safety/security certification artifacts.
- The FACE CTS itself.

## Transport note
nng (pub/sub, bus) is the underlying transport. FACE permits multiple
underlying transports; nng is not itself a conformance blocker, but its
delivery semantics (best-effort pub/sub, no persistence) bound what QoS and
reliability claims this TSS can ever make.
