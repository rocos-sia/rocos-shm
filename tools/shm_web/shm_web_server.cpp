// =============================================================================
// shm_web_server.cpp —— ROCOS 共享内存观察器（Web 后端）
// -----------------------------------------------------------------------------
// 一个零依赖的静态 HTTP 服务器，复用 SharedMemoryConfig 的客户端路径连接到
// 主站进程创建的 POSIX 共享内存（ecm_v2_{id} / pd_input_v2_{id} / pd_output_v2_{id}），
// 把总线快照与 PDO 变量值以 JSON 形式暴露给浏览器前端，并用 Server-Sent
// Events 周期性推送，实现"动态显示"。
//
// 用法：
//   shm_web_server [--master-id N] [--port P] [--host H] [--demo]
//     --master-id  要观察的 master_id（默认 0）
//     --port       监听端口（默认 8484）
//     --host       绑定地址（默认 127.0.0.1，仅本机可访问）
//     --demo       自建一份共享内存并模拟数据（无硬件/主站也能看效果）
//
// 端点：
//   GET /              前端页面（index.html，从可执行文件旁加载）
//   GET /api/snapshot  当前共享内存快照（JSON）
//   GET /api/events    SSE 流，周期推送快照
//   POST /api/mode, /api/state, /api/command, /api/output: explicit writer session
// =============================================================================

#include <rocos_shm/shared_memory_config.hpp>

#include <arpa/inet.h>
#include <chrono>
#include <array>
#include <atomic>
#include <condition_variable>
#include <set>
#include <charconv>
#include <map>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

using rocos::EcatBus;
using rocos::PdVar;
using rocos::Slave;
using rocos::SharedMemoryConfig;

// -----------------------------------------------------------------------------
// 常量
// -----------------------------------------------------------------------------
constexpr int kDefaultPort    = 8484;
constexpr int kPushIntervalMs = 250;   // SSE 推送间隔
constexpr int kReconnectMs    = 1000;  // 未连接时的重试间隔
constexpr int kStaleMs        = 3000;  // 判定主站已退出的时间阈值

// -----------------------------------------------------------------------------
// 应用状态：被所有连接线程共享
// -----------------------------------------------------------------------------
struct AppState {
    int master_id = 0;
    bool demo     = false;
    bool demo_output_written = false;

    std::mutex mtx;
    std::unique_ptr<SharedMemoryConfig> cfg;  // 客户端连接（或 demo 模式下自建）
    bool connected = false;
    // Retain the client across mode toggles so its command IDs never restart.
    std::unique_ptr<SharedMemoryConfig> writer;
    bool write_enabled = false;
    bool command_pending = false;
    bool recovery_required = false;
    bool has_command = false;
    rocos::AsyncCommandBox command_request{}, command_result{};
    std::string write_session;
    std::string write_owner;
    int writer_lock = -1;
    ~AppState() { if (writer_lock >= 0) ::close(writer_lock); }

    std::chrono::steady_clock::time_point last_attempt{};  // 上次尝试连接时刻
    std::uint64_t last_ts = 0;                            // 上一次观察到的单调心跳
    std::chrono::steady_clock::time_point last_seen_ts{};  // 上次看到心跳更新的时刻
};

// -----------------------------------------------------------------------------
// 基础工具
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// 全局退出标志：信号处理函数置位，主循环据此关闭监听并退出。
// -----------------------------------------------------------------------------
volatile std::sig_atomic_t g_shutdown = 0;
void handleSignal(int) { g_shutdown = 1; }

void ignoreSigpipe() {
    std::signal(SIGPIPE, SIG_IGN);
}

