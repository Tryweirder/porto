#pragma once

#include <string>

#include "common.hpp"
#include "util/path.hpp"

struct TDevice;
class TCgroup;

#define CGROUP_FREEZER  0x0001ull
#define CGROUP_MEMORY   0x0002ull
#define CGROUP_CPU      0x0004ull
#define CGROUP_CPUACCT  0x0008ull
#define CGROUP_NETCLS   0x0010ull
#define CGROUP_BLKIO    0x0020ull
#define CGROUP_DEVICES  0x0040ull
#define CGROUP_HUGETLB  0x0080ull
#define CGROUP_CPUSET   0x0100ull
#define CGROUP_PIDS     0x0200ull

extern const TFlagsNames ControllersName;

class TSubsystem {
public:
    const uint64_t Kind;
    uint64_t Controllers;
    const std::string Type;
    const TSubsystem *Hierarchy = nullptr;
    TPath Root;

    TSubsystem(uint64_t kind, const std::string &type) : Kind(kind), Type(type) { }
    virtual void InitializeSubsystem() { }

    virtual TError InitializeCgroup(TCgroup &cgroup) {
        return TError::Success();
    }

    TCgroup RootCgroup() const;
    TCgroup Cgroup(const std::string &name) const;

    TError TaskCgroup(pid_t pid, TCgroup &cgroup) const;
    bool IsEnabled(const TCgroup &cgroup) const;
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

    TError Create();
    TError Remove() const;

    TError KillAll(int signal) const;

    TError GetProcesses(std::vector<pid_t> &pids) const {
        return GetPids("cgroup.procs", pids);
    }

    TError GetTasks(std::vector<pid_t> &pids) const {
        return GetPids("tasks", pids);
    }

    TError GetCount(bool threads, uint64_t &count) const;

    bool IsEmpty() const;

    TError Attach(pid_t pid) const;
    TError AttachAll(const TCgroup &cg) const;

    TPath Knob(const std::string &knob) const;
    bool Has(const std::string &knob) const;
    TError Get(const std::string &knob, std::string &value) const;
    TError Set(const std::string &knob, const std::string &value) const;

    TError GetPids(const std::string &knob, std::vector<pid_t> &pids) const;

    TError GetInt64(const std::string &knob, int64_t &value) const;
    TError SetInt64(const std::string &knob, int64_t value) const;

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

    TMemorySubsystem() : TSubsystem(CGROUP_MEMORY, "memory") {}

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
    uint64_t GetOomEvents(TCgroup &cg);
};

class TFreezerSubsystem : public TSubsystem {
public:
    TFreezerSubsystem() : TSubsystem(CGROUP_FREEZER, "freezer") {}

    TError WaitState(const TCgroup &cg, const std::string &state) const;
    TError Freeze(const TCgroup &cg, bool wait = true) const;
    TError Thaw(const TCgroup &cg, bool wait = true) const;
    bool IsFrozen(const TCgroup &cg) const;
    bool IsSelfFreezing(const TCgroup &cg) const;
    bool IsParentFreezing(const TCgroup &cg) const;
};

class TCpuSubsystem : public TSubsystem {
public:
    bool HasShares, HasQuota, HasSmart, HasReserve, HasRtGroup;
    uint64_t BasePeriod, BaseShares, MinShares, MaxShares;
    TCpuSubsystem() : TSubsystem(CGROUP_CPU, "cpu") { }
    void InitializeSubsystem() override;
    TError SetCpuLimit(TCgroup &cg, const std::string &policy,
                        double guarantee, double limit);
};

class TCpuacctSubsystem : public TSubsystem {
public:
    TCpuacctSubsystem() : TSubsystem(CGROUP_CPUACCT, "cpuacct") {}
    TError Usage(TCgroup &cg, uint64_t &value) const;
    TError SystemUsage(TCgroup &cg, uint64_t &value) const;
};

class TCpusetSubsystem : public TSubsystem {
public:
    TCpusetSubsystem() : TSubsystem(CGROUP_CPUSET, "cpuset") { }

    bool Supported = false;
    void InitializeSubsystem() override {
        Supported = true;
    }

