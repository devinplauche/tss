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

## CTS 3.2.3 full workflow run (2026-09-14)

The official FACE Conformance Test Suite 3.2.3 distribution was downloaded
(`cts/Linux_CTS_3.2.3_Distribution.zip`, gitignored, not committed). The
complete CLI-driven CTS workflow was executed for the `face_tss` unit of
conformance (a FACE 3.2 C99 TSS UoC):

1. **Project validation** (`conformance_test.py -v`): the TSS project
   configuration (`cts/project/face_tss.pcfg`, `gcc_linux_c99.tcfg`,
   both local/gitignored) validates cleanly.
2. **GSL generation** (`conformance_test.py -g`): Gold Standard Libraries
   for the TSS Base and TypedTS interfaces generate successfully; the
   project exports build under the configured C99 toolchain.
3. **Strict toolchain verification**: 41/41 `PASSED`.
4. **Full conformance run** (`conformance_test.py face_tss.pcfg`): exit
   code **0**. Report: `cts/project/FACEConformanceTest_face_tss.pdf`
   (28 pages, local/gitignored).

Reported results:
- Data Model Conformance Tests: **PASSED** (all four checks: FACE
  metamodel validation, OCL constraints, view specification validation,
  shared data model conformance).
- TS Segment Conformance Tests: **PASSED**.
  - Base Interface: **PASSED** (3/3 link assertions: Initialize,
    Create_Connection, Destroy_Connection).
  - Typed Interface: **PASSED** (12/12 link assertions for the three
    generated TypedTS interfaces).
  - TSS POSIX fork: **PASSED** (limited-GSL fork/exec link check).

Scope and limitations (read before citing these results):
- The CTS C Base/TypedTS interface assertions are **link checks**: the CTS
  compiles and links the generated test executables against the
  CTS-local C99 adapter (`cts/project/uoc/`, gitignored) but never runs
  them. The adapter is a signature-level stub with exact generated FACE
  signatures and no heap/libc calls, required by the Non-OSS toolchain's
  `-nodefaultlibs -nostartfiles` constraints. **This is interface/signature
  validation, not functional TypedTS validation.** Real behavior is covered
  by the repo's own C/Python test suites and the earlier functional
  Base-adapter smoke test.
- Environment note: the Java Data Model validator connects to the CTS
  Python protobuf server over loopback; in this sandbox Java's default
  IPv6-mapped IPv4 connection is accepted by the kernel but never reaches
  an IPv4 listening socket. Forcing IPv4 with
  `JAVA_TOOL_OPTIONS=-Djava.net.preferIPv4Stack=true` resolves it. No CTS
  or UoC code was modified for this.
- Formal FACE certification still requires an approved Verification
  Authority. This project is **not certified** and must not be described
  as certified or fully conformant.

## CTS 3.2.3 re-run (2026-09-15)

Re-ran the full `conformance_test.py face_tss.pcfg` workflow against the
current tree after the Configuration/TPM/codegen/discovery enhancements:
exit code **0**, Data Model (4/4) and TS Segment (Base 3/3, Typed 12/12,
POSIX fork) all **PASSED** — identical to the 2026-09-14 result. The
28-page report is committed at
`docs/certification/FACEConformanceTest_face_tss_2026-09-15.pdf`
(45 PASSED, 0 FAILED, 0 not-performed). Same scope and limitations as
above: link-level interface checks against the CTS-local stub adapter,
not functional runs of the production library, and not certification.

## Earlier CTS 3.2.3 self-test (2026-09-14, superseded by the full run above)

The official FACE Conformance Test Suite 3.2.3 distribution was downloaded
(`cts/Linux_CTS_3.2.3_Distribution.zip`, gitignored, not committed). A
FACE 3.2 C-language adapter was written over the library (`cts/adapter/`,
local only): a C mapping layer for the `FACE::TSS::Base` API shape,
`CTS_Factory_Functions.h` with `Get_FACE_TSS_Base()`, and adapter
implementations of `Initialize` / `Create_Connection` /
`Destroy_Connection` delegating to `face_tss_*`.

Results:
- The three official C interface tests for `FACE_TSS_Base`
  (`conformanceInterfaceTests/C/TSS/Base/Base/test{1,2,3}.c`: Initialize,
  Create_Connection, Destroy_Connection) **compile and link** against the
  adapter. Per the CTS user manual this is the actual interface-conformance
  check: the CTS builds these as executables that are never run - it only
  tests that the UoC links with them.