// 安装 SIGINT/SIGTERM 处理器（用于干净退出、清理 demo 共享内存）。
// 用 sigaction 且不设置 SA_RESTART，保证 accept() 能被打断返回 EINTR，
// 从而主循环能观察到 g_shutdown 并退出。
void installSignalHandlers() {
    struct sigaction sa {};
    sa.sa_handler = handleSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // 显式不带 SA_RESTART
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// JSON 字符串转义（名称来自共享内存的 char 数组）。
std::string jsonEscape(const std::string &s) {
    std::ostringstream out;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b";  break;
            case '\f': out << "\\f";  break;
            case '\n': out << "\\n";  break;
            case '\r': out << "\\r";  break;
            case '\t': out << "\\t";  break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    return out.str();
}

bool sendAll(int fd, const std::string &data) {
    size_t offset = 0;
    while (offset < data.size()) {
        const auto n = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        offset += static_cast<size_t>(n);
    }
    return true;
}

std::string jsonNumber(double value) {
    if (!std::isfinite(value)) return "null";
    std::ostringstream out;
    out << std::setprecision(17) << value;
    return out.str();
}

std::string cstr(const char *p, std::size_t cap) {
    return std::string(p, strnlen(p, cap));
}

const char *stateName(int state) {
    switch (state) {
        case 1: return "INIT";
        case 2: return "PREOP";
        case 3: return "BOOTSTRAP";
        case 4: return "SAFEOP";
        case 8: return "OP";
        default: return "?";
    }
}

// 读取并十六进制编码一段 PDO 数据。
std::string hexBytes(const void *base, int offset, int size) {
    std::ostringstream out;
    if (base == nullptr || offset < 0 || size <= 0) return "";
    const auto *p = static_cast<const unsigned char *>(base) + offset;
    for (int i = 0; i < size; ++i) {
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(p[i]);
    }
    return out.str();
}

// -----------------------------------------------------------------------------
// demo 模式：自建共享内存并模拟一个 EtherCAT 驱动
// -----------------------------------------------------------------------------
// 独立演示用的伺服 PDO 布局：
//   输出 13 B:  TargetPosition[4] TargetVelocity[4] TargetTorque[2]
//               ControlWord[2] ModesOfOperation[1]
//   输入 18 B:  StatusWord[2] Position[4] Velocity[4] Torque[2]
//               AuxPosition[4] AnalogInput[2]
struct DemoVar {
    const char *name;
    int  offset;
    int  size;
    int  index;
    int  sub_index;
};

constexpr DemoVar kDemoOutputVars[] = {
    {"Target Position",       0,  4, 0x607A, 0x00},
    {"Target Velocity",       4,  4, 0x60FF, 0x00},
    {"Target Torque",         8,  2, 0x6071, 0x00},
    {"Control Word",         10,  2, 0x6040, 0x00},
    {"Modes of Operation",   12,  1, 0x6060, 0x00},
};
constexpr DemoVar kDemoInputVars[] = {
    {"Status Word",              0,  2, 0x6041, 0x00},
    {"Position Actual Value",    2,  4, 0x6064, 0x00},
    {"Velocity Actual Value",    6,  4, 0x606C, 0x00},
    {"Torque Actual Value",     10,  2, 0x6077, 0x00},
    {"Auxiliary Position Value",12,  4, 0x20A0, 0x00},
    {"Analog Input",            16,  2, 0x2205, 0x02},
};

void populateDemoBus(EcatBus &bus) {
    bus = EcatBus{};
    bus.dt             = 1000;
    bus.current_state  = ECAT_STATE_OP;
    bus.request_state  = ECAT_STATE_OP;
    bus.next_expected_state = ECAT_STATE_OP;
    bus.is_authorized  = true;
    bus.slave_num      = 1;
    std::strcpy(bus.ecm_version, "shm-web-demo");
    bus.supported_cmds = (1u << rocos::CMD_SDO_READ) | (1u << rocos::CMD_SDO_WRITE) | (1u << rocos::CMD_RESET_STATS);
    bus.state_control_supported = 1;
    bus.connected_slave_num = 1;
    bus.link_up = 1;
    bus.wc_state = 1;
    bus.status_valid = 7;

    Slave &slave = bus.slaves[0];
    slave.id     = 0;
    slave.vendor_id = 0x1234;
    slave.product_code = 0x5678;
    slave.revision_number = 0x10000;
    slave.serial_number = 42;
    std::strcpy(slave.firmware_version, "demo-1.0");
    slave.info_valid = 31;
    slave.identity_source = 2;
    slave.online = 1;
    slave.al_state = ECAT_STATE_OP;
    slave.runtime_valid = 7;
    std::strncpy(slave.name, "EtherCAT Drive (demo)", MAX_SLAVE_NAME_LEN - 1);
    slave.name[MAX_SLAVE_NAME_LEN - 1] = '\0';

    auto copyVar = [](PdVar &dst, const DemoVar &v) {
        std::strncpy(dst.name, v.name, MAX_PD_NAME_LEN - 1);
        dst.name[MAX_PD_NAME_LEN - 1] = '\0';
        dst.offset    = v.offset;
        dst.size      = v.size;
        dst.index     = v.index;
        dst.sub_index = v.sub_index;
    };

    int out_count = 0, in_count = 0;
    for (const auto &v : kDemoOutputVars) copyVar(slave.output_vars[out_count++], v);
    for (const auto &v : kDemoInputVars)  copyVar(slave.input_vars[in_count++], v);
    slave.output_var_num = out_count;
    slave.input_var_num  = in_count;
}

bool createDemoMemory(AppState &st, std::string &why) {
    try {
        st.cfg = std::make_unique<SharedMemoryConfig>(st.master_id);
    } catch (const std::exception &) {
        why = "invalid master id";
        return false;
    }
    if (!st.cfg->createSharedMemory()) { why = "createSharedMemory failed"; return false; }
    if (!st.cfg->createPdDataMemoryProvider(18, 13)) { why = "createPdDataMemoryProvider failed"; return false; }
    populateDemoBus(*st.cfg->ecatBus);
    if (!st.cfg->publishReady()) { why = st.cfg->lastError(); return false; }
    return true;
}

// 每一帧修改模拟的输入/输出 PDO（位置斜坡、速度正弦等做动画）。
void animateDemo(AppState &st) {
    EcatBus *bus = st.cfg->ecatBus;
    static long n = 0;
    ++n;

    // 模拟主站运行：时间戳递增 + 周期统计。
    timespec wall{};
    clock_gettime(CLOCK_REALTIME, &wall);
    rocos::atomicStore(bus->timestamp, wall.tv_sec * 1000000L + wall.tv_nsec / 1000L);
    rocos::atomicStore(bus->heartbeat_mono_ns, rocos::monotonicNs());
    rocos::atomicStore(bus->current_cycle_time, 1000.0);
    rocos::atomicStore(bus->min_cycle_time, 980.0);
    rocos::atomicStore(bus->max_cycle_time, 1120.0);
    rocos::atomicStore(bus->avg_cycle_time, 1002.0);

    auto *in  = static_cast<unsigned char *>(st.cfg->pdInputPtr);
    auto *out = static_cast<unsigned char *>(st.cfg->pdOutputPtr);

    auto put32le = [](unsigned char *base, int off, int32_t v) {
        base[off]     = static_cast<unsigned char>(v & 0xff);
        base[off + 1] = static_cast<unsigned char>((v >> 8) & 0xff);
        base[off + 2] = static_cast<unsigned char>((v >> 16) & 0xff);
        base[off + 3] = static_cast<unsigned char>((v >> 24) & 0xff);
    };
    auto put16le = [](unsigned char *base, int off, int16_t v) {
        base[off]     = static_cast<unsigned char>(v & 0xff);
        base[off + 1] = static_cast<unsigned char>((v >> 8) & 0xff);
    };
    auto put8 = [](unsigned char *base, int off, int8_t v) {
        base[off] = static_cast<unsigned char>(v);
    };

    int32_t pos = static_cast<int32_t>(n * 100);
    int32_t vel = static_cast<int32_t>(2000 * std::sin(double(n) / 20));

    // 输入：
    put16le(in,  0, 0x0237);             // Status Word
    put32le(in,  2, pos);                // Position Actual Value
    put32le(in,  6, vel);                // Velocity Actual Value
    put16le(in, 10, 1000);               // Torque Actual Value
    put32le(in, 12, pos / 10);           // Auxiliary Position Value
    put16le(in, 16, static_cast<int16_t>(n % 1000));  // Analog Input

    if (st.demo_output_written) return;  // 保留用户写入的输出，输入动画继续。

    // 输出（真实场景由客户端写入，这里写出跟随值演示回读）：
    put32le(out,  0, pos);               // Target Position
    put32le(out,  4, vel);               // Target Velocity
    put16le(out,  8, 750);               // Target Torque
    put16le(out, 10, 0x0006);            // Control Word
    put8(out,    12, 8);                 // Modes of Operation (csp)
}

// Simulated non-RT backend: only the advertised PDO dictionary is implemented.
void processDemoCommand(AppState &st) {
    if (!st.demo || !st.cfg) return;
    auto &bus = *st.cfg->ecatBus;
    rocos::atomicStore(bus.current_state, rocos::atomicLoad(bus.request_state));
    rocos::atomicStore(bus.next_expected_state, rocos::atomicLoad(bus.request_state));
    rocos::atomicStore(bus.slaves[0].al_state, rocos::atomicLoad(bus.current_state));
    st.cfg->processCommand([&](const rocos::AsyncCommandBox &req, rocos::AsyncCommandBox &result) -> int32_t {
        if (req.cmd_type == rocos::CMD_RESET_STATS) {
            rocos::atomicStore(bus.min_cycle_time, 0.0);
            rocos::atomicStore(bus.max_cycle_time, 0.0);
            rocos::atomicStore(bus.avg_cycle_time, 0.0);
            return rocos::RESULT_OK;
        }
        const auto &slave = bus.slaves[req.target_slave];
        for (int direction = 0; direction < 2; ++direction) {
            const int count = direction ? slave.output_var_num : slave.input_var_num;
            const auto *vars = direction ? slave.output_vars : slave.input_vars;
            auto *base = static_cast<unsigned char *>(direction ? st.cfg->pdOutputPtr : st.cfg->pdInputPtr);
            for (int v = 0; v < count; ++v) {
                if (vars[v].index != req.sdo_index || vars[v].sub_index != req.sdo_subindex) continue;
                if (req.cmd_type == rocos::CMD_SDO_READ) {
                    if (req.data_size < unsigned(vars[v].size)) return rocos::BUFFER_TOO_SMALL;
                    result.data_size = vars[v].size;
                    std::memcpy(result.data, base + vars[v].offset, result.data_size);
                    return rocos::RESULT_OK;
                }
                if (!direction || req.data_size != unsigned(vars[v].size)) {
                    result.error_valid = 2;
                    result.sdo_abort_code = !direction ? 0x06010002 : 0x06070010;
                    return rocos::BACKEND_ERROR;
                }
                std::memcpy(base + vars[v].offset, req.data, req.data_size);
                st.demo_output_written = true;
                return rocos::RESULT_OK;
            }
        }
        result.error_valid = 2;
        result.sdo_abort_code = 0x06020000;
        return rocos::BACKEND_ERROR;
    });
}

// Mailbox payloads are copied only by the owning client, never by the observer.
std::string commandJson(const rocos::AsyncCommandBox &box) {
    std::ostringstream o;
    o << "{\"cmd_id\":\"" << box.cmd_id << "\",\"instance_id\":\"" << box.instance_id
      << "\",\"deadline_mono_ns\":\"" << box.deadline_mono_ns << "\",\"cmd_type\":" << box.cmd_type
      << ",\"target_slave\":" << box.target_slave << ",\"sdo_index\":" << box.sdo_index
      << ",\"sdo_subindex\":" << unsigned(box.sdo_subindex) << ",\"data_size\":" << box.data_size
      << ",\"data\":\"" << hexBytes(box.data, 0, std::min(box.data_size, rocos::MAX_CMD_DATA_SIZE))
      << "\",\"result_code\":" << box.result_code << ",\"backend_error\":" << box.backend_error
      << ",\"sdo_abort_code\":" << box.sdo_abort_code << ",\"error_valid\":" << box.error_valid
      << ",\"status\":" << box.status << "}";
    return o.str();
}

void revokeWrites(AppState &st) {
    st.write_enabled = false;
    st.write_session.clear();
    st.write_owner.clear();
    if (st.command_pending) st.recovery_required = true;
}

void pollOwnedCommand(AppState &st) {
    if (!st.command_pending || !st.writer) return;
    rocos::AsyncCommandBox result;
    const auto rc = st.writer->pollCommand(result);
    if (rc == rocos::BUSY && !rocos::terminalStatus(result.status)) return;
    if (!rocos::terminalStatus(result.status)) {
        st.recovery_required = true;
        revokeWrites(st);
        return;
    }
    st.command_result = result;
    st.command_pending = false;
}

// -----------------------------------------------------------------------------
// 快照渲染：把 EcatBus + PDO 值序列化为 JSON
// -----------------------------------------------------------------------------
std::string renderSnapshot(AppState &st) {
    std::lock_guard<std::mutex> lk(st.mtx);
    const auto now = std::chrono::steady_clock::now();

    if (!st.cfg) {
        try {
            st.cfg = std::make_unique<SharedMemoryConfig>(st.master_id);
        } catch (...) {
            st.cfg.reset();
        }
    }

    if (!st.connected) {
        const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - st.last_attempt);
        if (st.demo) {
            st.connected = true;
        } else if (since_last.count() >= kReconnectMs) {
            st.last_attempt = now;
            if (st.cfg && st.cfg->connect(true)) {
                st.connected = true;
                st.last_ts = rocos::atomicLoad(st.cfg->ecatBus->heartbeat_mono_ns);
                st.last_seen_ts = now;
            }
        }
    } else if (!st.demo && st.cfg && st.cfg->ecatBus != nullptr) {
        // 存活检测：单调心跳长时间没变，判定主站已退出，丢弃映射以便重连。
        if (rocos::atomicLoad(st.cfg->header->ready) != 1U) {
            revokeWrites(st);
            st.cfg.reset();
            st.connected = false;
        } else if (rocos::atomicLoad(st.cfg->ecatBus->heartbeat_mono_ns) != st.last_ts) {
            st.last_ts = rocos::atomicLoad(st.cfg->ecatBus->heartbeat_mono_ns);
            st.last_seen_ts = now;
        } else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - st.last_seen_ts).count() > kStaleMs) {
            revokeWrites(st);
            st.cfg.reset();
            st.connected = false;
        }
    }

    std::ostringstream o;
    if (!st.connected || !st.cfg || st.cfg->ecatBus == nullptr) {
        o << "{\"master_id\":" << st.master_id
          << ",\"available\":false,"
          << "\"demo\":" << (st.demo ? "true" : "false")
          << ",\"error\":\"no shared memory for master " << st.master_id << "\"}";
        return o.str();
    }

    pollOwnedCommand(st);
    const EcatBus *bus = st.cfg->ecatBus;
    o << "{"
      << "\"master_id\":" << st.master_id << ","
      << "\"available\":true,"
      << "\"demo\":" << (st.demo ? "true" : "false") << ","
      << "\"read_only\":" << (st.write_enabled ? "false" : "true") << ","
      << "\"write_owner\":\"" << st.write_owner << "\","
      << "\"command_pending\":" << (st.command_pending ? "true" : "false") << ","
      << "\"recovery_required\":" << (st.recovery_required ? "true" : "false") << ","
      << "\"cmd_box\":{\"status\":" << rocos::atomicLoad(bus->cmd_box.status) << "},"
      << "\"last_command\":" << (st.has_command ? commandJson(st.command_result) : "null") << ","
      << "\"last_request\":" << (st.has_command ? commandJson(st.command_request) : "null") << ","
      << "\"header\":{\"magic\":\"" << jsonEscape(cstr(st.cfg->header->magic, 8))
      << "\",\"abi_version\":" << st.cfg->header->abi_version
      << ",\"header_size\":" << st.cfg->header->header_size << ",\"bus_size\":" << st.cfg->header->bus_size
      << ",\"input_bytes\":" << st.cfg->header->input_bytes << ",\"output_bytes\":" << st.cfg->header->output_bytes
      << ",\"ready\":" << rocos::atomicLoad(st.cfg->header->ready)
      << ",\"instance_id\":\"" << st.cfg->header->instance_id << "\"},"
      << "\"sample_mono_ns\":\"" << rocos::monotonicNs() << "\","
      << "\"backend_type\":" << bus->backend_type << ","
      << "\"ecm_version\":\"" << jsonEscape(cstr(bus->ecm_version, sizeof(bus->ecm_version))) << "\","
      << "\"heartbeat_mono_ns\":\"" << rocos::atomicLoad(bus->heartbeat_mono_ns) << "\","
      << "\"connected_slave_num\":" << rocos::atomicLoad(bus->connected_slave_num) << ","
      << "\"link_up\":" << rocos::atomicLoad(bus->link_up) << ","
      << "\"wc_state\":" << rocos::atomicLoad(bus->wc_state) << ","
      << "\"status_valid\":" << rocos::atomicLoad(bus->status_valid) << ","
      << "\"state_control_supported\":" << bus->state_control_supported << ","
      << "\"supported_cmds\":" << bus->supported_cmds << ","
      << "\"dc_sync_status\":" << rocos::atomicLoad(bus->dc_sync_status) << ","
      << "\"dc_metric_kind\":" << rocos::atomicLoad(bus->dc_metric_kind) << ","
      << "\"dc_diff_ns\":\"" << rocos::atomicLoad(bus->dc_diff_ns) << "\","
      << "\"dc_sample_mono_ns\":\"" << rocos::atomicLoad(bus->dc_sample_mono_ns) << "\","
      << "\"timestamp\":" << rocos::atomicLoad(bus->timestamp) << ","
      << "\"dt\":" << rocos::atomicLoad(bus->dt) << ","
      << "\"current_state\":" << rocos::atomicLoad(bus->current_state) << ","
      << "\"current_state_name\":\"" << stateName(rocos::atomicLoad(bus->current_state)) << "\","
      << "\"request_state\":" << rocos::atomicLoad(bus->request_state) << ","
      << "\"next_expected_state\":" << rocos::atomicLoad(bus->next_expected_state) << ","
      << "\"is_authorized\":" << (rocos::atomicLoad(bus->is_authorized) ? "true" : "false") << ","
      << "\"resetCycleTime\":" << (rocos::atomicLoad(bus->resetCycleTime) ? "true" : "false") << ","
      << "\"cycle\":{\"min\":" << jsonNumber(rocos::atomicLoad(bus->min_cycle_time))
      << ",\"max\":" << jsonNumber(rocos::atomicLoad(bus->max_cycle_time))
      << ",\"avg\":" << jsonNumber(rocos::atomicLoad(bus->avg_cycle_time))
      << ",\"current\":" << jsonNumber(rocos::atomicLoad(bus->current_cycle_time)) << "},"
      << "\"slave_num\":" << bus->slave_num << ","
      << "\"slaves\":[";

    for (int s = 0; s < static_cast<int>(bus->slave_num) && s < MAX_SLAVE_NUM; ++s) {
        const Slave slave = st.cfg->getSlave(s);
        if (s != 0) o << ",";
        o << "{\"position\":" << s << ",\"id\":" << slave.id
          << ",\"name\":\"" << jsonEscape(cstr(slave.name, MAX_SLAVE_NAME_LEN)) << "\""
          << ",\"online\":" << slave.online
          << ",\"al_state\":" << slave.al_state
          << ",\"al_status_code\":" << slave.al_status_code
          << ",\"runtime_valid\":" << slave.runtime_valid
          << ",\"vendor_id\":" << slave.vendor_id
          << ",\"product_code\":" << slave.product_code
          << ",\"revision_number\":" << slave.revision_number
          << ",\"serial_number\":" << slave.serial_number
          << ",\"info_valid\":" << slave.info_valid
          << ",\"identity_source\":" << slave.identity_source
          << ",\"firmware_version\":\"" << jsonEscape(cstr(slave.firmware_version, sizeof(slave.firmware_version))) << "\""
          << ",\"input_var_num\":" << slave.input_var_num
          << ",\"output_var_num\":" << slave.output_var_num
          << ",\"input_vars\":[";

        for (int i = 0; i < static_cast<int>(slave.input_var_num) && i < MAX_PDINPUT_NUM; ++i) {
            const PdVar &pv = slave.input_vars[i];
            if (i != 0) o << ",";
            o << "{\"name\":\"" << jsonEscape(cstr(pv.name, MAX_PD_NAME_LEN)) << "\""
              << ",\"offset\":" << pv.offset
              << ",\"size\":" << pv.size
              << ",\"index\":\"" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << pv.index << std::dec << "\""
              << ",\"sub_index\":" << static_cast<int>(pv.sub_index)
              << ",\"bytes\":\"" << hexBytes(st.cfg->pdInputPtr, pv.offset, pv.size) << "\"}";
        }
        o << "],\"output_vars\":[";

        for (int i = 0; i < static_cast<int>(slave.output_var_num) && i < MAX_PDOUTPUT_NUM; ++i) {
            const PdVar &pv = slave.output_vars[i];
            if (i != 0) o << ",";
            o << "{\"name\":\"" << jsonEscape(cstr(pv.name, MAX_PD_NAME_LEN)) << "\""
              << ",\"offset\":" << pv.offset
              << ",\"size\":" << pv.size
              << ",\"index\":\"" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << pv.index << std::dec << "\""
              << ",\"sub_index\":" << static_cast<int>(pv.sub_index)
              << ",\"bytes\":\"" << hexBytes(st.cfg->pdOutputPtr, pv.offset, pv.size) << "\"}";
        }
        o << "]}";
    }
    o << "]}";
    return o.str();
}

