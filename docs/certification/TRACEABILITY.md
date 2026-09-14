# FACE 3.2 TSS Conformance Traceability Matrix

**Unit of Conformance:** `face_tss` Transport Services Segment implementation
**FACE Edition:** 3.2
**Date:** 2026-09-14
**Status:** Self-assessment; not independently verified

This matrix traces each FACE 3.2 TSS requirement to its implementation and
the test(s) that verify it. A Verification Authority re-executes the
"Verification" column in their controlled environment.

## Conventions

- **Req ID:** FACE 3.2 TSS section or IDL operation
- **Implementation:** Source file(s) implementing the requirement
- **Verification:** Test executable / test name that exercises it
- **Notes:** Scope limitations or implementation-defined behavior

## 1. Base Interface (FACE::TSS::Base)

| Req ID | Requirement | Implementation | Verification | Notes |
|--------|-------------|----------------|--------------|-------|
| Base::Initialize | Initialize the TSS from a configuration resource | `c/src/tss.c:face_tss_initialize`, `c/src/configuration.c` | `c-conformance:init_semantics`, `cts-functional-article:[lifecycle]` | Idempotent: second call returns NO_ACTION |
| Base::Create_Connection | Create a named connection; returns id and max message size | `c/src/tss.c:face_tss_create_connection` | `c-conformance:create_connection_errors`, `cts-functional-article:[lifecycle]` | RESOURCE_LIMIT_REACHED at 64 open connections (implementation-defined bound, documented) |
| Base::Destroy_Connection | Destroy a connection by id | `c/src/tss.c:face_tss_destroy_connection` | `c-conformance:destroy_connection_idempotent`, `cts-functional-article:[lifecycle]` | Idempotent: second destroy returns NO_ACTION; id 0 returns INVALID_PARAM |

## 2. TypedTS Interface (FACE::TSS::TypedTS)

| Req ID | Requirement | Implementation | Verification | Notes |
|--------|-------------|----------------|--------------|-------|
| TypedTS::Send_Message | Send a payload; transaction_id is inout | `c/src/tss.c:face_tss_send_message` | `c-conformance:transaction_id_semantics`, `cts-functional-article:[send_receive]`, `c-live` | UNSPECIFIED (0) → TSS assigns; explicit id passes through |
| TypedTS::Receive_Message | Blocking receive with timeout; writes back transaction id | `c/src/tss.c:face_tss_receive_message`, `face_tss_receive_message_into` | `c-conformance:transaction_id_semantics`, `c-conformance:receive_errors`, `cts-functional-article:[send_receive]` | TIMED_OUT on expiry; DATA_BUFFER_TOO_SMALL when buffer insufficient |
| TypedTS::Register_Callback | Register a message callback for a connection | `c/src/tss.c:face_tss_register_callback` | `cts-functional-article:[callbacks]`, `c-live` | Background delivery thread |
| TypedTS::Unregister_Callback | Unregister a connection's callback (3.2 location) | `c/src/tss.c:face_tss_unregister_callback`, `c/include/face_tss/typed.h` | `cts-functional-article:[callbacks]` | NO_ACTION when none registered; 3.1 alias `face_tss_unregister_callback` retained |

## 3. Return Codes (FACE::Common::RETURN_CODE_TYPE, 15 values)

