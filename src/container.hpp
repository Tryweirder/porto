#pragma once

#include <string>
#include <vector>
#include <list>
#include <memory>
#include <atomic>

#include "util/unix.hpp"
#include "util/locks.hpp"
#include "util/log.hpp"
#include "util/idmap.hpp"
#include "stream.hpp"
#include "cgroup.hpp"
#include "property.hpp"

class TEpollSource;
class TCgroup;
class TSubsystem;
class TEvent;
enum class ENetStat;
class TNetwork;
class TNamespaceFd;
class TContainerWaiter;
class TClient;
class TVolume;
class TKeyValue;
struct TBindMount;

struct TEnv;

enum class EContainerState {
    Stopped,
    Dead,
    Starting,
    Running,
    Paused,
    Meta,
    Destroyed,
};

enum class ECpuSetType {
    Inherit,
    Reserve,
    Threads,
    Cores,
    Node,
    Absolute,
};

class TProperty;

class TContainer : public std::enable_shared_from_this<TContainer>,
                   public TNonCopyable {
    friend class TProperty;

    int Locked = 0;
    int SubtreeRead = 0;
    int SubtreeWrite = 0;
    bool PendingWrite = false;
    pid_t LastOwner = 0;

    TFile OomEvent;

    /* protected with ContainersMutex */
    std::list<std::weak_ptr<TContainerWaiter>> Waiters;

    std::shared_ptr<TEpollSource> Source;

    // data
    TError UpdateSoftLimit();
    void SetState(EContainerState next);

    TError ApplyUlimits();
    TError ApplySchedPolicy() const;
    TError ApplyDynamicProperties();
    TError PrepareWorkDir();
    TError RestoreNetwork();
    TError PrepareOomMonitor();
    void ShutdownOom();
    TError PrepareCgroups();
    TError ConfigureDevices(std::vector<TDevice> &devices);
    TError ParseNetConfig(struct TNetCfg &NetCfg);
    TError CheckIpLimit(struct TNetCfg &NetCfg);
    TError PrepareNetwork(struct TNetCfg &NetCfg);
    TError PrepareTask(struct TTaskEnv *TaskEnv,
                       struct TNetCfg *NetCfg);
    TError StartOne();

    void ScheduleRespawn();
    TError Respawn();
    TError PrepareResources();
    void FreeRuntimeResources();
    void FreeResources();

    void Reap(bool oomKilled);
    void Exit(int status, bool oomKilled);

    void CleanupWaiters();
    void NotifyWaiters();

    TError ReserveCpus(unsigned nr_threads, unsigned nr_cores,
                       TBitMap &threads, TBitMap &cores);
    TError DistributeCpus();

public:
    const std::shared_ptr<TContainer> Parent;
    const std::string Name;
    const std::string FirstName;
    const int Level; // 0 for root

    int Id = 0;

    /* protected with exclusive lock and ContainersMutex */
    EContainerState State = EContainerState::Stopped;
    int RunningChildren = 0;

    /* protected with ContainersMutex */
    std::list<std::shared_ptr<TContainer>> Children;

    bool PropSet[(int)EProperty::NR_PROPERTIES];
    bool PropDirty[(int)EProperty::NR_PROPERTIES];
    uint64_t Controllers, RequiredControllers;
    TCred OwnerCred;
    TCred TaskCred;
    std::string Command;
    std::string CoreCommand;
    std::string Cwd;
    TStdStream Stdin, Stdout, Stderr;
    std::string Root;
    bool RootRo;
    mode_t Umask;
    int VirtMode;
    bool BindDns;
    bool Isolate;
    bool NetIsolate = false;
    TMultiTuple NetProp;
    std::string Hostname;
    TTuple EnvCfg;
    std::vector<TBindMount> BindMounts;
    TMultiTuple IpList;
    TTuple IpLimit;
    TCapabilities CapAmbient;   /* get at start */
    TCapabilities CapAllowed;   /* can be set as ambient */
    TCapabilities CapLimit;     /* upper limit */
    TCapabilities CapBound;     /* actual bounding set */
    TMultiTuple DefaultGw;
    TTuple ResolvConf;
    TMultiTuple Devices;
    TStringMap Sysctl;

    time_t RealCreationTime;
    time_t RealStartTime = 0;

    uint64_t StartTime;
    uint64_t DeathTime;
    uint64_t AgingTime;

    TStringMap Ulimit;

    std::string NsName;

    uint64_t MemLimit = 0;
    uint64_t MemGuarantee = 0;
    uint64_t NewMemGuarantee = 0;
    uint64_t AnonMemLimit = 0;
    uint64_t DirtyMemLimit = 0;
    int64_t HugetlbLimit = -1;
    uint64_t ThreadLimit = 0;

    bool RechargeOnPgfault = false;

    std::string IoPolicy;
    TUintMap IoBpsLimit;
    TUintMap IoOpsLimit;

    std::string CpuPolicy;

    int SchedPolicy;
    int SchedPrio;
    int SchedNice;

    double CpuLimit;
    double CpuGuarantee;

    /* Under CpuAffinityMutex */
    ECpuSetType CpuSetType = ECpuSetType::Inherit;
    int CpuSetArg = 0;
    TBitMap CpuAffinity;
    TBitMap CpuVacant;
    TBitMap CpuReserve;

    uint32_t ContainerTC;
    uint32_t ParentTC;
    uint32_t LeafTC;

    TUintMap NetGuarantee;
    TUintMap NetLimit;
    TUintMap NetRxLimit;
    TUintMap NetPriority;

    bool ToRespawn;
    int MaxRespawns;
    uint64_t RespawnCount;

    std::string Private;
    EAccessLevel AccessLevel;
    std::atomic<int> ClientsCount;
    std::atomic<uint64_t> ContainerRequests;

    bool IsWeak = false;
    bool OomIsFatal = true;
    unsigned OomEvents = 0;
    bool OomKilled = false;
    int ExitStatus = 0;

    TPath RootPath; /* path in host namespace, set at start */
    int LoopDev = -1; /* legacy */
    std::shared_ptr<TVolume> RootVolume;
    std::vector<std::string> Place;

    TTask Task;
    pid_t TaskVPid;
    TTask WaitTask;
    TTask SeizeTask;
    std::shared_ptr<TNetwork> Net;

    std::string GetCwd() const;
    TPath WorkPath() const;

    bool IsMeta() const {
        return Command.empty();
    }

    TContainer(std::shared_ptr<TContainer> parent, const std::string &name);
    ~TContainer();

    void Register();

    bool HasProp(EProperty prop) const {
        return PropSet[(int)prop];
    }

    void SetProp(EProperty prop) {
        PropSet[(int)prop] = true;
        PropDirty[(int)prop] = true;
    }

    void ClearProp(EProperty prop) {
        PropSet[(int)prop] = false;
        PropDirty[(int)prop] = true;
    }

    bool TestPropDirty(EProperty prop) const {
        return PropDirty[(int)prop];
    }

    bool TestClearPropDirty(EProperty prop) {
        if (!PropDirty[(int)prop])
            return false;
        PropDirty[(int)prop] = false;
        return true;
    }

    std::string GetPortoNamespace(bool write = false) const;

    TError Lock(TScopedLock &lock, bool for_read = false, bool try_lock = false);
    TError LockRead(TScopedLock &lock, bool try_lock = false) {
        return Lock(lock, true, try_lock);
    }
    void Unlock(bool locked = false);
    static void DumpLocks();

    /* Only for temporary write lock downgrade
     * with subsequent upgrade */
    void DowngradeLock(void);
    void UpgradeLock(void);

    TStringMap GetUlimit() const;
    void SanitizeCapabilities();
    uint64_t GetTotalMemGuarantee(bool locked = false) const;
    uint64_t GetTotalMemLimit(const TContainer *base = nullptr) const;

    bool IsRoot() const { return !Level; }
    bool IsChildOf(const TContainer &ct) const;

    std::list<std::shared_ptr<TContainer>> Subtree();

    std::shared_ptr<TContainer> GetParent() const;
    TError OpenNetns(TNamespaceFd &netns) const;

    TError GetNetStat(ENetStat kind, TUintMap &stat);

    TError GetPidFor(pid_t pidns, pid_t &pid) const;

    TError StartTask();
    TError Start();
    TError Stop(uint64_t timeout);
    TError Pause();
    TError Resume();
    TError Terminate(uint64_t deadline);
    TError Kill(int sig);
    TError Destroy();

    TError GetProperty(const std::string &property, std::string &value) const;
    TError SetProperty(const std::string &property, const std::string &value);

    void ForgetPid();
    void SyncState();
    TError Seize();
    TError SyncCgroups();

    TError Save(void);
    TError Load(const TKeyValue &node);

    TCgroup GetCgroup(const TSubsystem &subsystem) const;
    std::shared_ptr<TContainer> FindRunningParent() const;

    void AddWaiter(std::shared_ptr<TContainerWaiter> waiter);

    void ChooseTrafficClasses();
    TError UpdateTrafficClasses();
    TError CreateIngressQdisc();

    bool MayRespawn();
    bool MayReceiveOom(int fd);
    bool HasOomReceived();

    /* protected with VolumesLock */
    std::list<std::shared_ptr<TVolume>> Volumes;

    TError GetEnvironment(TEnv &env);

    static TError ValidName(const std::string &name);
    static std::string ParentName(const std::string &name);

    static std::string StateName(EContainerState state);
    static EContainerState ParseState(const std::string &name);

    static std::shared_ptr<TContainer> Find(const std::string &name);
    static TError Find(const std::string &name, std::shared_ptr<TContainer> &ct);
    static TError FindTaskContainer(pid_t pid, std::shared_ptr<TContainer> &ct);

    static TError Create(const std::string &name, std::shared_ptr<TContainer> &ct);
    static TError Restore(const TKeyValue &kv, std::shared_ptr<TContainer> &ct);

    static void Event(const TEvent &event);
};

