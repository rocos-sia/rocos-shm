# ABI v2 contract (2026-10-27)

Based on [ROCOS-ECM/IGH shared-memory specification](https://rocos-sia.feishu.cn/wiki/DQXzwAy0viTXCJkRAEpcrpsXnsg), including the approved 48-byte magic extension. Source of exact fields/enums: `include/rocos_shm/abi.hpp`. Changes are reviewed in this repository and consumer revisions are updated as one release set.

## Fixed layout

Linux x86_64 LP64, default natural alignment, GCC/Clang C++11, little endian. The original `long`, `int`, `bool`, `double` fields retain their specified ABI; sizes are asserted. All new fields use fixed-width integers. No packing or compiler-dependent capacity overrides.

| Type | Size | Alignment | Selected offsets |
|---|---:|---:|---|
| ShmHeader | 80 | 8 | magic=0, abi_version=48, header_size=52, bus_size=56, input_bytes=60, output_bytes=64, ready=68, instance_id=72 |
| PdVar | 84 | 4 | name=0, offset=72, size=76, index=80, sub_index=82 |
| Slave | 4396 | 4 | id=0, name=4, input_vars=92, output_vars=2192, vendor_id=4292, online=4380, runtime_valid=4392 |
| AsyncCommandBox | 128 | 8 | cmd_id=0, instance_id=8, deadline=16, data_size=36, data=40, result_code=104, status=120 |
| EcatBus | 220088 | 8 | timestamp=0, slaves=68, ecm_version=219868, heartbeat_mono_ns=219912, cmd_box=219960 |
| SharedMemoryV2 | 220168 | 8 | header=0, bus=80 |

magic occupies 48 bytes; bytes 0..7 are `ROCOSHM2`, bytes 8..47 are zero reserved bytes. Compare the first eight bytes, not a C string. ABI number is exactly 2. Future assignments to reserved bytes require a documented semantic contract and incremented ABI. `header_size` and `bus_size` are not a substitute for ABI matching. Each master start generates a nonzero random instance_id.

The master initializes static metadata, lengths and mailbox before a release store of ready=1. Clients fstat/map enough for the fixed header, acquire-load ready, check magic/exact ABI/sizes/bounds and actual PDO object lengths before exposing EcatBus. An unready or incompatible object returns an error. Zero-length PDOs are valid but may not be accessed. A changed Slave size changes every subsequent element's offset; this is incompatible with v1 even though fields were appended.

## Fields and validity

The inherited timestamp is Unix wall-clock microseconds; heartbeat_mono_ns uses CLOCK_MONOTONIC nanoseconds. dt and cycle statistics use microseconds. `is_authorized` is a product runtime-license result (IGH has no such semantics and uses true), not a WKC/link check. `ecm_version` is the running backend wrapper's version, independent of ABI/package version.

`backend_type`: 0 unknown, 1 EC-Master, 2 IGH. `supported_cmds` uses bit(cmd_type), only enabled implemented commands. `state_control_supported` gates request_state. `status_valid` bits 0/1/2 validate online count/link/WKC. `wc_state`: 0 unknown, 1 normal, 2 error. `dc_sync_status`: 0 disabled, 1 syncing, 2 synced, 3 lost lock, 4 unknown. `dc_metric_kind`: 0 invalid, 1 signed DCM control error, 2 nonnegative IGH bus deviation; do not compare unlike metrics. Readers enforce diagnostic freshness using dc_sample_mono_ns and their configured expiry.

Slave `info_valid` bits 0..3 independently validate vendor/product/revision/serial; bit4 validates the firmware string. `identity_source`: 0 unknown, 1 online device, 2 configuration (not physical verification). `runtime_valid` bits 0/1/2 validate online/al_state/al_status_code. Firmware is optional 0x100A:00 or a device-defined object, never a revision-number substitute; reject truncation. Names/identity/PDO descriptions remain immutable after ready. Missing data clears validity bits.

Live scalar fields are accessed atomically on both sides. This guarantees individual values do not tear, not simultaneous multi-field snapshots. Cycle semaphores are notifications, not mutexes. PDO exchange retains the existing data-plane guarantees and does not gain whole-frame consistency from v2.

## Single mailbox

States: IDLE(0) -> REQUESTED(1) -> PROCESSING(2) -> SUCCESS(3)/FAILED(4)/TIMEOUT_UNKNOWN(5) -> IDLE. App publishes requests, master publishes results, App copies the result before clearing IDLE. One App-local mutex covers the complete transaction. Status is an aligned lock-free uint32_t with acquire/release operations; payload is ordinary bytes under exclusive ownership.

Commands: NONE=0, SDO_READ=1, SDO_WRITE=2, RESET_STATS=3, RESET_FAULT=4 (reserved/unsupported). target_slave is a zero-based descriptor-array index; RESET_STATS uses -1. The backend resolves its own private SDK-ID mapping. SDO request size/capacity is 1..64, successful reads return actual size; over-capacity data is rejected, not truncated. Failed responses set data_size=0. No implicit data conversion, Complete Access, object splitting or file transfer.

| Result | Number |
|---|---:|
| RESULT_OK | 0 |
| INVALID_ARGUMENT | 1 |
| UNSUPPORTED | 2 |
| BUSY | 3 |
| STALE_INSTANCE | 4 |
| SLAVE_OFFLINE | 5 |
| STATE_MISMATCH | 6 |
| BUFFER_TOO_SMALL | 7 |
| DEADLINE_EXCEEDED | 8 |
| BACKEND_ERROR | 9 |
| OUTCOME_UNKNOWN | 10 |

`error_valid` bit0 validates backend_error; bit1 validates sdo_abort_code. Never reinterpret an SDK error number as a raw CoE abort. Unsupported retrieval clears the validity bit. Requests have monotonically increasing cmd_id, current instance_id and an absolute monotonic deadline. Expired-before-issue requests fail DEADLINE_EXCEEDED. A local App wait timeout cannot reset the box: drain its eventual result with pollCommand, or stop/restart both sides. If an operation may have happened but cannot be confirmed, publish TIMEOUT_UNKNOWN only once backend buffer use has finished. No automatic retries of writes.

Normal state changes are serialized with commands. Transitions reject new SDO, active SDO defers ordinary transitions, hardware fault stopping must not wait for the command lock. RESET_STATS asks the RT thread to reset on a cycle boundary and acknowledges only after completion; the legacy resetCycleTime bool is reserved/deprecated and not consumed.

## Verification and backend responsibilities

Offline tests cover fixed layout, mismatch rejection, 50/25 bounds, byte alignment, owner locking, zero-sized PDOs, full 64-byte mailbox, errors, independent processes, App multi-thread serialization, local timeout and late response consumption. Each backend separately validates allowed states, real identities, SDO abort/failure semantics, no post-return buffer use and cyclic latency. The library does not implement EC-Master/IGH SDK calls or device object policies.
