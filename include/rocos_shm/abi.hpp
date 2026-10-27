// Copyright 2026, Yang Luo
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>
#if !defined(__linux__) || !defined(__x86_64__)
# error "ROCOS SHM ABI v2 requires Linux x86_64"
#endif
#ifndef EC_SHM
#  define EC_SHM          "ecm_v2_"
#endif
#ifndef EC_SEM_MUTEX
#  define EC_SEM_MUTEX    "sync_v2_"
#endif
#ifndef EC_SEM_NUM
#  define EC_SEM_NUM      10
#endif
#ifndef EC_SHM_MAX_SIZE
#  define EC_SHM_MAX_SIZE 5242880  // 5 MB
#endif

#ifndef MAX_SLAVE_NUM
#  define MAX_SLAVE_NUM      50
#endif
#ifndef MAX_PDINPUT_NUM
#  define MAX_PDINPUT_NUM    25
#endif
#ifndef MAX_PDOUTPUT_NUM
#  define MAX_PDOUTPUT_NUM   25
#endif
#ifndef MAX_PD_NAME_LEN
#  define MAX_PD_NAME_LEN    72
#endif
#ifndef MAX_SLAVE_NAME_LEN
#  define MAX_SLAVE_NAME_LEN 80
#endif

#ifndef ECAT_STATE_INIT
#  define ECAT_STATE_INIT      1
#  define ECAT_STATE_PREOP     2
#  define ECAT_STATE_SAFEOP    4
#  define ECAT_STATE_OP        8
#  define ECAT_STATE_BOOTSTRAP 3
#endif


static_assert(MAX_SLAVE_NUM == 50 && MAX_PDINPUT_NUM == 25 && MAX_PDOUTPUT_NUM == 25 &&
              MAX_PD_NAME_LEN == 72 && MAX_SLAVE_NAME_LEN == 80 && EC_SEM_NUM == 10,
              "ABI v2 capacities cannot be overridden");
namespace rocos {
constexpr uint32_t SHM_ABI_VERSION = 2;
constexpr uint32_t MAX_CMD_DATA_SIZE = 64;
constexpr uint32_t DEFAULT_COMMAND_TIMEOUT_MS = 1000;
enum AsyncCmdType : uint32_t {
    CMD_NONE=0, CMD_SDO_READ=1, CMD_SDO_WRITE=2, CMD_RESET_STATS=3, CMD_RESET_FAULT=4
};
enum AsyncCmdStatus : uint32_t {
    BOX_IDLE=0, BOX_REQUESTED=1, BOX_PROCESSING=2, BOX_SUCCESS=3, BOX_FAILED=4, BOX_TIMEOUT_UNKNOWN=5
};
// Stable protocol numbers; never expose errno or SDK codes as result_code.
enum CommandResult : int32_t {
    RESULT_OK=0, INVALID_ARGUMENT=1, UNSUPPORTED=2, BUSY=3, STALE_INSTANCE=4,
    SLAVE_OFFLINE=5, STATE_MISMATCH=6, BUFFER_TOO_SMALL=7, DEADLINE_EXCEEDED=8,
    BACKEND_ERROR=9, OUTCOME_UNKNOWN=10
};
struct AsyncCommandBox {
    uint64_t cmd_id{0};
    uint64_t instance_id{0};
    uint64_t deadline_mono_ns{0};
    uint32_t cmd_type{CMD_NONE};
    int32_t target_slave{-1};
    uint16_t sdo_index{0};
    uint8_t sdo_subindex{0};
    uint8_t reserved{0};
    uint32_t data_size{0};
    uint8_t data[MAX_CMD_DATA_SIZE]{};
    int32_t result_code{0};
    uint32_t backend_error{0};
    uint32_t sdo_abort_code{0};
    uint32_t error_valid{0};
    alignas(4) uint32_t status{BOX_IDLE};
};
struct PdVar {
    char     name[MAX_PD_NAME_LEN]{'\0'}; ///< Variable name (null-terminated).
    int      offset{-1};                  ///< Byte offset into the PDO buffer.
    int      size{-1};                    ///< Size in bytes (must equal sizeof(T)).
    uint16_t index{0};                    ///< PDO object index.
    uint8_t  sub_index{0};                ///< PDO object sub-index.
};

/**
 * @brief Cross-process descriptor of one slave and its PDO variables.
 */
struct Slave {
    int  id{-1};                          ///< Slave index.
    char name[MAX_SLAVE_NAME_LEN]{'\0'};  ///< Slave name (null-terminated).

    int input_var_num{0};                 ///< Number of valid input_vars entries.
    int output_var_num{0};                ///< Number of valid output_vars entries.

