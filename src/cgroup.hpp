#pragma once

#include <string>

#include "common.hpp"
#include "util/path.hpp"

struct TDevice;
class TCgroup;

class TSubsystem {
public:
    const std::string Type;
    const TSubsystem *Hierarchy;
    TPath Root;

    TSubsystem(const std::string &type) : Type(type) { }
    virtual void InitializeSubsystem() { }

    TCgroup RootCgroup() const;
    TCgroup Cgroup(const std::string &name) const;

    TError TaskCgroup(pid_t pid, TCgroup &cgroup) const;
};

class TCgroup {
public:
    const TSubsystem *Subsystem;
    std::string Name;

    TCgroup() { }
    TCgroup(const TSubsystem *subsystem, const std::string &name) :
        Subsystem(subsystem), Name(name) { }

    bool Secondary() const {
        return !Subsystem || Subsystem->Hierarchy != Subsystem;
    }

    std::string Type() const {
        return Subsystem ? Subsystem->Type : "(null)";
    }

    friend std::ostream& operator<<(std::ostream& os, const TCgroup& cgroup) {
        return os << cgroup.Type() << ":" << cgroup.Name;
    }

    friend bool operator==(const TCgroup& lhs, const TCgroup& rhs) {
        return lhs.Name == rhs.Name;
    }

    friend bool operator!=(const TCgroup& lhs, const TCgroup& rhs) {
        return lhs.Name != rhs.Name;
    }

    TCgroup Child(const std::string& name) const;
    TError Childs(std::vector<TCgroup> &cgroups) const;
    TError ChildsAll(std::vector<TCgroup> &cgroups) const;

    TPath Path() const;
    bool IsRoot() const;
    bool Exists() const;

    TError Create() const;
    TError Remove() const;

    TError KillAll(int signal) const;

    TError GetProcesses(std::vector<pid_t> &pids) const {
        return GetPids("cgroup.procs", pids);
    }

    TError GetTasks(std::vector<pid_t> &pids) const {
        return GetPids("tasks", pids);
    }

    bool IsEmpty() const;

    TError Attach(pid_t pid) const;

    TPath Knob(const std::string &knob) const;
    bool Has(const std::string &knob) const;
    TError Get(const std::string &knob, std::string &value) const;
    TError Set(const std::string &knob, const std::string &value) const;

    TError GetPids(const std::string &knob, std::vector<pid_t> &pids) const;

    TError GetUint64(const std::string &knob, uint64_t &value) const;
    TError SetUint64(const std::string &knob, uint64_t value) const;

    TError GetBool(const std::string &knob, bool &value) const;
    TError SetBool(const std::string &knob, bool value) const;

    TError GetUintMap(const std::string &knob, TUintMap &value) const;
};

class TMemorySubsystem : public TSubsystem {
public:
    const std::string STAT = "memory.stat";
    const std::string OOM_CONTROL = "memory.oom_control";
    const std::string EVENT_CONTROL = "cgroup.event_control";
    const std::string USE_HIERARCHY = "memory.use_hierarchy";
    const std::string RECHARGE_ON_PAGE_FAULT = "memory.recharge_on_pgfault";
    const std::string USAGE = "memory.usage_in_bytes";
    const std::string LIMIT = "memory.limit_in_bytes";
    const std::string SOFT_LIMIT = "memory.soft_limit_in_bytes";
    const std::string LOW_LIMIT = "memory.low_limit_in_bytes";
    const std::string MEM_SWAP_LIMIT = "memory.memsw.limit_in_bytes";
    const std::string DIRTY_LIMIT = "memory.dirty_limit_in_bytes";
    const std::string DIRTY_RATIO = "memory.dirty_ratio";
    const std::string FS_BPS_LIMIT = "memory.fs_bps_limit";
    const std::string FS_IOPS_LIMIT = "memory.fs_iops_limit";
    const std::string ANON_USAGE = "memory.anon.usage";
    const std::string ANON_LIMIT = "memory.anon.limit";
    const std::string FAIL_CNT = "memory.failcnt";

    TMemorySubsystem() : TSubsystem("memory") {}

    TError Statistics(TCgroup &cg, TUintMap &stat) const {
        return cg.GetUintMap(STAT, stat);
    }

    TError Usage(TCgroup &cg, uint64_t &value) const {
        return cg.GetUint64(USAGE, value);
    }

    TError GetSoftLimit(TCgroup &cg, uint64_t &limit) const {
        return cg.GetUint64(SOFT_LIMIT, limit);
    }

    TError SetSoftLimit(TCgroup &cg, uint64_t limit) const {
        return cg.SetUint64(SOFT_LIMIT, limit);
    }