// -----------------------------------------------------------------------------
// 极简 HTTP 响应
// -----------------------------------------------------------------------------
// keep_alive=true 时不发送 "Connection: close"（SSE 长连接需要）。
// body 为空且 keep_alive 时也省略 Content-Length（SSE 是流式、无固定长度）。
std::string response(int status, const std::string &status_text,
                     const std::string &ctype, const std::string &body,
                     bool keep_alive = false) {
    std::ostringstream o;
    o << "HTTP/1.1 " << status << " " << status_text << "\r\n"
      << "Content-Type: " << ctype << "\r\n";
    if (!(body.empty() && keep_alive)) {
        o << "Content-Length: " << body.size() << "\r\n";
    }
    o << "Cache-Control: no-cache\r\n";
    if (keep_alive) {
        o << "Connection: keep-alive\r\n"
          << "X-Accel-Buffering: no\r\n";
    } else {
        o << "Connection: close\r\n";
    }
    o << "\r\n" << body;
    return o.str();
}

bool loadFrontendAsset(const std::string &filename, std::string &out, const std::string &argv0) {
    std::string dir;
    const auto slash = argv0.find_last_of('/');
    dir = (slash == std::string::npos) ? std::string(".") : argv0.substr(0, slash);
    const std::vector<std::string> candidates = { dir + "/" + filename, "./" + filename, filename };
    for (const auto &path : candidates) {
        std::ifstream f(path, std::ios::binary);
        if (f.good()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            out = ss.str();
            return true;
        }
    }
    return false;
}

