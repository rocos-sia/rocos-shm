# Development rules

- This repository is the sole source of ROCOS SHM ABI v2 and shared access/ownership logic; consumers pin a commit. Never copy an edited consumer header back without reviewing its ABI and synchronization consequences.
- C++11, Linux x86_64, CMake >=3.10, header-only INTERFACE target `rocos::shm`. No SDK, App logger, Boost or YAML dependencies. Preserve GPL notices and the `EcatConfigMaster`/`EcatConfig` aliases.
- `abi.hpp` contains only constants and fixed shared layouts. No pointers, STL containers, process-local mutexes, `std::atomic` objects, or `pack(1)` in mapped structures. New fields use fixed-width integers. Preserve old fields' order; macro capacities are frozen.
- `SHM_ABI_VERSION` and `sizeof`/`alignof`/`offsetof` assertions must change together for incompatible layouts; package semver and product semver are separate. Reserved bytes are zero until assigned by a new ABI. Both ends must be rebuilt and coordinated deployment must reject mismatched layouts.
- `access.hpp` owns atomic helpers. All live scalars use these helpers on both sides; mailbox payload copying requires ownership from acquire/release status transitions. No whole-bus coherent-snapshot claim.
- Preserve owner flock-before-truncate and unlink-only-owned semantics; no global `/dev/shm` cleanup. Do not test against production IDs. Stop peer processes before reclaiming crashed owners' objects.
- Command processing is non-RT; preserve one writable App, one in-flight transaction, no timeout reset or blind write retry. A backend must finish using private buffers before publishing a terminal result. Unknown outcome is distinct from confirmed failure.
- Run the documented CMake build and filtered `rocos_shm_v2` CTest for changes. Tests must include independent process mappings, error paths and ABI rejection. Do not run EtherCAT, load drivers or control devices for routine validation.
- Repository version is in CMakeLists.txt. Update docs and tests with API/ABI changes. Do not create a remote repository, push, publish or move tags without user authorization.