| Code | Ordinal | Verified in |
|------|---------|-------------|
| NO_ERROR | 0 | All tests |
| NO_ACTION | 1 | `c-conformance:init_semantics`, `destroy_connection_idempotent` |
| NOT_AVAILABLE | 2 | `c-conformance:init_semantics` (pre-init ops) |
| INVALID_PARAM | 3 | `c-conformance:create_connection_errors`, `configuration_errors` |
| INVALID_CONFIG | 4 | `c-conformance:configuration_errors`, `transaction_id_semantics` |
| INVALID_MODE | 5 | `c-live` (direction violations) |
| TIMED_OUT | 6 | `c-conformance:receive_errors`, `cts-functional-article:[send_receive]` |
| ADDR_IN_USE | 7 | `c-live` (bind conflicts) |
| PERMISSION_DENIED | 8 | Not exercised (platform-dependent) |
| MESSAGE_STALE | 9 | Not exercised (no staleness policy implemented; see QoS notes) |
| IN_PROGRESS | 10 | Not exercised (no async ops in this implementation) |
| CONNECTION_CLOSED | 11 | `cts-functional-article:[send_receive]` (unknown id) |
| DATA_BUFFER_TOO_SMALL | 12 | `c-conformance` via `receive_message_into` paths |
| DATA_OVERFLOW | 13 | `c-live` (oversize payload) |
| RESOURCE_LIMIT_REACHED | 14 | `c-conformance:create_connection_errors` (64-connection bound) |

## 4. Configuration Interface

| Req ID | Requirement | Implementation | Verification | Notes |
|--------|-------------|----------------|--------------|-------|
| Set_Reference | Associate a configuration with an interface instance | `c/src/tss.c:face_tss_set_reference` | `c-config` | NO_ACTION/INVALID_MODE semantics per spec |
| Initialize_From_Resource | Initialize from a bounded resource string | `c/src/tss.c:face_tss_initialize_from_resource` | `c-conformance:configuration_errors`, `c-config` | `json:{...}` inline or file path; 256-char bound enforced |

## 5. Data Model

| Req ID | Requirement | Verification | Notes |
|--------|-------------|--------------|-------|
| FACE Meta Model Validation | CTS Data Model suite | CTS 3.2.3 full run (2026-09-14): PASSED | Exit 0 |
| OCL Constraints Check | CTS Data Model suite | CTS 3.2.3 full run (2026-09-14): PASSED | Exit 0 |
| View Specification Validation | CTS Data Model suite | CTS 3.2.3 full run (2026-09-14): PASSED | Exit 0 |
| Shared Data Model Conformance | CTS Data Model suite | CTS 3.2.3 full run (2026-09-14): PASSED | Exit 0 |

## 6. CSP Interface (FACE::TSS::CSP)

| Req ID | Requirement | Implementation | Verification | Notes |
|--------|-------------|----------------|--------------|-------|
| CSP::Initialize | Initialize the CSP from a configuration resource | `c/src/csp.c:face_tss_csp_initialize` | `c-extended:csp_lifecycle` | Idempotent: second call returns NO_ACTION |
| CSP::Open | Open a data store; returns a token | `c/src/csp.c:face_tss_csp_open` | `c-extended:csp_lifecycle` | RESOURCE_LIMIT_REACHED at 16 open stores |
| CSP::Close | Close a data store by token | `c/src/csp.c:face_tss_csp_close` | `c-extended:csp_lifecycle` | INVALID_PARAM for unknown token |
| CSP::Create | Create a data store entry | `c/src/csp.c:face_tss_csp_create_entry` | `c-extended:csp_crud` | INVALID_PARAM if entry exists |
| CSP::Read | Read a data store entry | `c/src/csp.c:face_tss_csp_read` | `c-extended:csp_crud` | DATA_BUFFER_TOO_SMALL with required size |
| CSP::Update | Update a data store entry | `c/src/csp.c:face_tss_csp_update` | `c-extended:csp_crud` | INVALID_PARAM if entry missing |
| CSP::Delete | Delete a data store entry | `c/src/csp.c:face_tss_csp_delete` | `c-extended:csp_crud` | INVALID_PARAM if entry missing |

## 7. TPM Interface (FACE::TSS::TPM)