bool parseRequest(const std::string &raw, std::string &method, std::string &target) {
    const auto line_end = raw.find("\r\n");
    if (line_end == std::string::npos) return false;
    std::istringstream line(raw.substr(0, line_end));
    line >> method >> target;
    return method == "GET" || method == "POST";
}

bool parseUnsigned(const std::string &text, unsigned int &value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

std::string connectionError(const AppState &st, const std::string &instance) {
    if (!st.connected || !st.cfg || !st.cfg->header || !st.cfg->ecatBus) return "共享内存未连接";
    if (instance != std::to_string(st.cfg->header->instance_id)) return "主站实例已改变，请刷新";
    if (rocos::atomicLoad(st.cfg->header->ready) != 1u) return "主站未就绪";
    const auto heartbeat = rocos::atomicLoad(st.cfg->ecatBus->heartbeat_mono_ns);
    const auto now = rocos::monotonicNs();
    if (!st.demo && (!heartbeat || heartbeat > now || now - heartbeat > uint64_t(kStaleMs) * 1000000))
        return "主站数据已过期";
    return "";
}

std::string writerError(const AppState &st, const std::string &session, const std::string &instance) {
    const auto error = connectionError(st, instance);
    if (!error.empty()) return error;
    if (!st.write_enabled || session.empty() || session != st.write_session) return "只读模式或写入会话不匹配";
    if (st.recovery_required) return "命令结果未确认，需协调停止两端后重启";
    if (!st.writer || !st.writer->header || st.writer->header->instance_id != st.cfg->header->instance_id)
        return "可写客户端实例不匹配";
    if (st.command_pending || rocos::atomicLoad(st.writer->ecatBus->cmd_box.status) != rocos::BOX_IDLE)
        return "已有在途命令或未消费结果";
    return "";
}

std::string randomSession() {
    std::array<unsigned char, 24> bytes{};
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    const auto n = ::read(fd, bytes.data(), bytes.size());
    ::close(fd);
    return n == static_cast<ssize_t>(bytes.size()) ? hexBytes(bytes.data(), 0, bytes.size()) : "";
}

// This lock excludes other shm_web processes. External Apps must be stopped by the operator.
std::string enableWriter(AppState &st, const std::string &instance, std::string &reply) {
    auto error = connectionError(st, instance);
    if (!error.empty()) return error;
    if (st.recovery_required || st.command_pending) return "命令结果未确认，禁止重新获取写权限";
    if (st.write_enabled) return "已有浏览器持有写权限，请在原页面切回只读";
    if (rocos::atomicLoad(st.cfg->ecatBus->cmd_box.status) != rocos::BOX_IDLE) return "邮箱非空，禁止接管或重置";
    if (st.writer_lock < 0) {
        const auto path = "/tmp/rocos-shm-web-" + std::to_string(st.master_id) + ".lock";
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) return "无法打开 Web 写入锁";
        struct stat info{};
        if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1
            || flock(fd, LOCK_EX | LOCK_NB)) { ::close(fd); return "另一 Web 进程占用写入锁或锁文件无效"; }
        st.writer_lock = fd; // Never unlink a lock inode while another process can hold it.
    }
    if (st.writer && st.writer->header->instance_id != st.cfg->header->instance_id) st.writer.reset();
    if (!st.writer) {
        auto writer = std::make_unique<SharedMemoryConfig>(st.master_id);
        if (!writer->connect(false)) return writer->lastError();
        if (writer->header->instance_id != st.cfg->header->instance_id) return "主站实例已改变";
        st.writer = std::move(writer);
    }
    const auto session = randomSession();
    if (session.empty()) return "无法创建写入会话";
    st.write_session = session;
    st.write_owner = randomSession();
    if (st.write_owner.empty()) { st.write_session.clear(); return "无法创建写入标识"; }
    st.write_enabled = true;
    reply = "{\"ok\":true,\"session\":\"" + session + "\",\"owner\":\"" + st.write_owner + "\"}";
    return "";
}