- A functional test driving the same FACE C API with real arguments
  **passes**: Initialize (inline JSON config resource), Create_Connection
  (valid id + max size), unknown name -> `INVALID_PARAM`, Destroy twice ->
  `NO_ERROR` then `NO_ACTION`, and the 3.2 `RESOURCE_LIMIT_REACHED` value
  present in the mapping.
- Out of scope for this pass: literal GUI-driven test execution (see
  below), and the CSP/TPM/marshalling suites (interfaces this UoC does
  not implement). Formal certification remains with an approved
  Verification Authority.

### GUI launcher status (2026-09-14)

The official GUI (`run_CTS_GUI.py` → `ConformanceTestSuiteGUI-1.6.jar`,
a JavaFX app) was launched under Xvfb using Zulu JDK 8 with bundled
JavaFX (the `JDK8_HOME` the launcher requires) plus a generated `.guicfg`
settings file and a `python` → `python3` symlink the GUI's backend calls
need. The Welcome screen renders and the "Run Conformance Test" button
navigates to the "Run Segment Conformance Test" screen (screenshots in
`your_files/tss-cts/`). JavaFX dropdown menus do not open in headless
Xvfb, so File → Projects → Import could not be reached to load the
`.pcfg` through the GUI. The GUI is a frontend over the same
`face_conformance_app` engine driven to completion via CLI above.

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
  Each receive/callback reports honest transport-observable elements:
  `message_age_ns` (receive time minus envelope send timestamp, clamped
  to zero), `priority` (the sender's stamped priority, 0 when absent),
  plus `sequence_gap` when reliability monitoring is enabled on the
  connection. `STALENESS` and `PRIORITY` policies are enforced on the
  receive path; `RELIABILITY` is honest admission control plus
  sequence-gap observability, not reliable delivery.
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
  codec + descriptor. Also supported: unions of tables (discriminator as
  `ubyte` at the declared id, value table at id+1), vectors of tables,
  vectors of strings, nested vectors of scalars (`[[float]]`), and
  explicit field ids (`f:int (id: 3)`, all-or-none per table, union value
  takes the next id). Non-table union members, vectors of unions, nested
  non-scalar vectors, non-integral-backed enums, and unknown types are
  rejected with a clear error. Deterministic 63-bit FNV-1a message GUID
  from the qualified type name.
- Generated + checked in: `c/generated/positionreport_typed.*`
  (`FaceTSS.PositionReport`, GUID `7542349876525629205`),
  `c/generated/telemetry_typed.*` (`FaceTSS.Telemetry`), and
  `c/generated/event_typed.*` (`FaceTSS.Event`, GUID `6175617105530255968`)
  from `c/schemas/kitchen_sink.fbs`, which exercises unions, vectors of
  tables/strings, nested scalar vectors, and explicit ids.
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
- **QoS**: the event carries honest elements per message
  (`message_age_ns`, `priority`, and `sequence_gap` when reliability
  monitoring is active) on receive and callback paths. Two policies are
  enforced via `face_tss_set_qos_policy` / `face_tss_get_qos_policy` (C)
  and `set_qos_policy` / `get_qos_policy` (Python):
  - `STALENESS` (alias `MAX_AGE`): messages older than the connection's
    threshold (ns) are discarded on receive and the call returns
    `MESSAGE_STALE` / raises `MessageStaleError`, with the drop counted
    in `stats.stale_dropped`. Stale messages are not delivered to
    registered callbacks.
  - `PRIORITY`: the sender's value is stamped into the wire envelope
    (backward-compatible field 8; absent decodes as 0) and reported in
    the receiver's QoS event. A receiving connection's value acts as a
    minimum delivery threshold: below-threshold messages are dropped
    (blocking receive keeps waiting for a qualifying message;
    callbacks silently skip them), counted in
    `stats.priority_dropped`.
  - `RELIABILITY` accepts only `BEST_EFFORT` (0); `RELIABLE` (1) is
    rejected with `NOT_AVAILABLE` on the best-effort nng transports
    (pub/sub, bus) rather than being silently pretended. Setting either
    level enables per-connection sequence-gap monitoring: the first
    message (or a source change) re-baselines without counting a gap,
    and skipped sequence numbers are reported as `sequence_gap` in the
    QoS event and counted in `stats.reliability_gaps`.
  These policy names and semantics are this implementation's own
  extension — the FACE IDL defines QoS only as string key/value
  elements with no normative policy names. There is no cross-connection
  QoS negotiation, and nng delivery remains best-effort.
- **Receive buffers**: caller-owned receive is `face_tss_receive_message_into`
  (C) / `receive_into` (Python) with full `DATA_BUFFER_TOO_SMALL` + required
  size semantics. The allocating `receive_message` remains as a documented
  convenience extension.
- **Configuration**: `Initialize` takes a config object, not a FACE
  `CONFIGURATION_RESOURCE` — use `face_tss_set_reference` +
  `face_tss_initialize_from_resource` (C) or `set_reference` +
  `initialize_from_resource` (Python) for the FACE shape. The full
  FACE::Configuration service API (containers/sets) is implemented in
  `face_tss/config_service.h` + `config_service.c` (C) and
  `face_tss/config_service.py` (Python): `Initialize`, `Open`
  (int64 session handle), `Get_Size`, `Read`, `Seek`
  (`SEEK_FROM_START`/`CURRENT`/`END`), `Close`, with `INVALID_CONFIG` /
  `INVALID_PARAM` / `NOT_AVAILABLE` per the IDL. Two built-in backends:
  `memory:<name>` (in-memory key/value sets, populated via the
  `write` extension) and `file:<dir>` (regular files as sets, streamed
  with seek). JSON remains the built-in resource adapter for the
  injection path.
- **Connection scope**: `Create_Connection` takes a bare name; no
  `CONNECTION_ID` typedef plumbing beyond the integer ID.
- **Codegen limits**: vectors of tables inside nested vectors
  (e.g. `[[MyTable]]`), nesting deeper than 3 levels, and `[[[string]]]`
  are rejected. Supported: multiple tables per file, nested tables,
  scalar vectors, string vectors, nested scalar vectors (up to 3 levels)
  and nested string vectors (up to 2 levels), enums with explicit
  integral base or defaulted to `int32`, unions of tables, unions with
  mixed scalar/string/table members, vectors of tables/strings, vectors
  of unions (table/string members only; two vtable slots per the
  FlatBuffers parallel-vector encoding), and explicit field ids
  (all-or-none per table; `[[scalar]]` nested vectors are this
  implementation's own extension, verified by round-trip tests, not by
  interop with the official flatc compiler).
- **Late subscriber**: fixed — dials are non-blocking with background retry,
  so start order no longer matters for connectivity. Messages sent before
  the subscription propagates are still dropped (inherent pub/sub).
- **Threading**: audited 2026-09-15 (no normative threading requirement was
  found in the local FACE CTS 3.2.3 IDL; this is an implementation
  concurrency audit). The TSS instance lock is never held across blocking
  transport I/O; per-connection refcounts keep connections and the
  instance alive until in-flight operations finish; destroy/unregister
  join callback threads with no locks held; destroying a connection or
  the instance aborts blocked sends/receives by closing the socket;
  a data callback may unregister its own connection (self-stop defers
  the free to the dispatch loop's exit); TPM destroy wakes blocked
  readers via broadcast and waits for waiters to drain. Per-call send
  and receive timeouts are serialized per direction (nng timeouts are
  per-socket). Verified by new `c-threading` CTest suite and
  `tests/test_threading.py` (Python): blocked receive does not stall
  the instance, destroy interrupts blocked I/O, unregister mid-stream
  and self-unregister do not deadlock, unregister leaves the connection
  usable for re-registration, concurrent receives keep their own
  timeouts, TPM destroy wakes a blocked reader. Full CTest suite is
  clean under ASan+UBSan and ThreadSanitizer (the two TSan warnings
  found during the audit were data races in test-only callback
  counters, fixed; TSan needs `setarch -R` on this machine because of
  a shadow-memory mapping conflict with ASLR).
- **TPM**: the TPM is a local loopback model, not a real transport.
  `Is_Data_Available` and `Read_From_Transport` honor their timeout
  (ns, `TIMEOUT_INFINITE` = -1 blocks forever, 0 polls): they block until
  data arrives on a listed channel or the timeout expires, using a
  condition variable signaled by `Write_To_Transport` (channel
  close/state change also wakes waiters). Expiry returns `NO_ERROR`
  with an empty list from `Is_Data_Available` and `TIMED_OUT` from
  `Read_From_Transport`. Implemented in C (`face_tss/tpm.h`) and Python
  (`face_tss/tpm.py`, pure-Python mirror with the same semantics).

## Remaining gaps (not started)
- Any safety/security certification artifacts.
- The FACE CTS itself.

## Implemented extensions (not part of the FACE standard)

- **UDP-broadcast peer discovery** (`face_tss/discovery.h`): instances
  announce name + address on UDP port 51970 and discover peers without
  static config. This is a tss implementation extension; FACE 3.2 has no
  normative discovery protocol.

## Transport note
nng (pub/sub, bus) is the underlying transport. FACE permits multiple
underlying transports; nng is not itself a conformance blocker, but its
delivery semantics (best-effort pub/sub, no persistence) bound what QoS and
reliability claims this TSS can ever make.
