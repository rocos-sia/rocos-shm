// Copyright 2026, Yang Luo
// SPDX-License-Identifier: GPL-3.0-or-later
// Extracted from rocos-ecm shared_memory_config.hpp; ABI v2 implementation.
#pragma once
#include "access.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <semaphore.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
namespace rocos {
class SharedMemoryConfig {
    struct Region {
        int fd{-1}; void* data{nullptr}; size_t size{0}; bool owned{false}; std::string name;
        ~Region() { close(); }
        void close() {
            // Unlink while owner flock is still held.
            if (owned) shm_unlink(name.c_str());
            if (data) munmap(data,size);
            if (fd>=0) ::close(fd);
            data=nullptr; size=0; fd=-1; owned=false;
        }
        bool create(const std::string& n,size_t bytes) {
            if (fd>=0 || bytes>EC_SHM_MAX_SIZE) return false;
            name=n; fd=shm_open(n.c_str(),O_RDWR|O_CREAT,0660);
            if (fd<0) return false;
            if (flock(fd,LOCK_EX|LOCK_NB)!=0) { close(); return false; }
            owned=true;
            if (ftruncate(fd,static_cast<off_t>(bytes))!=0) { close(); return false; }
            return map(bytes,false);
        }
        bool open(const std::string& n,bool readOnly,size_t minimum,size_t exact=0) {
            if (fd>=0) return false;
            name=n; fd=shm_open(n.c_str(),readOnly ? O_RDONLY : O_RDWR,0);
            if (fd<0) return false;
            struct stat st{};
            if (fstat(fd,&st)!=0 || st.st_size<0 || uint64_t(st.st_size)>EC_SHM_MAX_SIZE ||
                uint64_t(st.st_size)<minimum || (exact && uint64_t(st.st_size)!=exact)) { close(); return false; }
            return map(static_cast<size_t>(st.st_size),readOnly);
        }
        bool map(size_t bytes,bool readOnly) {
            size=bytes;
            if (!bytes) return true;
            data=mmap(nullptr,bytes,readOnly ? PROT_READ : PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
            if (data==MAP_FAILED) { data=nullptr; close(); return false; }
            return true;
        }
    } busRegion_, inputRegion_, outputRegion_;
    int id_; bool readOnly_{false}; int ownedSemaphores_{0};
    std::mutex waitMutex_, commandMutex_;
    std::vector<std::thread::id> waitThreads_;
    uint64_t nextCommand_{0}, pendingCommand_{0}, lastServerCommand_{0};
    std::string error_;
    std::string name(const char* prefix) const { return std::string("/")+prefix+std::to_string(id_); }
    std::string semaphoreName(int i) const { return name(EC_SEM_MUTEX)+"_"+std::to_string(i); }
    bool fail(const char* message) { error_=message; return false; }
    void closeSemaphores() {
        for (int i=0;i<EC_SEM_NUM;++i) {
            if (i<ownedSemaphores_) sem_unlink(semaphoreName(i).c_str());
            if (sem_mutex[i] && sem_mutex[i]!=SEM_FAILED) sem_close(sem_mutex[i]);
            sem_mutex[i]=nullptr;
        }
        ownedSemaphores_=0;
    }
    bool openPdos() {
        if (!header) return fail("No validated header");
        if (!inputRegion_.open(name("pd_input_v2_"),readOnly_,header->input_bytes) ||
            inputRegion_.size!=header->input_bytes ||
            !outputRegion_.open(name("pd_output_v2_"),readOnly_,header->output_bytes) ||
            outputRegion_.size!=header->output_bytes) {
            inputRegion_.close(); outputRegion_.close(); return fail("PDO length mismatch or open failure");
        }
        pdInputPtr=inputRegion_.data; pdOutputPtr=outputRegion_.data;
        pd_input_size_=inputRegion_.size; pd_output_size_=outputRegion_.size;
        return true;
    }
    int32_t consume(AsyncCommandBox& response) {
        if (!ecatBus || !pendingCommand_) return INVALID_ARGUMENT;
        auto& box=ecatBus->cmd_box;
        const uint32_t status=atomicLoad(box.status);
        if (!terminalStatus(status)) return BUSY;
        if (box.instance_id!=header->instance_id || box.cmd_id!=pendingCommand_) return STALE_INSTANCE;
        copyCommandPayload(response,box); response.status=status;
        atomicStore(box.status,BOX_IDLE); pendingCommand_=0;
        return response.result_code;
    }
    const Slave& checkedSlave(int s) const {
        if (!ecatBus || s<0 || s>=ecatBus->slave_num || s>=MAX_SLAVE_NUM) throw std::out_of_range("Slave index");
        return ecatBus->slaves[s];
    }
    const PdVar& checkedVar(int s,int v,bool output) const {
        const Slave& slave=checkedSlave(s);
        const int count=output ? slave.output_var_num : slave.input_var_num;
        const int cap=output ? MAX_PDOUTPUT_NUM : MAX_PDINPUT_NUM;
        if (count<0 || count>cap || v<0 || v>=count) throw std::out_of_range("PDO index");
        return output ? slave.output_vars[v] : slave.input_vars[v];
    }
    void* checkedData(int s,int v,bool output,size_t bytes,bool write) const {
        const PdVar& var=checkedVar(s,v,output);
        void* base=output ? pdOutputPtr : pdInputPtr;
        const size_t length=output ? pd_output_size_ : pd_input_size_;
        if ((write && readOnly_) || !base || !validPdo(var,length) || size_t(var.size)!=bytes)
            throw std::out_of_range("PDO size, mapping or write permission");
        return static_cast<char*>(base)+var.offset;
    }
    int findVar(int s,const std::string& n,bool output) const {
        const Slave& slave=checkedSlave(s);
        const int count=output ? slave.output_var_num : slave.input_var_num;
        if (count<0 || count>(output ? MAX_PDOUTPUT_NUM : MAX_PDINPUT_NUM)) return -1;
        for (int i=0;i<count;++i) if (n==checkedVar(s,i,output).name) return i;
        return -1;
    }
public:
    ShmHeader* header{nullptr};
    EcatBus* ecatBus{nullptr};
    void* pdInputPtr{nullptr}; void* pdOutputPtr{nullptr};
    sem_t* sem_mutex[EC_SEM_NUM]{};
    size_t pd_input_size_{0}, pd_output_size_{0};
    explicit SharedMemoryConfig(int id=0):id_(id) {
        if (id<0) throw std::invalid_argument("Master id must be non-negative");
    }
    ~SharedMemoryConfig() { closeSemaphores(); }
    SharedMemoryConfig(const SharedMemoryConfig&)=delete;
    SharedMemoryConfig& operator=(const SharedMemoryConfig&)=delete;
    const std::string& lastError() const { return error_; }
    // Use connect() for recoverable errors. Legacy singleton throws; never exits the process.
    static SharedMemoryConfig* getInstance(int id=0) {
        static std::mutex mutex;
        static std::map<int,std::unique_ptr<SharedMemoryConfig>> instances;
        std::lock_guard<std::mutex> lock(mutex);
        auto it=instances.find(id);
        if (it!=instances.end()) return it->second.get();
        std::unique_ptr<SharedMemoryConfig> value(new SharedMemoryConfig(id));
        value->init(); auto* ptr=value.get(); instances.emplace(id,std::move(value)); return ptr;
    }
    bool createSharedMemory() {
        if (!atomicsLockFree() || !busRegion_.create(name(EC_SHM),sizeof(SharedMemoryV2)))
            return fail("Cannot acquire owner lock/create v2 bus");
        auto* region=new(busRegion_.data) SharedMemoryV2{};
        header=&region->header; ecatBus=&region->bus;
        std::memcpy(header->magic,"ROCOSHM2",8);
        // Read a fresh nonzero random instance token before publishing.
        int randomFd=::open("/dev/urandom",O_RDONLY|O_CLOEXEC);
        ssize_t received=-1;
        if (randomFd>=0) {
            do { received=::read(randomFd,&header->instance_id,sizeof(header->instance_id)); } while (received<0 && errno==EINTR);
            ::close(randomFd);
        }
        if (received!=8 || !header->instance_id) { disconnect(); return fail("Cannot create instance id"); }
        for (int i=0;i<EC_SEM_NUM;++i) {
            const auto n=semaphoreName(i); sem_unlink(n.c_str());
            sem_mutex[i]=sem_open(n.c_str(),O_CREAT|O_EXCL,0660,0);
            if (sem_mutex[i]==SEM_FAILED) { disconnect(); return fail("Cannot create cycle semaphore"); }
            ++ownedSemaphores_;
        }
        return true;
    }
    bool createPdDataMemoryProvider(int inputBytes,int outputBytes) {
        if (!header || !busRegion_.owned || atomicLoad(header->ready) || inputBytes<0 || outputBytes<0)
            return fail("Invalid PDO creation state or size");
        if (!inputRegion_.create(name("pd_input_v2_"),size_t(inputBytes)) ||
            !outputRegion_.create(name("pd_output_v2_"),size_t(outputBytes))) {
            inputRegion_.close(); outputRegion_.close(); return fail("Cannot acquire owner locks/create PDOs");
        }
        pdInputPtr=inputRegion_.data; pdOutputPtr=outputRegion_.data;
        pd_input_size_=inputRegion_.size; pd_output_size_=outputRegion_.size;
        if (inputBytes) std::memset(pdInputPtr,0,inputBytes);
        if (outputBytes) std::memset(pdOutputPtr,0,outputBytes);
        header->input_bytes=inputBytes; header->output_bytes=outputBytes; return true;
    }
    bool validateLayout() const {
        if (!header || !ecatBus || ecatBus->slave_num<0 || ecatBus->slave_num>MAX_SLAVE_NUM) return false;
        for (int i=0;i<ecatBus->slave_num;++i) {
            const Slave& s=ecatBus->slaves[i];
            if (!std::memchr(s.name,0,sizeof(s.name)) || !std::memchr(s.firmware_version,0,sizeof(s.firmware_version)) ||
                s.input_var_num<0 || s.input_var_num>MAX_PDINPUT_NUM || s.output_var_num<0 || s.output_var_num>MAX_PDOUTPUT_NUM) return false;
            for (int j=0;j<s.input_var_num;++j)
                if (!validPdo(s.input_vars[j],header->input_bytes) || !std::memchr(s.input_vars[j].name,0,MAX_PD_NAME_LEN)) return false;
            for (int j=0;j<s.output_var_num;++j)
                if (!validPdo(s.output_vars[j],header->output_bytes) || !std::memchr(s.output_vars[j].name,0,MAX_PD_NAME_LEN)) return false;
        }
        return std::memchr(ecatBus->ecm_version,0,sizeof(ecatBus->ecm_version));
    }
    bool publishReady() {
        if (!busRegion_.owned || inputRegion_.fd<0 || outputRegion_.fd<0 || !validateLayout()) return fail("Incomplete or invalid layout");
        atomicStore(header->ready,1u); return true;
    }
    void disconnect() {
        closeSemaphores(); inputRegion_.close(); outputRegion_.close(); busRegion_.close();
        header=nullptr; ecatBus=nullptr; pdInputPtr=nullptr; pdOutputPtr=nullptr;
        pd_input_size_=pd_output_size_=0; pendingCommand_=nextCommand_=lastServerCommand_=0;
    }
    bool connect(bool readOnly=false) {
        if (busRegion_.fd>=0) return fail("Disconnect before reconnecting");
        readOnly_=readOnly;
        if (!getSharedMemory()) return false;
        return true;
    }
    bool getSharedMemory() {
        if (!atomicsLockFree() || !busRegion_.open(name(EC_SHM),readOnly_,sizeof(ShmHeader)))
            return fail("Cannot map fixed v2 header");
        header=static_cast<ShmHeader*>(busRegion_.data);
        if (atomicLoad(header->ready)!=1 || std::memcmp(header->magic,"ROCOSHM2",8) ||
            header->abi_version!=SHM_ABI_VERSION || header->header_size!=sizeof(ShmHeader) ||
            header->bus_size!=sizeof(EcatBus) || busRegion_.size!=sizeof(SharedMemoryV2) || !header->instance_id ||
            header->input_bytes>EC_SHM_MAX_SIZE || header->output_bytes>EC_SHM_MAX_SIZE) {
            disconnect(); return fail("Not ready or incompatible ABI/header/bus size");
        }
        if (!openPdos()) { disconnect(); return false; }
        ecatBus=&static_cast<SharedMemoryV2*>(busRegion_.data)->bus;
        if (!validateLayout()) { disconnect(); return fail("Invalid slave/PDO layout"); }
        if (!readOnly_) for (int i=0;i<EC_SEM_NUM;++i) {
            sem_mutex[i]=sem_open(semaphoreName(i).c_str(),0);
            if (sem_mutex[i]==SEM_FAILED) { disconnect(); return fail("Cannot open cycle semaphore"); }
        }
        return true;
    }
    bool getPdDataMemoryProvider() { return header && inputRegion_.fd>=0 && outputRegion_.fd>=0; }
    void init() { if (!connect()) throw std::runtime_error(lastError()); }
    bool notifyClients() noexcept {
        bool ok=true;
        for (auto* s:sem_mutex) {
            int n=0;
            if (!s || s==SEM_FAILED || sem_getvalue(s,&n)!=0 || (n<1 && sem_post(s)!=0)) ok=false;
        }
        return ok;
    }
    void updateSempahore() { (void)notifyClients(); }
    bool waitForSignal(int slot=0) noexcept {
        if (readOnly_ || slot<0 || slot>=EC_SEM_NUM || !sem_mutex[slot] || sem_mutex[slot]==SEM_FAILED) return false;
        while (sem_wait(sem_mutex[slot])!=0) if (errno!=EINTR) return false;
        return true;
    }
    void wait() {
        int slot;
        { std::lock_guard<std::mutex> lock(waitMutex_);
          auto it=std::find(waitThreads_.begin(),waitThreads_.end(),std::this_thread::get_id());
          if (it==waitThreads_.end()) {
              if (waitThreads_.size()>=EC_SEM_NUM) throw std::runtime_error("No semaphore slot");
              slot=static_cast<int>(waitThreads_.size()); waitThreads_.push_back(std::this_thread::get_id());
          } else slot=static_cast<int>(it-waitThreads_.begin()); }
        if (!waitForSignal(slot)) throw std::runtime_error("Cycle wait failed");
    }
    // One SharedMemoryConfig per bus in the sole writable App. Mutex spans the entire transaction.
    int32_t executeCommand(AsyncCommandBox request,AsyncCommandBox& response,uint32_t waitMs=DEFAULT_COMMAND_TIMEOUT_MS) {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (readOnly_ || !ecatBus || !header || !waitMs) return INVALID_ARGUMENT;
        auto& box=ecatBus->cmd_box;
        if (pendingCommand_ || atomicLoad(box.status)!=BOX_IDLE) return BUSY;
        if (nextCommand_==UINT64_MAX) return INVALID_ARGUMENT;
        request.cmd_id=++nextCommand_; request.instance_id=header->instance_id;
        const uint64_t end=monotonicNs()+uint64_t(waitMs)*1000000ULL;
        if (!request.deadline_mono_ns) request.deadline_mono_ns=monotonicNs()+uint64_t(DEFAULT_COMMAND_TIMEOUT_MS)*1000000ULL;
        request.result_code=0; request.backend_error=0; request.sdo_abort_code=0; request.error_valid=0;
        pendingCommand_=request.cmd_id;
        copyCommandPayload(box,request); atomicStore(box.status,BOX_REQUESTED);
        do {
            if (terminalStatus(atomicLoad(box.status))) return consume(response);
            timespec pause{0,1000000}; nanosleep(&pause,nullptr);
        } while (monotonicNs()<end);
        // Never reset REQUESTED/PROCESSING on local timeout. pollCommand() drains the eventual response.
        const uint32_t status=atomicLoad(box.status);
        if (terminalStatus(status)) return consume(response);
        response=AsyncCommandBox{};
        response.cmd_id=pendingCommand_; response.instance_id=header->instance_id;
        response.status=status; response.result_code=OUTCOME_UNKNOWN;
        return OUTCOME_UNKNOWN;
    }
    int32_t pollCommand(AsyncCommandBox& response) {
        std::lock_guard<std::mutex> lock(commandMutex_); return consume(response);
    }
    template<class Handler> bool processCommand(Handler handler,uint32_t maxMs=DEFAULT_COMMAND_TIMEOUT_MS) {
        return processCommand(handler,maxMs,[](const AsyncCommandBox&) -> int32_t { return RESULT_OK; });
    }
    template<class Handler,class Validator> bool processCommand(Handler handler,uint32_t maxMs,Validator preflight) {
        if (!busRegion_.owned || !header || !atomicLoad(header->ready)) return false;
        auto& box=ecatBus->cmd_box;
        if (atomicLoad(box.status)!=BOX_REQUESTED) return false;
        AsyncCommandBox req; copyCommandPayload(req,box);
        AsyncCommandBox result=req;
        int32_t rc=validateCommand(req,*header,*ecatBus,monotonicNs(),maxMs);
        if (rc==RESULT_OK && req.cmd_id<=lastServerCommand_) rc=INVALID_ARGUMENT;
        if (rc==RESULT_OK) {
            try { rc=preflight(req); } catch (...) { rc=BACKEND_ERROR; }
        }
        if (rc==RESULT_OK) {
            lastServerCommand_=req.cmd_id;
            // Adapter preflight validates live identity/state before publishing PROCESSING.
            atomicStore(box.status,BOX_PROCESSING);
            try { rc=handler(req,result); } catch (...) { rc=OUTCOME_UNKNOWN; }
            if (rc==RESULT_OK && (result.data_size>MAX_CMD_DATA_SIZE || result.data_size>req.data_size)) rc=BUFFER_TOO_SMALL;
        }
        result.cmd_id=req.cmd_id; result.instance_id=req.instance_id; result.result_code=rc;
        if (rc!=RESULT_OK) { result.data_size=0; std::memset(result.data,0,sizeof(result.data)); }
        copyCommandPayload(box,result);
        atomicStore(box.status,rc==RESULT_OK ? BOX_SUCCESS : rc==OUTCOME_UNKNOWN ? BOX_TIMEOUT_UNKNOWN : BOX_FAILED);
        return true;
    }
    double getBusMinCycleTime() const { return atomicLoad(ecatBus->min_cycle_time); }
    double getBusMaxCycleTime() const { return atomicLoad(ecatBus->max_cycle_time); }
    double getBusAvgCycleTime() const { return atomicLoad(ecatBus->avg_cycle_time); }
    double getBusCurrentCycleTime() const { return atomicLoad(ecatBus->current_cycle_time); }
    bool isAuthorized() const { return atomicLoad(ecatBus->is_authorized); }
    long getTimestamp() const { return atomicLoad(ecatBus->timestamp); }
    int getBusCurrentState() const { return atomicLoad(ecatBus->current_state); }
    uint32_t getDt() const { return atomicLoad(ecatBus->dt); }
    int getSlaveNum() const { return atomicLoad(ecatBus->slave_num); }
    void setBusRequestState(int state) {
        if (readOnly_ || !ecatBus || !ecatBus->state_control_supported) throw std::runtime_error("State control unavailable");
        atomicStore(ecatBus->request_state,state);
    }
    void resetCycleTime() {
        AsyncCommandBox request, response; request.cmd_type=CMD_RESET_STATS;
        (void)executeCommand(request,response);
    }
    std::string getSlaveName(int id) const { return checkedSlave(id).name; }
    Slave getSlave(int id) const {
        const Slave& src=checkedSlave(id); Slave dst;
        // Copy only immutable prefix; runtime fields are individual atomic snapshots.
        std::memcpy(static_cast<void*>(&dst),&src,offsetof(Slave,online));
        dst.online=atomicLoad(src.online); dst.al_state=atomicLoad(src.al_state);
        dst.al_status_code=atomicLoad(src.al_status_code); dst.runtime_valid=atomicLoad(src.runtime_valid);
        return dst;
    }
    int findSlaveIdByName(const std::string& name) const {
        if (!ecatBus || ecatBus->slave_num<0 || ecatBus->slave_num>MAX_SLAVE_NUM) return -1;
        for (int i=0;i<ecatBus->slave_num;++i) if (name==ecatBus->slaves[i].name) return i;
        return -1;
    }
    Slave findSlaveByName(const std::string& name) const { int i=findSlaveIdByName(name); return i<0 ? Slave{} : getSlave(i); }
    std::string getInputVarName(int s,int v) const { return checkedVar(s,v,false).name; }
    PdVar getSlaveInputVar(int s,int v) const { return checkedVar(s,v,false); }
    int findSlaveInputVarIdByName(int s,const std::string& n) const { return findVar(s,n,false); }
    PdVar findSlaveInputVarByName(int s,const std::string& n) const { int i=findVar(s,n,false); return i<0 ? PdVar{} : checkedVar(s,i,false); }
    template<class T> T getSlaveInputVarValue(int s,int v) const {
        static_assert(std::is_trivially_copyable<T>::value,"PDO value must be trivial");
        T result; std::memcpy(&result,checkedData(s,v,false,sizeof(T),false),sizeof(T)); return result;
    }
    template<class T> void setSlaveInputVarValue(int s,int v,T value) {
        static_assert(std::is_trivially_copyable<T>::value,"PDO value must be trivial");
        std::memcpy(checkedData(s,v,false,sizeof(T),true),&value,sizeof(T));
    }
    template<class T> T getSlaveInputVarValueByName(int s,const std::string& n) const { return getSlaveInputVarValue<T>(s,findVar(s,n,false)); }
    template<class T> void setSlaveInputVarValueByName(int s,const std::string& n,T v) { setSlaveInputVarValue<T>(s,findVar(s,n,false),v); }
    // Compatibility pointer API: alignment checked; caller retains responsibility for PDO synchronization.
    template<class T> T* getSlaveInputVarPtr(int s,int v) const {
        void* data=checkedData(s,v,false,sizeof(T),true);
        if (reinterpret_cast<uintptr_t>(data)%alignof(T)) throw std::out_of_range("Unaligned PDO pointer; use value API");
        return static_cast<T*>(data);
    }
    template<class T> T* findSlaveInputVarPtrByName(int s,const std::string& n) const { int v=findVar(s,n,false); return v<0 ? nullptr : getSlaveInputVarPtr<T>(s,v); }
    std::string getOutputVarName(int s,int v) const { return checkedVar(s,v,true).name; }
    PdVar getSlaveOutputVar(int s,int v) const { return checkedVar(s,v,true); }
    int findSlaveOutputVarIdByName(int s,const std::string& n) const { return findVar(s,n,true); }
    PdVar findSlaveOutputVarByName(int s,const std::string& n) const { int i=findVar(s,n,true); return i<0 ? PdVar{} : checkedVar(s,i,true); }
    template<class T> T getSlaveOutputVarValue(int s,int v) const {
        static_assert(std::is_trivially_copyable<T>::value,"PDO value must be trivial");
        T result; std::memcpy(&result,checkedData(s,v,true,sizeof(T),false),sizeof(T)); return result;
    }
    template<class T> void setSlaveOutputVarValue(int s,int v,T value) {
        static_assert(std::is_trivially_copyable<T>::value,"PDO value must be trivial");
        std::memcpy(checkedData(s,v,true,sizeof(T),true),&value,sizeof(T));
    }
    template<class T> T getSlaveOutputVarValueByName(int s,const std::string& n) const { return getSlaveOutputVarValue<T>(s,findVar(s,n,true)); }
    template<class T> void setSlaveOutputVarValueByName(int s,const std::string& n,T v) { setSlaveOutputVarValue<T>(s,findVar(s,n,true),v); }
    // Compatibility pointer API: alignment checked; caller retains responsibility for PDO synchronization.
    template<class T> T* getSlaveOutputVarPtr(int s,int v) const {
        void* data=checkedData(s,v,true,sizeof(T),true);
        if (reinterpret_cast<uintptr_t>(data)%alignof(T)) throw std::out_of_range("Unaligned PDO pointer; use value API");
        return static_cast<T*>(data);
    }
    template<class T> T* findSlaveOutputVarPtrByName(int s,const std::string& n) const { int v=findVar(s,n,true); return v<0 ? nullptr : getSlaveOutputVarPtr<T>(s,v); }
};
using EcatConfigMaster=SharedMemoryConfig;
using EcatConfig=SharedMemoryConfig;
} // namespace rocos