std::string controlRequest(AppState &st, const std::string &path, const std::string &body,
                           const std::string &session, const std::string &instance, std::string &reply) {
    std::lock_guard<std::mutex> lk(st.mtx);
    if (path == "/api/mode") {
        if (body == "enable sole-writer") return enableWriter(st, instance, reply);
        if (body != "disable") return "模式参数无效；启用需确认已停止其他可写 App";
        if (session.empty() || session != st.write_session) return "写入会话不匹配";
        if (!st.writer || instance != std::to_string(st.writer->header->instance_id)) return "主站实例不匹配";
        // Stop new writes immediately, but retain and drain any outstanding transaction.
        st.write_enabled = false;
        st.write_session.clear();
        st.write_owner.clear();
        return "";
    }
    auto error = writerError(st, session, instance);
    if (!error.empty()) return error;
    auto &cfg = *st.writer;
    if (path == "/api/state") {
        unsigned int state = 0;
        if (!parseUnsigned(body, state) || (state != 1 && state != 2 && state != 3 && state != 4 && state != 8))
            return "无效主站状态";
        if (!cfg.ecatBus->state_control_supported) return "当前后端不支持主站状态切换";
        cfg.setBusRequestState(state);
        return "";
    }
    if (path != "/api/command") return "未知操作";
    // type position index subindex capacity hex; '-' is used when no data is sent.
    std::istringstream input(body);
    std::array<unsigned int, 5> fields{};
    std::string token, hex, trailing;
    for (auto &field : fields) if (!(input >> token) || !parseUnsigned(token, field)) return "命令参数无效";
    if (!(input >> hex) || (input >> trailing)) return "命令参数无效";
    const auto [type, position, index, sub, size] = fields;
    if (type < rocos::CMD_SDO_READ || type > rocos::CMD_RESET_STATS || !(cfg.ecatBus->supported_cmds & (1u << type)))
        return "当前后端不支持此命令";
    rocos::AsyncCommandBox req, result;
    req.cmd_type = type;
    if (type == rocos::CMD_RESET_STATS) {
        if (position || index || sub || size || hex != "-") return "统计复位参数无效";
    } else {
        if (position >= static_cast<unsigned>(cfg.ecatBus->slave_num) || position >= MAX_SLAVE_NUM
            || !index || index > 0xffff || sub > 0xff || !size || size > rocos::MAX_CMD_DATA_SIZE)
            return "SDO 目标或长度无效（1–64 字节）";
        req.target_slave = position; // Array position, not the display Slave.id.
        req.sdo_index = index;
        req.sdo_subindex = sub;
        req.data_size = size;
        if (type == rocos::CMD_SDO_READ) {
            if (hex != "-") return "SDO 读取不接受发送数据";
        } else {
            if (hex.size() != size * 2) return "SDO 字节长度不匹配";
            for (unsigned i = 0; i < size; ++i) {
                unsigned byte = 0;
                const auto parsed = std::from_chars(hex.data() + i * 2, hex.data() + i * 2 + 2, byte, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != hex.data() + i * 2 + 2) return "SDO 字节无效";
                req.data[i] = byte;
            }
        }
    }
    req.deadline_mono_ns = rocos::monotonicNs() + uint64_t(rocos::DEFAULT_COMMAND_TIMEOUT_MS) * 1000000;
    // Brief non-RT wait; a background worker drains the same client even after the browser closes.
    const auto rc = cfg.executeCommand(req, result, 1);
    if (!result.cmd_id) return "命令未提交：邮箱忙或参数无效";
    (void)rc;
    st.command_pending = !rocos::terminalStatus(result.status);
    st.has_command = true;
    req.cmd_id = result.cmd_id;
    req.instance_id = result.instance_id;
    req.status = rocos::BOX_REQUESTED;
    st.command_request = req;
    st.command_result = result;
    reply = "{\"ok\":true,\"command\":" + commandJson(result) + "}";
    return "";
}

