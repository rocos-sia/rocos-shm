// Copyright 2026, Yang Luo
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "abi.hpp"
#include <cstring>
#include <ctime>
#include <limits>
namespace rocos {
// GCC/Clang generic builtins also support double without aliasing violations.
// All dynamic shared scalars must use these helpers on BOTH sides.
template<class T> inline T atomicLoad(const T& value) noexcept {
    static_assert(std::is_arithmetic<T>::value, "scalar only");
    T result; __atomic_load(&value, &result, __ATOMIC_ACQUIRE); return result;
}
template<class T, class V> inline void atomicStore(T& value, V input) noexcept {
    static_assert(std::is_arithmetic<T>::value, "scalar only");
    T copy = static_cast<T>(input); __atomic_store(&value, &copy, __ATOMIC_RELEASE);
}
inline bool atomicsLockFree() noexcept {
    return __atomic_always_lock_free(1, nullptr) && __atomic_always_lock_free(4, nullptr) &&
           __atomic_always_lock_free(8, nullptr);
}
inline uint64_t monotonicNs() noexcept {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec)*1000000000ULL + ts.tv_nsec;
}
inline bool terminalStatus(uint32_t s) noexcept {
    return s==BOX_SUCCESS || s==BOX_FAILED || s==BOX_TIMEOUT_UNKNOWN;
}
inline bool validPdo(const PdVar& v, size_t bytes) noexcept {
    return v.offset>=0 && v.size>0 && static_cast<size_t>(v.offset)<=bytes &&
           static_cast<size_t>(v.size)<=bytes-static_cast<size_t>(v.offset);
}
inline bool setPdoBits(PdVar& v, int64_t bitOffset, int64_t bitSize, size_t bytes) noexcept {
    if (bitOffset<0 || bitSize<=0 || bitOffset%8 || bitSize%8 ||
        bitOffset/8>INT32_MAX || bitSize/8>INT32_MAX) return false;
    PdVar candidate=v; candidate.offset=static_cast<int>(bitOffset/8); candidate.size=static_cast<int>(bitSize/8);
    if (!validPdo(candidate,bytes)) return false;
    v.offset=candidate.offset; v.size=candidate.size; return true;
}
// Copy payload only under protocol ownership; status is always accessed atomically.
inline void copyCommandPayload(AsyncCommandBox& dst, const AsyncCommandBox& src) noexcept {
    std::memcpy(static_cast<void*>(&dst), &src, offsetof(AsyncCommandBox,status));
}
inline int32_t validateCommand(const AsyncCommandBox& req, const ShmHeader& h,
                               const EcatBus& bus, uint64_t now, uint32_t maxMs) noexcept {
    if (req.instance_id!=h.instance_id) return STALE_INSTANCE;
    if (!req.cmd_id || req.reserved || req.cmd_type==CMD_NONE) return INVALID_ARGUMENT;
    if (req.cmd_type>CMD_RESET_STATS || !(bus.supported_cmds & (1u<<req.cmd_type))) return UNSUPPORTED;
    if (req.deadline_mono_ns<=now) return DEADLINE_EXCEEDED;
    if (req.deadline_mono_ns-now>uint64_t(maxMs)*1000000ULL) return INVALID_ARGUMENT;
    if (req.cmd_type==CMD_RESET_STATS)
        return req.target_slave==-1 && req.data_size==0 ? RESULT_OK : INVALID_ARGUMENT;
    if (req.target_slave<0 || req.target_slave>=bus.slave_num || req.target_slave>=MAX_SLAVE_NUM ||
        !req.data_size || req.data_size>MAX_CMD_DATA_SIZE) return INVALID_ARGUMENT;
    return RESULT_OK; // Live identity and state validation belongs to the backend.
}
} // namespace rocos