    TError InitializeCgroup(TCgroup &cg) override;

    TError SetCpus(TCgroup &cg, const std::string &cpus) const;
    TError SetMems(TCgroup &cg, const std::string &mems) const;
};

class TNetclsSubsystem : public TSubsystem {
public:
    TNetclsSubsystem() : TSubsystem(CGROUP_NETCLS, "net_cls") {}
};

class TBlkioSubsystem : public TSubsystem {
public:
    bool HasWeight;
    bool HasThrottler;
    bool HasSaneBehavior;
    TBlkioSubsystem() : TSubsystem(CGROUP_BLKIO, "blkio") {}
    void InitializeSubsystem() override {
        HasWeight = RootCgroup().Has("blkio.weight");
        HasThrottler = RootCgroup().Has("blkio.throttle.read_bps_device");
        if (RootCgroup().GetBool("cgroup.sane_behavior", HasSaneBehavior))
            HasSaneBehavior = false;
    }
    TError GetIoStat(TCgroup &cg, TUintMap &map, int dir, bool iops) const;
    TError SetIoPolicy(TCgroup &cg, const std::string &policy) const;
    TError SetIoLimit(TCgroup &cg, const TUintMap &map, bool iops = false);

    TError DiskName(const std::string &disk, std::string &name) const;
    TError ResolveDisk(const std::string &key, std::string &disk) const;
};

class TDevicesSubsystem : public TSubsystem {
public:
    TDevicesSubsystem() : TSubsystem(CGROUP_DEVICES, "devices") {}
    TError ApplyDefault(TCgroup &cg);
    TError ApplyDevice(TCgroup &cg, const TDevice &device);
};

class THugetlbSubsystem : public TSubsystem {
public:
    const std::string HUGE_USAGE = "hugetlb.2MB.usage_in_bytes";
    const std::string HUGE_LIMIT = "hugetlb.2MB.limit_in_bytes";
    const std::string GIGA_USAGE = "hugetlb.1GB.usage_in_bytes";
    const std::string GIGA_LIMIT = "hugetlb.1GB.limit_in_bytes";
    THugetlbSubsystem() : TSubsystem(CGROUP_HUGETLB, "hugetlb") {}

    bool Supported = false;

    /* for now supports only 2MB pages */
    void InitializeSubsystem() override {
        Supported = RootCgroup().Has(HUGE_LIMIT);
    }

    TError GetHugeUsage(TCgroup &cg, uint64_t &usage) const {
        return cg.GetUint64(HUGE_USAGE, usage);
    }

    TError SetHugeLimit(TCgroup &cg, int64_t limit) const {
        return cg.SetInt64(HUGE_LIMIT, limit);
    }

    bool SupportGigaPages() const {
        return RootCgroup().Has(GIGA_LIMIT);
    }

    TError SetGigaLimit(TCgroup &cg, int64_t limit) const {
        return cg.SetInt64(GIGA_LIMIT, limit);
    }
};

class TPidsSubsystem : public TSubsystem {
public:
    TPidsSubsystem() : TSubsystem(CGROUP_PIDS, "pids") { }
    bool Supported = false;
    void InitializeSubsystem() override {
        Supported = true;
    }
    TError GetUsage(TCgroup &cg, uint64_t &usage) const;
    TError SetLimit(TCgroup &cg, uint64_t limit) const;
};

extern TMemorySubsystem     MemorySubsystem;
extern TFreezerSubsystem    FreezerSubsystem;
extern TCpuSubsystem        CpuSubsystem;
extern TCpuacctSubsystem    CpuacctSubsystem;
extern TCpusetSubsystem     CpusetSubsystem;
extern TNetclsSubsystem     NetclsSubsystem;
extern TBlkioSubsystem      BlkioSubsystem;
extern TDevicesSubsystem    DevicesSubsystem;
extern THugetlbSubsystem    HugetlbSubsystem;
extern TPidsSubsystem       PidsSubsystem;

extern std::vector<TSubsystem *> AllSubsystems;
extern std::vector<TSubsystem *> Subsystems;
extern std::vector<TSubsystem *> Hierarchies;

TError InitializeCgroups();
TError InitializeDaemonCgroups();