std::string writeOutput(AppState &st, const std::string &body, const std::string &session, const std::string &instance) {
    // Wire format: master slave index(decimal) subindex offset size hexbytes.
    std::istringstream input(body);
    std::array<unsigned int, 6> fields{};
    std::string token, hex, trailing;
    for (auto &field : fields) {
        if (!(input >> token) || !parseUnsigned(token, field)) return "写入参数无效";
    }
    if (!(input >> hex) || (input >> trailing)) return "写入参数无效";
    const auto [master, slave_id, index, sub, offset, size] = fields;
    if (master != static_cast<unsigned int>(st.master_id)) return "主站 ID 不匹配";
    if (slave_id >= MAX_SLAVE_NUM || index == 0 || index > 0xffff || sub > 0xff
        || (!size || size > 64) || hex.size() != size * 2) return "字典地址或长度无效";
    std::array<unsigned char, 64> bytes{};
    for (unsigned int i = 0; i < size; ++i) {
        unsigned int byte = 0;
        const auto parsed = std::from_chars(hex.data() + i * 2, hex.data() + i * 2 + 2, byte, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != hex.data() + i * 2 + 2) return "字节数据无效";
        bytes[i] = static_cast<unsigned char>(byte);
    }

    std::lock_guard<std::mutex> lk(st.mtx);
    const auto error = writerError(st, session, instance);
    if (!error.empty()) return error;
    if (!st.writer->pdOutputPtr) return "OUT 映射不可用";
    const EcatBus &bus = *st.writer->ecatBus;
    if (bus.slave_num < 0 || bus.slave_num > MAX_SLAVE_NUM) return "从站数量无效";
    const PdVar *match = nullptr;
    for (int s = 0; s < bus.slave_num; ++s) {
        const Slave &slave = bus.slaves[s];
        if (slave.id != static_cast<int>(slave_id)) continue;
        if (slave.output_var_num < 0 || slave.output_var_num > MAX_PDOUTPUT_NUM) return "OUT 数量无效";
        for (int v = 0; v < slave.output_var_num; ++v) {
            const PdVar &var = slave.output_vars[v];
            if (var.index == index && var.sub_index == sub && var.offset >= 0
                && static_cast<unsigned int>(var.offset) == offset && var.size == static_cast<int>(size)) {
                if (match) return "OUT 字典地址不唯一";
                match = &var;
            }
        }
    }
    if (!match) return "OUT 字典不存在或配置已改变";
    const auto capacity = st.writer->pd_output_size_;
    if (offset > capacity || size > capacity - offset) return "OUT 写入超出映射范围";
    std::memcpy(static_cast<unsigned char *>(st.writer->pdOutputPtr) + offset, bytes.data(), size);
    st.demo_output_written = true;
    return "";
}

