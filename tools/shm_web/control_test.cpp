// Independent owner/client process mappings. No EtherCAT, drivers or production IDs.
#define main shm_web_server_main
#include "shm_web_server.cpp"
#undef main
#include <sys/wait.h>
#include <stdexcept>
#define check(ok) do { if (!(ok)) throw std::runtime_error("control check failed at line " + std::to_string(__LINE__)); } while (false)

struct Peer {
    pid_t pid = -1;
    int stop = -1;
    ~Peer() { if (stop >= 0) { close(stop); int status; waitpid(pid, &status, 0); } }
};

std::string httpControl(AppState &st, const std::string &path, const std::string &body,
                        const std::string &session, const std::string &instance) {
    int fds[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    std::thread worker(handleConnection, fds[1], std::ref(st), std::string("test"));
    const auto request = "POST " + path + " HTTP/1.1\r\nHost: localhost\r\nOrigin: http://localhost\r\nX-Rocos-Write: 1\r\nX-Rocos-Session: "
        + session + "\r\nX-Rocos-Instance: " + instance + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    check(sendAll(fds[0], request));
    shutdown(fds[0], SHUT_WR);
    std::string response;
    char buffer[1024];
    ssize_t n;
    while ((n = recv(fds[0], buffer, sizeof(buffer), 0)) > 0) response.append(buffer, n);
    close(fds[0]); worker.join();
    return response;
}

int main() {
    const int id = 200000 + getpid();
    int ready[2], stop[2];
    check(pipe(ready) == 0 && pipe(stop) == 0);
    Peer peer;
    peer.pid = fork();
    check(peer.pid >= 0);
    if (peer.pid == 0) {
        close(ready[0]); close(stop[1]);
        int result = 0;
        {
            SharedMemoryConfig owner(id);
            if (!owner.createSharedMemory() || !owner.createPdDataMemoryProvider(18, 13)) result = 1;
            if (!result) {
                populateDemoBus(*owner.ecatBus);
                owner.ecatBus->slaves[0].id = 17; // SDO uses array index, OUT uses display id + descriptor.
                if (!owner.publishReady()) result = 1;
            }
            if (!result) rocos::atomicStore(owner.ecatBus->heartbeat_mono_ns, rocos::monotonicNs());
            const char status = result ? 'F' : 'R';
            if (::write(ready[1], &status, 1) != 1) result = 1;
            close(ready[1]);
            fcntl(stop[0], F_SETFL, O_NONBLOCK);
            while (!result) {
                char c;
                if (::read(stop[0], &c, 1) == 0) break;
                rocos::atomicStore(owner.ecatBus->heartbeat_mono_ns, rocos::monotonicNs());
                owner.processCommand([](const rocos::AsyncCommandBox &req, rocos::AsyncCommandBox &out) -> int32_t {
                    if (req.cmd_type == rocos::CMD_RESET_STATS) return rocos::RESULT_OK;
                    if (req.sdo_index == 0x1001) { out.error_valid = 3; out.backend_error = 123; out.sdo_abort_code = 0x06020000; return rocos::BACKEND_ERROR; }
                    if (req.sdo_index == 0x1002) return rocos::BUSY; // BUSY can itself be a terminal backend result.
                    if (req.sdo_index == 0x1003) return rocos::OUTCOME_UNKNOWN;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    out.data[0] = 0x78; out.data[1] = 0x56; out.data[2] = 0x34; out.data[3] = 0x12;
                    out.data_size = 4;
                    return rocos::RESULT_OK;
                });
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } // Owner alone unlinks its objects.
        close(stop[0]);
        _exit(result);
    }
    close(ready[1]); close(stop[0]); peer.stop = stop[1];
    char status;
    check(::read(ready[0], &status, 1) == 1 && status == 'R'); close(ready[0]);
    AppState st;
    st.master_id = id;
    check(renderSnapshot(st).find("\"read_only\":true") != std::string::npos);
    check(st.connected && !st.writer);
    const auto instance = std::to_string(st.cfg->header->instance_id);
    std::string reply;
    auto call = [&](const std::string &path, const std::string &body) {
        return controlRequest(st, path, body, st.write_session, instance, reply);
    };
    check(!call("/api/state", "8").empty());
    check(call("/api/mode", "enable sole-writer").empty());
    const auto session = st.write_session;
    check(!session.empty());
    check(!controlRequest(st, "/api/state", "8", "wrong-session", instance, reply).empty());
    check(!controlRequest(st, "/api/state", "8", session, "0", reply).empty());
    check(!call("/api/state", "9").empty());
    check(httpControl(st, "/api/state", "4", "wrong", instance).find("400 Bad Request") != std::string::npos);
    check(httpControl(st, "/api/state", "4", session, instance).find("200 OK") != std::string::npos);
    check(rocos::atomicLoad(st.cfg->ecatBus->request_state) == 4);
    rocos::atomicStore(st.writer->ecatBus->state_control_supported, 0u);
    check(!call("/api/state", "8").empty());
    rocos::atomicStore(st.writer->ecatBus->state_control_supported, 1u);
    const auto output = std::to_string(id) + " 17 24698 0 0 4 01000000";
    check(writeOutput(st, output, session, instance).empty());
    check(hexBytes(st.cfg->pdOutputPtr, 0, 4) == "01000000");
    for (const auto body : {"1 17 4096 0 4 -", "1 0 4096 0 65 -", "2 0 4096 0 4 ffff", "4 0 0 0 0 -", "1 0 65536 0 4 -"})
        check(!call("/api/command", body).empty());
    auto drain = [&]() {
        for (int i = 0; i < 500 && st.command_pending; ++i) {
            pollOwnedCommand(st);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(!st.command_pending);
    };
    check(httpControl(st, "/api/command", "1 0 4096 0 4 -", st.write_session, instance).find("200 OK") != std::string::npos);
    check(st.command_pending && st.command_result.result_code == rocos::OUTCOME_UNKNOWN);
    check(!call("/api/command", "1 0 4096 0 4 -").empty());
    check(!writeOutput(st, output, session, instance).empty());
    check(httpControl(st, "/api/mode", "disable", session, "wrong").find("400 Bad Request") != std::string::npos);
    check(httpControl(st, "/api/mode", "disable", session, instance).find("200 OK") != std::string::npos);
    check(!st.write_enabled && st.command_pending);
    check(!call("/api/mode", "enable sole-writer").empty());
    drain();
    check(st.command_result.status == rocos::BOX_SUCCESS && st.command_result.data[0] == 0x78);
    const auto firstId = st.command_result.cmd_id;
    check(rocos::atomicLoad(st.cfg->ecatBus->cmd_box.status) == rocos::BOX_IDLE);
    check(call("/api/mode", "enable sole-writer").empty());
    for (const auto index : {4097, 4098, 4099}) {
        check(call("/api/command", "1 0 " + std::to_string(index) + " 0 4 -").empty());
        drain();
        check(st.command_result.cmd_id > firstId);
        if (index == 4097) check(st.command_result.status == rocos::BOX_FAILED && st.command_result.error_valid == 3 && st.command_result.sdo_abort_code == 0x06020000);
        if (index == 4098) check(st.command_result.status == rocos::BOX_FAILED && st.command_result.result_code == rocos::BUSY);
        if (index == 4099) check(st.command_result.status == rocos::BOX_TIMEOUT_UNKNOWN);
    }
    // Another server may observe, but cannot obtain this server's lifetime writer lock.
    AppState other; other.master_id = id;
    (void)renderSnapshot(other);
    check(!controlRequest(other, "/api/mode", "enable sole-writer", "", instance, reply).empty());
    // Refuse incompatible ABI on a new independent mapping.
    rocos::atomicStore(st.writer->header->abi_version, 99u);
    SharedMemoryConfig incompatible(id);
    check(!incompatible.connect(true));
    rocos::atomicStore(st.writer->header->abi_version, rocos::SHM_ABI_VERSION);
    // Lost heartbeat revokes writes without resetting a pending transaction.
    check(call("/api/command", "1 0 4096 0 4 -").empty());
    check(st.command_pending);
    revokeWrites(st);
    check(st.recovery_required && !st.write_enabled);
    drain();
    check(!call("/api/mode", "enable sole-writer").empty());
    std::cout << "Web ownership, state, SDO and independent mapping checks passed\n";
}
