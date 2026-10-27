# rocos-shm

ROCOS EtherCAT shared-memory protocol **v2**, package **2.0.0**. Header-only C++11 library for Linux x86_64 GCC/Clang. Repository: https://github.com/rocos-sia/rocos-shm .

The ABI, POSIX resource lifecycle, scalar atomic access, PDO bounds checks and single-mailbox handshake have one source here. EC-Master/IGH calls and App logging/business error conversion stay in their respective adapters. There is no SDK, Boost, yaml-cpp or App dependency. Derived from ROCOS-ECM's `shared_memory_config.hpp`; GPL-3.0-or-later, see LICENSE.

## Build and test

With CMake 3.20 or newer, use the Release or Debug presets from the repository
root. Configure each preset once before building; subsequent builds only need
`cmake --build --preset release` or `cmake --build --preset debug`.
Both presets enable the offline v2 IPC tests and use Unix Makefiles.

```sh
cmake --preset release
cmake --build --preset release
ctest --test-dir build/release -R '^rocos_shm_v2$' --output-on-failure --timeout 30

cmake --preset debug
cmake --build --preset debug
ctest --test-dir build/debug -R '^rocos_shm_v2$' --output-on-failure --timeout 30
```

Build outputs are in `build/release` and `build/debug`, respectively. Add
`--parallel 2` to a build command to compile with two jobs.

