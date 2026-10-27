// Copyright 2026, Yang Luo
// SPDX-License-Identifier: GPL-3.0-or-later
#include <rocos_shm/shared_memory_config.hpp>
#include <sys/wait.h>
#include <iostream>
#include <atomic>
#include <cstdlib>
using namespace rocos;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while(0)
static void waitChild(pid_t pid) { int status=0; CHECK(waitpid(pid,&status,0)==pid); CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0); }
int main() {
    // Unique per process IDs. No pre-test unlink: owner lock is authoritative.
    const int id=100000+getpid();
    SharedMemoryConfig master(id);
    CHECK(master.createSharedMemory());
    SharedMemoryConfig duplicate(id); CHECK(!duplicate.createSharedMemory());
    CHECK(master.createPdDataMemoryProvider(256,256));
    SharedMemoryConfig early(id); CHECK(!early.connect());
    auto& bus=*master.ecatBus;
    bus.slave_num=50;
    bus.supported_cmds=(1u<<CMD_SDO_READ)|(1u<<CMD_SDO_WRITE)|(1u<<CMD_RESET_STATS);
    bus.state_control_supported=1;
    for (int i=0;i<50;++i) {
        bus.slaves[i].id=i;
        bus.slaves[i].input_var_num=25; bus.slaves[i].output_var_num=25;
        for (int j=0;j<25;++j) {
            CHECK(setPdoBits(bus.slaves[i].input_vars[j],j*8,8,256));
            CHECK(setPdoBits(bus.slaves[i].output_vars[j],j*8,8,256));
        }
    }
    CHECK(!setPdoBits(bus.slaves[0].input_vars[0],1,8,256));
    CHECK(!setPdoBits(bus.slaves[0].input_vars[0],0,7,256));
    CHECK(!setPdoBits(bus.slaves[0].input_vars[0],2048,8,256));
    bus.slave_num=51; CHECK(!master.publishReady()); bus.slave_num=50;
    bus.slaves[0].input_var_num=26; CHECK(!master.publishReady()); bus.slaves[0].input_var_num=25;
    CHECK(master.publishReady());
    // Before clients exist, corrupt each fixed header field and confirm rejection.
    uint32_t* fields[]={&master.header->abi_version,&master.header->header_size,&master.header->bus_size,&master.header->input_bytes,&master.header->output_bytes};
    for (auto* field:fields) { const auto old=*field; ++*field; SharedMemoryConfig c(id); CHECK(!c.connect()); *field=old; }
    master.header->magic[0]='X'; {SharedMemoryConfig c(id); CHECK(!c.connect());} master.header->magic[0]='R';
    { SharedMemoryConfig monitor(id); CHECK(monitor.connect(true)); CHECK(!monitor.waitForSignal());
      bool rejected=false; try {monitor.setSlaveOutputVarValue<uint8_t>(0,0,2);} catch(const std::out_of_range&) {rejected=true;} CHECK(rejected); }
    SharedMemoryConfig client(id); CHECK(client.connect());
    client.setSlaveOutputVarValue<uint8_t>(49,24,42); CHECK(master.getSlaveOutputVarValue<uint8_t>(49,24)==42);
    bool rejected=false; try {client.getSlaveInputVarValue<uint32_t>(0,0);} catch(const std::out_of_range&) {rejected=true;} CHECK(rejected);
    rejected=false; try {client.getSlaveInputVarValue<uint8_t>(50,0);} catch(const std::out_of_range&) {rejected=true;} CHECK(rejected);
    CHECK(master.notifyClients()); CHECK(client.waitForSignal(0));
    // Raw malformed request tests, no live client transaction.
    auto raw=[&](AsyncCommandBox req,int expected) {
        auto& box=bus.cmd_box; req.instance_id=master.header->instance_id; req.cmd_id=100;
        req.deadline_mono_ns=monotonicNs()+500000000; copyCommandPayload(box,req); atomicStore(box.status,BOX_REQUESTED);
        CHECK(master.processCommand([](const AsyncCommandBox&,AsyncCommandBox&)->int32_t { return RESULT_OK; }));
        CHECK(atomicLoad(box.status)==BOX_FAILED); CHECK(box.result_code==expected); CHECK(box.data_size==0);
        atomicStore(box.status,BOX_IDLE);
    };
    AsyncCommandBox req; req.cmd_type=CMD_SDO_READ; req.target_slave=0; req.data_size=65; raw(req,INVALID_ARGUMENT);
    req.data_size=64; req.cmd_type=CMD_RESET_FAULT; raw(req,UNSUPPORTED);
    { auto& box=bus.cmd_box; box.instance_id=master.header->instance_id+1; box.cmd_id=1; atomicStore(box.status,BOX_REQUESTED);
      CHECK(master.processCommand([](const AsyncCommandBox&,AsyncCommandBox&)->int32_t {return RESULT_OK;}));
      CHECK(box.result_code==STALE_INSTANCE); atomicStore(box.status,BOX_IDLE); }
    { auto& box=bus.cmd_box; box.instance_id=master.header->instance_id; box.cmd_id=1; box.cmd_type=CMD_SDO_READ; box.data_size=1; box.target_slave=0; box.deadline_mono_ns=1; atomicStore(box.status,BOX_REQUESTED);
      CHECK(master.processCommand([](const AsyncCommandBox&,AsyncCommandBox&)->int32_t {return RESULT_OK;}));
      CHECK(box.result_code==DEADLINE_EXCEEDED); atomicStore(box.status,BOX_IDLE); }
    // Real independent client process: full payload, multiple threads, local timeout and late drain.
    pid_t child=fork(); CHECK(child>=0);
    if (!child) {
        SharedMemoryConfig c(id); CHECK(c.connect());
        AsyncCommandBox request,response; request.cmd_type=CMD_SDO_READ; request.target_slave=0; request.data_size=64;
        CHECK(c.executeCommand(request,response)==RESULT_OK); CHECK(response.data_size==64 && response.data[63]==63);
        std::atomic<int> completed{0};
        auto task=[&] { AsyncCommandBox out; if(c.executeCommand(request,out)==RESULT_OK) ++completed; };
        std::thread a(task),b(task); a.join(); b.join(); CHECK(completed==2);
        request.cmd_type=CMD_SDO_WRITE; request.data_size=1;
        CHECK(c.executeCommand(request,response,5)==OUTCOME_UNKNOWN);
        CHECK(!terminalStatus(response.status) && response.cmd_id!=0 && response.data_size==0);
        CHECK(c.executeCommand(request,response)==BUSY);
        int32_t result; do { usleep(1000); result=c.pollCommand(response); } while(result==BUSY);
        CHECK(result==OUTCOME_UNKNOWN && response.status==BOX_TIMEOUT_UNKNOWN && response.data_size==0);
        _exit(0); // Never destroy inherited master ownership after fork.
    }
    int handled=0;
    const uint64_t limit=monotonicNs()+5000000000ULL;
    while(handled<4 && monotonicNs()<limit) {
        if(master.processCommand([](const AsyncCommandBox& r,AsyncCommandBox& out)->int32_t {
            if(r.cmd_type==CMD_SDO_WRITE) {usleep(50000);return OUTCOME_UNKNOWN;}
            for(uint32_t i=0;i<r.data_size;++i) out.data[i]=static_cast<uint8_t>(i);
            out.data_size=r.data_size; return RESULT_OK;
        })) ++handled;
        usleep(1000);
    }
    CHECK(handled==4); waitChild(child); CHECK(atomicLoad(bus.cmd_box.status)==BOX_IDLE);
    // Fresh process restart produces a new instance, stale commands are rejected (above).
    const uint64_t first=master.header->instance_id;
    client.disconnect(); master.disconnect();
    CHECK(master.createSharedMemory()); CHECK(master.header->instance_id!=first);
    CHECK(master.createPdDataMemoryProvider(0,0)); CHECK(master.publishReady());
    CHECK(client.connect()); CHECK(client.pdInputPtr==nullptr);
    // A crashed creator leaves stale names, but no flock holder. Reclaim only after peers stopped.
    const int crashId=id+1000000;
    pid_t owner=fork(); CHECK(owner>=0);
    if (!owner) {
        SharedMemoryConfig crashed(crashId); CHECK(crashed.createSharedMemory());
        CHECK(crashed.createPdDataMemoryProvider(8,8)); CHECK(crashed.publishReady());
        _exit(0);
    }
    waitChild(owner);
    { SharedMemoryConfig reclaimed(crashId); CHECK(reclaimed.createSharedMemory());
      CHECK(reclaimed.createPdDataMemoryProvider(8,8)); CHECK(reclaimed.publishReady()); }
    std::cout << "ABI v2 layout, rejection, ownership, PDO, and cross-process mailbox tests passed\n";
}
