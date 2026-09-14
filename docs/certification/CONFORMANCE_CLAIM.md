# Conformance Claim: face_tss FACE 3.2 Transport Services Segment

**Claimant:** Devin Plauche
**Unit of Conformance (UoC):** `face_tss` — C library (`libTSS`) and Python bindings
**FACE Technical Standard:** Edition 3.2
**Conformance Class:** Transport Services Segment (TSS), Base + TypedTS interfaces
**Date of Claim:** 2026-09-14
**Status:** SELF-ASSESSED — not independently verified; not certified

## What Is Claimed

The `face_tss` implementation conforms to the FACE 3.2 Transport Services
Segment specification for the following interfaces:

- **FACE::TSS::Base** — Initialize, Create_Connection, Destroy_Connection
- **FACE::TSS::TypedTS** — Send_Message, Receive_Message, Register_Callback,
  Unregister_Callback (3.2 location; 3.1 alias retained)
- **Configuration interface** — Set_Reference, Initialize_From_Resource
- **Return codes** — all 15 FACE 3.2 values in standard order, including
  RESOURCE_LIMIT_REACHED (14)

## Basis for the Claim

1. **CTS 3.2.3 self-test (2026-09-14):** Full workflow exits 0. Data Model
   4/4 PASSED. TS Segment interface assertions PASSED (41 link checks).
   Report: `FACEConformanceTest_face_tss.pdf`.

2. **Behavioral test suites (all passing):**
   - CTest: 6/6 suites (envelope, config, lifecycle, live, conformance, typed)
   - Python: 40/40
   - Functional CTS test article: 53/53 checks executing the FACE C API
     against the real library

3. **Traceability:** Every claimed requirement is traced to implementation
   and verification in `TRACEABILITY.md`.

## What Is NOT Claimed

- **Not certified.** No approved FACE Verification Authority has
  independently executed these tests. This claim is a self-assessment.
- **CSP/TPM/marshalling:** Implemented (2026-09-14); see TRACEABILITY.md sections 6–8.
- **QoS policies:** Implemented (2026-09-14): staleness/priority/reliability policies with MESSAGE_STALE enforcement; see TRACEABILITY.md section 9.
  or staleness enforcement.
- **Behavioral gaps:** PERMISSION_DENIED, MESSAGE_STALE, and IN_PROGRESS
  paths are not exercised (see TRACEABILITY.md).

## Implementation-Defined Behavior

Documented where the FACE standard leaves behavior to the implementation:

- **Connection limit:** 64 open connections; the 65th returns
  RESOURCE_LIMIT_REACHED. This bound is implementation-defined, not
  normative.
- **Transport:** nng-based pub/sub and bus patterns; the standard leaves
  the underlying transport open.
- **QoS:** Staleness/priority/reliability policies with MESSAGE_STALE enforcement; `message_age_ns` reported per message.

## Path to Formal Certification

1. Scope decisions resolved (2026-09-14): CSP/TPM/marshalling and QoS policies implemented per claimant directive.
2. Engage an approved FACE Verification Authority (e.g., LDRA, TES-SAVI).
3. Submit this package (claim, traceability, test report, UoC).
4. The VA independently re-executes the CTS and behavioral suites in
   their controlled environment.
5. Address any findings; pay the VA fee; receive certification.

---

*This document states a self-assessed conformance claim. It does not
constitute FACE certification and must not be represented as such.*
