# FACE Conformance Tracker

This document tracks how closely the TSS implementation matches the FACE
Technical Standard Transport Services interface, what was changed, and
what remains.

> **Certification status: NOT CERTIFIED.** Interface-shape alignment is not
> conformance. Formal FACE conformance requires the FACE Conformance Test
> Suite (CTS) and certification process, which this project has not run.

## Target edition: FACE 3.2

The implementation targets the FACE Technical Standard **Edition 3.2**
Transport Services interfaces (verified against the IDL shipped with CTS
3.2.3, `FACEConformanceTestSuite_3.2.3/datafiles/IDL/FACE_3.x/`).

### 3.1 -> 3.2 delta (implemented)
- **New return code** `RESOURCE_LIMIT_REACHED` (value 14): the 15th
  `RETURN_CODE_TYPE` enumerator. Produced by `Create_Connection` when
  `FACE_TSS_MAX_CONNECTIONS` (64) connections are already open; destroying
  a connection frees a slot. (C: `FACE_TSS_RC_RESOURCE_LIMIT_REACHED`,
  `face_tss_rc_str`; Python: `ReturnCode.RESOURCE_LIMIT_REACHED`,
  `ResourceLimitError`.)
- **`Unregister_Callback` moved to TypedTS**: FACE 3.1 had it on `Base`;
  3.2's `TypedTS.idl` carries it on the `TypedTS` interface. New C API
  `face_tss_typed_unregister_callback(tss, connection_id, type_name)`
  (`typed.h`); the old `face_tss_unregister_callback` is kept as a
  compatibility alias. Python's single `unregister_callback` covers both.
- **New `TSS/Common.idl` constants**: `FACE_TSS_TID_NOT_APPLICABLE` (-1),
  `FACE_TSS_CALLEE_PROVIDES_TID` (0), `FACE_TSS_CALLEE_PROVIDES_GUID` (0)
  (Python: `TID_NOT_APPLICABLE`, `CALLEE_PROVIDES_TID`,
  `CALLEE_PROVIDES_GUID`).
- Verified no-change in 3.2: `Send_Message` / `Receive_Message` /
  `Register_Callback` / `Callback_Handler` signatures, `HEADER_TYPE`,
  `QoS_EVENT_TYPE`, Configuration interface operations.
- Not adopted: the `FACE::Logging` injectable seen in one vendor's 3.2-era
  docs does not appear in the CTS 3.2.3 IDL, so it is not implemented
  (cannot be verified normatively).

Branch history note: earlier work on this tracker (below) was done against
3.1; the items above bring it to 3.2.

## What was aligned (face-followups branch)

### Primitive types (`types.h` / `src/face_tss/types.py`)
- Full 15-value FACE 3.2 `RETURN_CODE_TYPE` enumeration, in standard order:
  `NO_ERROR, NO_ACTION, NOT_AVAILABLE, INVALID_PARAM, INVALID_CONFIG,
  INVALID_MODE, TIMED_OUT, ADDR_IN_USE, PERMISSION_DENIED, MESSAGE_STALE,
  IN_PROGRESS, CONNECTION_CLOSED, DATA_BUFFER_TOO_SMALL, DATA_OVERFLOW,
  RESOURCE_LIMIT_REACHED` (the last is the 3.2 addition).
- Renamed `BUFFER_TOO_SMALL` -> `DATA_BUFFER_TOO_SMALL` (standard name).
- `HEADER_TYPE` projection: `instance_uid` / `source_uid` / `timestamp`.
- `QoS_EVENT_TYPE` projection: fixed-capacity (8) list of QoS elements.
  Each receive/callback reports one honest transport-observable element:
  `message_age_ns` (receive time minus envelope send timestamp, clamped to
  zero). No QoS policies are managed or enforced.
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
- Code generator: `.fbs` tables (scalar + string fields, nested tables,
  scalar vectors, enums with integral backing types) -> C value struct +
  codec + descriptor. Unions, vectors of tables/strings, nested vectors,
  non-integral-backed enums, explicit field IDs, and unknown types are
  rejected with a clear error. Deterministic 63-bit FNV-1a message GUID
  from the qualified type name.
- Generated + checked in: `c/generated/positionreport_typed.*`
  (`FaceTSS.PositionReport`, GUID `7542349876525629205`).
- New CTest suite `c-typed` and `face_tss_typed_pubsub` demo.

### Python mirror
- Same signature/header/return-code/envelope alignment as the C API
  (`src/face_tss/` + `tests/` + `examples/`).

## Verification (2026-09-14)
- CMake build: clean (only pre-existing flatcc sign-compare notes).
- CTest: 5/5 suites pass (envelope, config, lifecycle, live, typed),
  including the 3.2 regression tests (connection limit, typed unregister,
  15 return codes).
- Python: 42/42 tests pass, including the 3.2 regression tests.
- C untyped pub/sub across processes: late subscriber 17/20 (first 3 missed
  before dial completed - normal pub/sub behavior). Subscriber-first start
  order works (dials retry in the background); only pre-subscription messages
  are dropped.
- C typed pub/sub across processes: 17/20, all struct fields decoded.
- C++17 proof vs new headers: 10/10, FACE header intact, `-Wall -Wextra` clean.
- Python pub/sub across processes: 16/20 with new header fields.

## Partially implemented / known divergences
- **QoS**: the event carries one honest element per message
  (`message_age_ns`) on receive and callback paths. No QoS policy
  management, no staleness enforcement, no `MESSAGE_STALE` production —
  unsupported FACE QoS guarantees are documented, not emulated.
- **Receive buffers**: caller-owned receive is `face_tss_receive_message_into`
  (C) / `receive_into` (Python) with full `DATA_BUFFER_TOO_SMALL` + required
  size semantics. The allocating `receive_message` remains as a documented
  convenience extension.
- **Configuration**: `Initialize` takes a config object, not a FACE
  `CONFIGURATION_RESOURCE` — use `face_tss_set_reference` +
  `face_tss_initialize_from_resource` (C) or `set_reference` +
  `initialize_from_resource` (Python) for the FACE shape. The full
  FACE::Configuration service API (containers/sets) is not implemented;
  JSON remains the built-in resource adapter.
- **Connection scope**: `Create_Connection` takes a bare name; no
  `CONNECTION_ID` typedef plumbing beyond the integer ID.
- **Codegen limits**: unions, vectors of tables/strings, nested vectors,
  and explicit field IDs are still rejected. Supported: multiple tables per
  file, nested tables, scalar vectors, enums with explicit integral base.
- **Late subscriber**: fixed — dials are non-blocking with background retry,
  so start order no longer matters for connectivity. Messages sent before
  the subscription propagates are still dropped (inherent pub/sub).
- **Threading**: callback stop/destroy locking follows the original design;
  not audited against FACE threading requirements.

## Remaining gaps (not started)
- Real QoS management (policies, staleness enforcement, MESSAGE_STALE).
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