Without presets:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DROCOS_SHM_BUILD_TESTS=ON
cmake --build build -- -j2
ctest --test-dir build -R '^rocos_shm_v2$' --output-on-failure --timeout 30
```

The default build includes the Web monitor and requires CMake 3.16 and C++17. For the C++11 library alone, set `-DROCOS_SHM_BUILD_WEB=OFF`; its CMake minimum remains 3.10. On CMake 3.10 use `mkdir build && cd build && cmake .. -DROCOS_SHM_BUILD_WEB=OFF` and run `ctest` inside the build directory. Tests use PID-derived IDs >=100000, no hardware and no global IPC cleanup. They create independent processes and mappings; they do not prove EtherCAT timing or SDK thread safety.

## Shared Web monitor (ECM / IgH)

[`tools/shm_web`](tools/shm_web/README.md) is the shared ABI v2 monitor and debugging tool
for `rocos-ecm` and `rocos-igh`. It was moved from `rocos-igh/tools/shm_web`;
consumers no longer maintain their own copy. It uses only `rocos::shm`, without
EtherCAT SDKs or a consumer source tree. `ROCOS_SHM_BUILD_WEB` defaults to `ON`;
the tool requires C++17 and CMake 3.16. Set it to `OFF` for a library-only build
(C++11 / CMake 3.10). Existing CMake caches retain their saved value; pass
`-DROCOS_SHM_BUILD_WEB=ON` once to enable it in a previously disabled build.

```sh
cmake -S . -B build-web -DBUILD_TESTING=ON
cmake --build build-web --parallel 2
ctest --test-dir build-web -R '^shm_web_output$' --output-on-failure
node --test tools/shm_web/*.test.mjs  # Node.js 20 or newer
./build-web/tools/shm_web/shm_web_server --master-id 0 --port 8484
```

Start a compatible ECM or IgH master with the same ID, then open
`http://127.0.0.1:8484`. Each server observes one ID; run another server with a
different `--master-id` and `--port` to monitor another bus. ECM and IgH masters
running together must use distinct IDs. Both real and demo sessions start read-only.
The UI can explicitly enable a single writer session for supported state requests,
SDO commands and RxPDO writes; stop other writable Apps before enabling it.
`--demo` creates its own simulated bus; use an isolated, non-production master ID. See the tool's
README for standalone builds and deployment requirements.

## Consume a pinned revision

```cmake
# Third-party checkout is a Git submodule pinned by the consumer repository.
add_subdirectory(3rdparty/rocos-shm)
target_link_libraries(my_target PRIVATE rocos::shm)
```

Or install the headers/CMake package to a development prefix (`cmake --build build --target install`, with `CMAKE_INSTALL_PREFIX` set at configuration), then:

```cmake
find_package(rocos_shm 2.0.0 EXACT CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE rocos::shm)
```

```cpp
#include <rocos_shm/shared_memory_config.hpp>
```

`rocos::EcatConfigMaster` and `rocos::EcatConfig` remain aliases for `SharedMemoryConfig`. Do not maintain copies of this header in consumer repositories. Update the pinned commits through coordinated dependency PRs, compile every consumer and run cross-process tests before deploying the matched binaries. An updated header does not update an already built executable. Package version and `SHM_ABI_VERSION` are distinct; changing any layout/capacity or assigning reserved bytes requires an ABI increment. Product versions remain independent.

## Master lifecycle

1. Construct `SharedMemoryConfig(id)`, `createSharedMemory()`, then `createPdDataMemoryProvider(inputBytes, outputBytes)`; check every result.
2. Populate static names, PDO descriptors, identity/firmware, product version, backend/capability fields. `setPdoBits()` rejects non-byte-aligned entries and out-of-range mappings. Limits: 50 slaves, 25 entries/direction/slave. Zero-length PDO objects are permitted and have null data pointers.
3. Create the one non-RT command worker; it may poll `processCommand(handler, maxDeadlineMs)` but the library won't process before ready. `publishReady()` validates the layout and release-publishes ready.
4. The cyclic task exchanges PDOs, atomically updates live scalars, then calls `notifyClients()`. No mailbox polling, blocking SDO or heap work belongs in that task. For RESET_STATS, the adapter must request/reset/ack at an RT cycle boundary before returning success.
5. Stop the command worker and cyclic task before destroying the owner. Only its own resources are unlinked, while owner locks are still held.

`processCommand` handles protocol validation/ownership and invokes a synchronous handler with private request/response buffers. The optional third `preflight` callback validates current device identity, online state and allowed AL state before PROCESSING; the handler must reject layout-changing writes, serialize with normal state transitions, honour the absolute deadline and populate backend error validity bits. Returning `OUTCOME_UNKNOWN` publishes `BOX_TIMEOUT_UNKNOWN`. The handler must not return while a backend can still access its buffers; do not automatically retry writes. Abnormal shutdown must promptly stop hardware operation and mark in-flight outcomes unknown. Monitor values that cannot be obtained must be marked invalid/unknown, not presented as successful zero values.

## Client lifecycle and timeout

Use **one shared `SharedMemoryConfig` object per bus** in the sole writable App:

```cpp
rocos::SharedMemoryConfig client(0);
if (!client.connect()) { /* inspect client.lastError(); report error */ }
rocos::AsyncCommandBox req, response;
req.cmd_type = rocos::CMD_SDO_READ;
req.target_slave = 0;          // array index, not SDK ID or station address
req.sdo_index = 0x1000;
req.data_size = 4;             // receive capacity
int32_t result = client.executeCommand(req, response);
```

One local mutex covers idle-check, request publication, wait and response copy/ack. `OUTCOME_UNKNOWN` from a local wait timeout leaves the original transaction pending; continue `pollCommand(response)` until it returns something other than `BUSY`, or stop both processes and reconnect. Do not overwrite a pending or terminal mailbox, do not automatically resend SDO writes. The response's `status` distinguishes a consumed terminal result from a local wait ending. The default absolute deadline is submission +1000ms; a supplied deadline is checked against the master's configured upper bound. Command IDs are generated by the client and never reused within its connected instance.

Legacy `getInstance(id)` returns a per-ID singleton; connection errors throw instead of terminating the process. Prefer `connect()` and `lastError()` for probing. `resetCycleTime()` submits RESET_STATS; use `executeCommand()` if you need its result.

Read-only monitors use `connect(true)`: mappings are read-only, no command writes or semaphore consumption. `getSlave()` samples live scalars individually. Raw public pointers exist for source compatibility, but live scalar reads/writes must use `atomicLoad`/`atomicStore`. Copying the entire bus is not a coherent snapshot. PDO value helpers use checked `memcpy` to handle byte alignment; compatibility typed-pointer helpers additionally require alignment and leave synchronization to the caller. Invalid access throws; valid access does not allocate or log.

## Deployment contract

Exactly one master and one writable App per bus ID; the startup/service manager must enforce the App singleton and refuse deployment if it cannot. Each App thread must use that same client object/mutex. Other tools are strictly read-only. This is not a multi-writer access-control protocol.

The v2 IPC names are `/ecm_v2_{id}`, `/pd_input_v2_{id}`, `/pd_output_v2_{id}`, `/sync_v2_{id}_{slot}`. Old executables cannot connect. Owner `flock` prevents a second live master from truncating objects. It does **not** authorize restarting over still-mapped clients: after either side crashes, stop the other side, close old mappings, then restart both. No hot takeover, dual writing, bridge, queue or session registry is implemented.

Before deployment run backend hardware qualification for identity/firmware, lost slaves, DC state/staleness, SDO abort/timeouts, state coordination and cyclic jitter. The generic library test passes are not this qualification. See [ABI and synchronization specification](docs/abi-v2.md).