// Read a bounded POST body, rejecting ambiguous framing and cross-origin browser writes.
void handleWriteRequest(int fd, AppState &st, std::string raw, const std::string &path) {
    auto reply = [fd](int status, const std::string &error, const std::string &success = "{\"ok\":true}") {
        const auto body = error.empty() ? success : "{\"error\":\"" + jsonEscape(error) + "\"}";
        const auto r = response(status, status == 200 ? "OK" : "Bad Request", "application/json; charset=utf-8", body);
        (void)sendAll(fd, r);
        close(fd);
    };
    const auto end = raw.find("\r\n\r\n");
    std::map<std::string, std::string> headers;
    std::size_t pos = raw.find("\r\n") + 2;
    while (pos < end) {
        const auto next = raw.find("\r\n", pos);
        const auto colon = raw.find(':', pos);
        if (colon == std::string::npos || colon >= next) { reply(400, "请求头无效"); return; }
        auto name = raw.substr(pos, colon - pos);
        for (char &c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto value = raw.substr(colon + 1, next - colon - 1);
        const auto first = value.find_first_not_of(" \t");
        value = first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t") - first + 1);
        if (!headers.emplace(name, value).second) { reply(400, "重复请求头"); return; }
        pos = next + 2;
    }
    unsigned int length = 0;
    if (headers["x-rocos-write"] != "1" || headers.count("transfer-encoding")
        || !parseUnsigned(headers["content-length"], length) || length == 0 || length > 256) {
        reply(400, "写入请求头或长度无效"); return;
    }
    if ((headers.count("sec-fetch-site") && headers["sec-fetch-site"] != "same-origin")
        || (headers.count("origin") && headers["origin"] != "http://" + headers["host"])) {
        reply(403, "不允许跨来源写入"); return;
    }
    const std::size_t body_start = end + 4;
    while (raw.size() < body_start + length) {
        char buf[256];
        const auto n = ::recv(fd, buf, std::min(sizeof(buf), body_start + length - raw.size()), 0);
        if (n <= 0) { reply(400, "写入请求不完整"); return; }
        raw.append(buf, static_cast<std::size_t>(n));
    }
    if (raw.size() != body_start + length) { reply(400, "写入长度不匹配"); return; }
    std::string success = "{\"ok\":true}";
    const auto body = raw.substr(body_start, length);
    const auto error = path == "/api/output"
        ? writeOutput(st, body, headers["x-rocos-session"], headers["x-rocos-instance"])
        : controlRequest(st, path, body, headers["x-rocos-session"], headers["x-rocos-instance"], success);
    reply(error.empty() ? 200 : 400, error, success);
}

void handleConnection(int fd, AppState &st, const std::string &argv0) {
    const timeval timeout{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    std::string raw;
    raw.reserve(4096);
    char buf[4096];
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) { close(fd); return; }
        raw.append(buf, n);
        if (raw.size() > 8192) { close(fd); return; }
    }

    std::string method, target;
    if (!parseRequest(raw, method, target)) {
        const std::string r = response(405, "Method Not Allowed", "text/plain; charset=utf-8", "method not allowed\n");
        (void)sendAll(fd, r);
        close(fd);
        return;
    }

    const auto path = target.substr(0, target.find('?'));

    if (method == "POST") {
        if (path == "/api/output" || path == "/api/mode" || path == "/api/state" || path == "/api/command") {
            handleWriteRequest(fd, st, std::move(raw), path); return;
        }
        const auto r = response(405, "Method Not Allowed", "text/plain", "method not allowed");
        (void)sendAll(fd, r);
        close(fd);
        return;
    }

    if (path == "/" || path == "/index.html") {
        std::string html;
        if (loadFrontendAsset("index.html", html, argv0)) {
            const std::string r = response(200, "OK", "text/html; charset=utf-8", html);
            (void)sendAll(fd, r);
        } else {
            const std::string r = response(200, "OK", "text/html; charset=utf-8",
                "<h1>index.html not found</h1><p>Place index.html next to the executable or in the current directory.</p>");
            (void)sendAll(fd, r);
        }
        close(fd);
    } else if (path == "/value_decoder.mjs" || path == "/topology.mjs"
               || path == "/diagnostics.mjs" || path == "/controls.mjs"
               || path == "/vendor/cytoscape-3.33.1.min.js") {
        std::string script;
        if (loadFrontendAsset(path.substr(1), script, argv0)) {
            const std::string r = response(200, "OK", "text/javascript; charset=utf-8", script);
            (void)sendAll(fd, r);
        } else {
            const std::string r = response(404, "Not Found", "text/plain; charset=utf-8", "not found\n");
            (void)sendAll(fd, r);
        }
        close(fd);
    } else if (path == "/AlibabaPuHuiTi-3-55-Regular.woff2"
               || path == "/AlibabaPuHuiTi-3-65-Medium.woff2"
               || path == "/AlibabaPuHuiTi-3-75-SemiBold.woff2"
               || path == "/AlibabaPuHuiTi-3-85-Bold.woff2") {
        std::string font;
        if (loadFrontendAsset(path.substr(1), font, argv0)) {
            const std::string r = response(200, "OK", "font/woff2", font);
            (void)sendAll(fd, r);
        } else {
            const std::string r = response(404, "Not Found", "text/plain; charset=utf-8", "not found\n");
            (void)sendAll(fd, r);
        }
        close(fd);
    } else if (path == "/AlibabaPuHuiTi-3.0-LICENSE.txt" || path == "/diagnostics.mjs" || path == "/controls.mjs"
               || path == "/vendor/cytoscape-LICENSE.txt") {
        std::string notice;
        const bool found = loadFrontendAsset(path.substr(1), notice, argv0);
        const std::string r = response(found ? 200 : 404, found ? "OK" : "Not Found",
                                       "text/plain; charset=utf-8", found ? notice : "not found\n");
        (void)sendAll(fd, r);
        close(fd);
    } else if (path == "/api/snapshot") {
        const std::string r = response(200, "OK", "application/json; charset=utf-8", renderSnapshot(st));
        (void)sendAll(fd, r);
        close(fd);
    } else if (path == "/api/events") {
        // SSE 长连接：先发响应头（keep-alive、无 Content-Length），再周期推送。
        const std::string head = response(200, "OK", "text/event-stream; charset=utf-8", "", true);
        (void)sendAll(fd, head);
        // 立即推一帧，建立首屏；隔一段时间判断连接是否仍有效（客户端断开时 send 返回 -1）。
        bool alive = true;
        while (alive) {
            const std::string snap = renderSnapshot(st);
            const std::string ev = "data: " + snap + "\n\n";
            if (!sendAll(fd, ev)) { alive = false; break; }  // EPIPE/ECONNRESET：客户端已断开
            std::this_thread::sleep_for(std::chrono::milliseconds(kPushIntervalMs));
        }
        close(fd);
    } else {
        const std::string r = response(404, "Not Found", "text/plain; charset=utf-8", "not found\n");
        (void)sendAll(fd, r);
        close(fd);
    }
}

}  // namespace