    PdVar input_vars[MAX_PDINPUT_NUM];    ///< Input (TxPDO) variables.
    PdVar output_vars[MAX_PDOUTPUT_NUM];  ///< Output (RxPDO) variables.
    uint32_t vendor_id{0};
    uint32_t product_code{0};
    uint32_t revision_number{0};
    uint32_t serial_number{0};
    char firmware_version[64]{};
    uint32_t info_valid{0};
    uint32_t identity_source{0};
    uint32_t online{0};
    uint32_t al_state{0};
    uint32_t al_status_code{0};
    uint32_t runtime_valid{0};
};

/**
 * @brief Cross-process bus snapshot shared between master and clients.
 *
 * This is an ABI type: do not change field order, types, or array sizes.
 */
struct EcatBus {
    long   timestamp{0}; ///< Last cycle Unix wall-clock timestamp [us].

    double min_cycle_time{0.0};      ///< Shortest observed cycle [us].
    double max_cycle_time{0.0};      ///< Longest observed cycle [us].
    double avg_cycle_time{0.0};      ///< Running average cycle [us].
    double current_cycle_time{0.0};  ///< Most recent cycle [us].

    uint32_t dt{0}; ///< Control cycle period [microseconds], e.g. 1000 = 1 kHz.
    
    bool   resetCycleTime{false}; ///< Client request to reset cycle statistics.

    int    current_state{ECAT_STATE_INIT}; ///< Current bus state.
    int    request_state{ECAT_STATE_OP};   ///< Client-requested state.
    int    next_expected_state{};          ///< Internal use only.

    bool   is_authorized{false}; ///< Runtime license result; IGH sets true.

    int    slave_num{0};          ///< Number of valid slaves entries.
    Slave  slaves[MAX_SLAVE_NUM]; ///< Slave descriptors.
    char ecm_version[32]{};
    uint32_t backend_type{0};
    uint32_t supported_cmds{0};
    uint32_t state_control_supported{0};
    uint64_t heartbeat_mono_ns{0};
    uint32_t connected_slave_num{0};
    uint32_t link_up{0};
    uint32_t wc_state{0};
    uint32_t status_valid{0};
    uint32_t dc_sync_status{0};
    uint32_t dc_metric_kind{0};
    int64_t dc_diff_ns{0};
    uint64_t dc_sample_mono_ns{0};
    AsyncCommandBox cmd_box;

};


struct ShmHeader {
    char magic[48]{}; // "ROCOSHM2" in first 8 bytes; remaining 40 reserved, zero.
    uint32_t abi_version{SHM_ABI_VERSION};
    uint32_t header_size{80};
    uint32_t bus_size{sizeof(EcatBus)};
    uint32_t input_bytes{0};
    uint32_t output_bytes{0};
    uint32_t ready{0}; // acquire/release only
    uint64_t instance_id{0};
};
struct SharedMemoryV2 { ShmHeader header; EcatBus bus; };
static_assert(sizeof(int)==4 && sizeof(long)==8 && sizeof(bool)==1 && sizeof(double)==8, "Unsupported ABI");
static_assert(sizeof(PdVar)==84 && alignof(PdVar)==4 && offsetof(PdVar,index)==80, "PdVar ABI");
static_assert(sizeof(Slave)==4396 && offsetof(Slave,input_vars)==92 && offsetof(Slave,output_vars)==2192 && offsetof(Slave,vendor_id)==4292 && offsetof(Slave,runtime_valid)==4392, "Slave ABI");
static_assert(sizeof(AsyncCommandBox)==128 && alignof(AsyncCommandBox)==8 && offsetof(AsyncCommandBox,data)==40 && offsetof(AsyncCommandBox,status)==120, "Mailbox ABI");
static_assert(sizeof(ShmHeader)==80 && alignof(ShmHeader)==8 && offsetof(ShmHeader,abi_version)==48 && offsetof(ShmHeader,ready)==68 && offsetof(ShmHeader,instance_id)==72, "Header ABI");
static_assert(offsetof(EcatBus,slaves)==68 && offsetof(EcatBus,ecm_version)==219868 && offsetof(EcatBus,cmd_box)==219960 && sizeof(EcatBus)==220088, "Bus ABI");
static_assert(offsetof(SharedMemoryV2,bus)==80 && sizeof(SharedMemoryV2)==220168, "Region ABI");
static_assert(std::is_standard_layout<SharedMemoryV2>::value && std::is_trivially_copyable<SharedMemoryV2>::value, "Shared layout must be POD-like");
} // namespace rocos
