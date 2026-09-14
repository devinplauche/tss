# Test Report: face_tss FACE 3.2 TSS Conformance

**Date:** 2026-09-14
**Unit of Conformance:** `face_tss` (C library + Python bindings)
**FACE Edition:** 3.2
**Test Environment:** Linux x86_64, GCC, CMake

## Summary

| Suite | Tests | Passed | Failed | Notes |
|-------|-------|--------|--------|-------|
| CTest: c-envelope | — | PASS | 0 | Message envelope encoding |
| CTest: c-config | — | PASS | 0 | Configuration parsing and validation |
| CTest: c-lifecycle | — | PASS | 0 | Init/create/destroy lifecycle |
| CTest: c-live | — | PASS | 0 | Live pub/sub message flow |
| CTest: c-conformance | 7 | PASS | 0 | **New:** FACE 3.2 spec-driven behavioral tests |
| CTest: c-extended | 6 | PASS | 0 | **New:** CSP/TPM/marshalling/QoS tests |
| CTest: c-typed | — | PASS | 0 | TypedTS codegen wrappers |
| Python suite | 40 | 40 | 0 | Python bindings + regression |
| CTS 3.2.3 full workflow | — | PASS | 0 | Exit 0; Data Model (4/4) + TS Segment PASSED |
| Functional CTS article | 53 | 53 | 0 | **New:** FACE C API shapes over real library |

**Total: all suites pass, 0 failures.**

## New in This Report

### c-conformance (7 tests)

Spec-driven behavioral tests added 2026-09-14 to cover what the CTS's
link-only interface checks do not execute:

1. `return_code_ordinals` — All 15 FACE 3.2 return codes in standard order,
   including the 3.2 addition RESOURCE_LIMIT_REACHED (14).
2. `init_semantics` — NOT_AVAILABLE before Initialize; NO_ACTION on re-init.
3. `create_connection_errors` — INVALID_PARAM for unknown/NULL names;
   RESOURCE_LIMIT_REACHED at the 64-connection implementation bound.
4. `destroy_connection_idempotent` — NO_ACTION on second destroy;
   INVALID_PARAM for id 0.
5. `transaction_id_semantics` — UNSPECIFIED → TSS assigns and writes back;
   explicit ids pass through; receive writes back the sent id.
6. `receive_errors` — TIMED_OUT with no message pending.
7. `configuration_errors` — INVALID_PARAM for NULL/oversize resources;
   INVALID_CONFIG for malformed JSON.

### Functional CTS test article (53 checks)

`cts/adapter/cts_functional_article.c` drives the complete FACE 3.2 C API
(Base::Initialize/Create_Connection/Destroy_Connection,
TypedTS::Send_Message/Receive_Message/Register_Callback/Unregister_Callback)
through the adapter against the real library:

- `[lifecycle]` — init, idempotent re-init, create/destroy, error paths
- `[send_receive]` — pub/sub message flow with transaction IDs, TIMED_OUT,
  unknown-connection handling
- `[callbacks]` — register, delivery, unregister, NO_ACTION on double-unregister
- `[return_codes]` — all 15 ordinals

This is the executable counterpart to the CTS's link-only checks: it proves
the operations the CTS links against actually behave per specification.

## CTS 3.2.3 Full Workflow (2026-09-14)

- Project validation: exit 0
- GSL generation: "Built GSLs successfully"
- Strict toolchain verification: exit 0, 41 PASSED
- Data Model: 4/4 PASSED (Meta Model, OCL, View Spec, Shared Data Model)
- TS Segment: Base PASSED, TypedTS PASSED, POSIX fork PASSED
- Report: `FACEConformanceTest_face_tss.pdf` (28 pages)

**Qualification:** The CTS C interface assertions are `test_type: LINK`
(compile+link only; executables never run). Behavioral conformance is
established by the suites above, not by the CTS run alone.

## Known Limitations

- CSP/TPM/marshalling interfaces: not implemented (out of scope)
- QoS: `message_age_ns` reported; no policy management or staleness enforcement
- PERMISSION_DENIED, MESSAGE_STALE, IN_PROGRESS: not exercised (see
  TRACEABILITY.md for rationale)

## Reproduction

```bash
cd ~/workspace/tss/build
ctest                    # C suites (6/6)
cd ~/workspace/tss
python -m pytest python/tests/   # Python (40/40)
# Functional article:
gcc -o /tmp/cts_article cts/adapter/cts_adapter.c \
    cts/adapter/cts_functional_article.c \
    -Icts/adapter -Ic/include -Ibuild \
    -Lbuild -lTSS -lnng -lflatccrt -lpthread \
    -Wl,-rpath,$PWD/build
/tmp/cts_article
```

---

*Self-test report. Formal certification requires independent execution by an
approved FACE Verification Authority.*