int main(int argc, char **argv) {
    AppState st;

    int port = kDefaultPort;
    std::string host = "127.0.0.1";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--master-id") == 0 && i + 1 < argc) {
            st.master_id = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (std::strcmp(argv[i], "--demo") == 0) {
            st.demo = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cout << "shm_web_server [--master-id N] [--port P] [--host H] [--demo]\n";
            return 0;
        }
    }

    if (st.demo) {
        std::string why;
        if (!createDemoMemory(st, why)) {
            std::cerr << "demo setup failed: " << why << "\n";
            return 1;
        }
        st.connected = true;
    }
    ignoreSigpipe();
    installSignalHandlers();

    const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { std::perror("socket"); return 1; }
    const int on = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    if (bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        std::perror("bind");
        std::cerr << "failed to bind " << host << ":" << port << "\n";
        return 1;
    }
    if (listen(listen_fd, 16) != 0) { std::perror("listen"); return 1; }

    std::atomic<bool> stop{false};
    std::thread maintenance([&]() {
        auto nextAnimation = std::chrono::steady_clock::now();
        auto nextSnapshot = nextAnimation;
        while (!stop.load()) {
            {
                std::lock_guard<std::mutex> lk(st.mtx);
                if (st.demo && std::chrono::steady_clock::now() >= nextAnimation) {
                    animateDemo(st);
                    nextAnimation = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPushIntervalMs);
                }
                processDemoCommand(st);
                pollOwnedCommand(st);
            }
            if (std::chrono::steady_clock::now() >= nextSnapshot) {
                (void)renderSnapshot(st); // Heartbeat/reconnect must not depend on a browser being open.
                nextSnapshot = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPushIntervalMs);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    std::mutex clientsMutex;
    std::condition_variable clientsDone;
    std::set<int> clients;

    std::cout << "[shm_web] listening on http://" << host << ":" << port
              << "  (master_id=" << st.master_id << (st.demo ? ", demo mode" : "") << ")\n";
    std::cout << "[shm_web] open this URL in a browser\n";

    for (;;) {
        const int client = accept(listen_fd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR || g_shutdown) break;
            continue;
        }
        const int controlFd = dup(client);
        if (controlFd < 0) { close(client); continue; }
        {
            std::lock_guard<std::mutex> lk(clientsMutex);
            clients.insert(controlFd);
        }
        std::thread([&, client, controlFd, executable = std::string(argv[0])]() {
            handleConnection(client, st, executable);
            std::lock_guard<std::mutex> lk(clientsMutex);
            clients.erase(controlFd);
            close(controlFd);
            clientsDone.notify_all();
        }).detach();
    }
    close(listen_fd);

    stop.store(true);
    maintenance.join();
    {
        std::unique_lock<std::mutex> lk(clientsMutex);
        for (int client : clients) shutdown(client, SHUT_RDWR);
        clientsDone.wait(lk, [&]() { return clients.empty(); });
    }
    // Only the demo owner unlinks its own IPC objects after all users have stopped.
    st.writer.reset();
    st.cfg.reset();
    return 0;
}