    bool SupportGuarantee() const {
        return RootCgroup().Has(LOW_LIMIT);
    }

    TError SetGuarantee(TCgroup &cg, uint64_t guarantee) const {
        if (!SupportGuarantee())
            return TError::Success();
        return cg.SetUint64(LOW_LIMIT, guarantee);
    }

    bool SupportIoLimit() const {
        return RootCgroup().Has(FS_BPS_LIMIT);
    }

    bool SupportDirtyLimit() const {
        return RootCgroup().Has(DIRTY_LIMIT);
    }

    bool SupportSwap() const {
        return RootCgroup().Has(MEM_SWAP_LIMIT);
    }

    bool SupportRechargeOnPgfault() const {
        return RootCgroup().Has(RECHARGE_ON_PAGE_FAULT);
    }

    TError RechargeOnPgfault(TCgroup &cg, bool enable) const {
        if (!SupportRechargeOnPgfault())
            return TError::Success();
        return cg.SetBool(RECHARGE_ON_PAGE_FAULT, enable);
    }

    TError GetAnonUsage(TCgroup &cg, uint64_t &usage) const;
    bool SupportAnonLimit() const;
    TError SetAnonLimit(TCgroup &cg, uint64_t limit) const;

    TError SetLimit(TCgroup &cg, uint64_t limit);
    TError SetIoLimit(TCgroup &cg, uint64_t limit);
    TError SetIopsLimit(TCgroup &cg, uint64_t limit);
    TError SetDirtyLimit(TCgroup &cg, uint64_t limit);
    TError SetupOOMEvent(TCgroup &cg, TFile &event);

    TError GetFailCnt(TCgroup &cg, uint64_t &cnt) {
        return cg.GetUint64(FAIL_CNT, cnt);
    }
};

class TFreezerSubsystem : public TSubsystem {
public:
    TFreezerSubsystem() : TSubsystem("freezer") {}

    TError WaitState(TCgroup &cg, const std::string &state) const;
    TError Freeze(TCgroup &cg) const;
    TError Thaw(TCgroup &cg, bool wait = true) const;
    bool IsFrozen(TCgroup &cg) const;
    bool IsSelfFreezing(TCgroup &cg) const;
    bool IsParentFreezing(TCgroup &cg) const;
};

class TCpuSubsystem : public TSubsystem {
public:
    bool HasShares, HasQuota, HasSmart, HasReserve;
    uint64_t BasePeriod, BaseShares;
    TCpuSubsystem() : TSubsystem("cpu") { }
    void InitializeSubsystem() override;
    TError SetCpuPolicy(TCgroup &cg, const std::string &policy,
                        double guarantee, double limit);
};

class TCpuacctSubsystem : public TSubsystem {
public:
    TCpuacctSubsystem() : TSubsystem("cpuacct") {}
    TError Usage(TCgroup &cg, uint64_t &value) const;
    TError SystemUsage(TCgroup &cg, uint64_t &value) const;
};

class TNetclsSubsystem : public TSubsystem {
public:
    TNetclsSubsystem() : TSubsystem("net_cls") {}
};

struct BlkioStat {
    std::string Device;
    uint64_t Read;
    uint64_t Write;
    uint64_t Sync;
    uint64_t Async;
};

class TBlkioSubsystem : public TSubsystem {
    TError GetStatLine(const std::vector<std::string> &lines,
                       const size_t i,
                       const std::string &name,
                       uint64_t &val) const;
    TError GetDevice(const std::string &majmin,
                     std::string &device) const;
public:
    TBlkioSubsystem() : TSubsystem("blkio") {}
    TError Statistics(TCgroup &cg,
                      const std::string &file,
                      std::vector<BlkioStat> &stat) const;
    TError SetIoPolicy(TCgroup &cg, const std::string &policy) const;
    bool SupportIoPolicy() const;
};

class TDevicesSubsystem : public TSubsystem {
public:
    TDevicesSubsystem() : TSubsystem("devices") {}
    TError ApplyDefault(TCgroup &cg);
    TError ApplyDevice(TCgroup &cg, const TDevice &device);
};

extern TMemorySubsystem     MemorySubsystem;
extern TFreezerSubsystem    FreezerSubsystem;
extern TCpuSubsystem        CpuSubsystem;
extern TCpuacctSubsystem    CpuacctSubsystem;
extern TNetclsSubsystem     NetclsSubsystem;
extern TBlkioSubsystem      BlkioSubsystem;
extern TDevicesSubsystem    DevicesSubsystem;

extern std::vector<TSubsystem *> AllSubsystems;
extern std::vector<TSubsystem *> Subsystems;
extern std::vector<TSubsystem *> Hierarchies;

TError InitializeCgroups();
TError InitializeDaemonCgroups();