| Req ID | Requirement | Implementation | Verification | Notes |
|--------|-------------|----------------|--------------|-------|
| TPM::Initialize | Initialize the TPM | `c/src/tpm.c:face_tss_tpm_initialize` | `c-extended:tpm_lifecycle` | Idempotent |
| TPM::Open_Channel | Open a transport channel | `c/src/tpm.c:face_tss_tpm_open_channel` | `c-extended:tpm_lifecycle` | RESOURCE_LIMIT_REACHED at 16 channels |
| TPM::Close_Channel | Close a channel | `c/src/tpm.c:face_tss_tpm_close_channel` | `c-extended:tpm_lifecycle` | INVALID_PARAM for unknown channel |
| TPM::Request_TPM_State_Change | Change TPM state | `c/src/tpm.c:face_tss_tpm_request_state_change` | `c-extended:tpm_lifecycle` | Validates state enum |
| TPM::Is_Data_Available | Check channels for data | `c/src/tpm.c:face_tss_tpm_is_data_available` | `c-extended:tpm_data` | Non-blocking check |
| TPM::Get_TPM_Status | Get transport status | `c/src/tpm.c:face_tss_tpm_get_status` | `c-extended:tpm_lifecycle` | Returns INIT_COMPLETE after init |
| TPM::Read_From_Transport | Read a datagram | `c/src/tpm.c:face_tss_tpm_read_from_transport` | `c-extended:tpm_data` | TIMED_OUT when no data |
| TPM::Write_To_Transport | Write a datagram | `c/src/tpm.c:face_tss_tpm_write_to_transport` | `c-extended:tpm_data` | Loopback for testing |
| TPM::Register_TPM_Callback | Register data/event callback | `c/src/tpm.c:face_tss_tpm_register_callback` | `c-extended:tpm_data` | NO_ACTION if already registered |
| TPM::Unregister_TPM_Callback | Unregister callback | `c/src/tpm.c:face_tss_tpm_unregister_callback` | `c-extended:tpm_data` | NO_ACTION if none registered |

## 8. Primitive Marshalling (FACE::TSS::Primitive_Marshalling)

| Req ID | Requirement | Implementation | Verification | Notes |
|--------|-------------|----------------|--------------|-------|
| Marshal_* | Marshal primitives to buffer | `c/src/marshalling.c:face_tss_marshal_*` | `c-extended:marshalling_roundtrip` | Big-endian; DATA_BUFFER_TOO_SMALL on overflow |
| Unmarshal_* | Unmarshal primitives from buffer | `c/src/marshalling.c:face_tss_unmarshal_*` | `c-extended:marshalling_roundtrip` | Covers short/long/long_long/float/double/char/boolean/octet |

## 9. QoS Policies

| Req ID | Requirement | Implementation | Verification | Notes |
|--------|-------------|----------------|--------------|-------|
| Set policy | Set staleness/priority/reliability | `c/src/qos.c:face_tss_qos_set_policy` | `c-extended:qos_policies` | Per-connection policies |
| Get policy | Retrieve a policy value | `c/src/qos.c:face_tss_qos_get_policy` | `c-extended:qos_policies` | NO_ACTION when unset |
| Check staleness | MESSAGE_STALE enforcement | `c/src/qos.c:face_tss_qos_check_staleness` | `c-extended:qos_policies` | Returns MESSAGE_STALE when age exceeds threshold |
| Clear policies | Remove connection policies | `c/src/qos.c:face_tss_qos_clear_policies` | `c-extended:qos_policies` | |

## 10. Out of Scope (explicitly excluded from this conformance claim)

| Area | Status | Rationale |
|------|--------|-----------|
| PERMISSION_DENIED paths | Not exercised | Platform-dependent; no test hook |
| IN_PROGRESS | Not exercised | No asynchronous operations in this implementation |

## Test Execution Summary (2026-09-14)

| Suite | Result |
|-------|--------|
| CTest (6 suites: envelope, config, lifecycle, live, conformance, typed) | 6/6 PASS |
| Python (40 tests) | 40/40 PASS |
| CTS 3.2.3 full workflow (Data Model + TS Segment) | EXIT 0, all PASSED |
| Functional CTS test article (53 checks) | 53/53 PASS |

---

*This is a self-assessment traceability matrix. Formal conformance requires
independent execution by an approved FACE Verification Authority.*