class TContainerWaiter {
private:
    static std::mutex WildcardLock;
    static std::list<std::weak_ptr<TContainerWaiter>> WildcardWaiters;
    std::weak_ptr<TClient> Client;
    std::function<void (std::shared_ptr<TClient>, TError, std::string)> Callback;
public:
    TContainerWaiter(std::shared_ptr<TClient> client,
                     std::function<void (std::shared_ptr<TClient>, TError, std::string)> callback);
    void WakeupWaiter(const TContainer *who, bool wildcard = false);
    static void WakeupWildcard(const TContainer *who);
    static void AddWildcard(std::shared_ptr<TContainerWaiter> &waiter);

    std::vector<std::string> Wildcards;
    bool MatchWildcard(const std::string &name);
};

extern std::mutex ContainersMutex;
extern std::shared_ptr<TContainer> RootContainer;
extern std::map<std::string, std::shared_ptr<TContainer>> Containers;
extern TPath ContainersKV;
extern TIdMap ContainerIdMap;

static inline std::unique_lock<std::mutex> LockContainers() {
    return std::unique_lock<std::mutex>(ContainersMutex);
}

extern std::mutex CpuAffinityMutex;

static inline std::unique_lock<std::mutex> LockCpuAffinity() {
    return std::unique_lock<std::mutex>(CpuAffinityMutex);
}
