#pragma once

#include <string>
#include <vector>
#include <list>
#include <memory>

#include "util/unix.hpp"
#include "util/locks.hpp"
#include "util/log.hpp"
#include "stream.hpp"
#include "cgroup.hpp"
#include "property.hpp"

class TEpollSource;
class TCgroup;
class TSubsystem;
class TPropertyMap;
class TValueMap;
class TEvent;
class TContainerHolder;
enum class ENetStat;
class TNetwork;
class TNamespaceFd;
class TNlLink;
class TContainerWaiter;
class TClient;
class TVolume;
class TKeyValue;
struct TBindMount;

struct TEnv;

enum class EContainerState {
    Unknown,
    Stopped,
    Dead,
    Running,
    Paused,
    Meta
};

class TProperty;

class TContainer : public std::enable_shared_from_this<TContainer>,
                   public TNonCopyable,
                   public TLockable {
    friend class TProperty;

    std::shared_ptr<TContainerHolder> Holder;
    const std::string Name;
    int Acquired = 0;
    int Id;
    TFile OomEvent;
    size_t RunningChildren = 0; // changed under holder lock
    std::list<std::weak_ptr<TContainerWaiter>> Waiters;

    std::shared_ptr<TEpollSource> Source;
    int Level; // 0 for root, 1 for porto_root, etc

    // data
    void UpdateRunningChildren(size_t diff);
    TError UpdateSoftLimit();
    void SetState(EContainerState newState);

    TError ApplyDynamicProperties();
    TError PrepareWorkDir();
    TError RestoreNetwork();
    TError PrepareOomMonitor();
    void ShutdownOom();
    TError PrepareCgroups();
    TError ConfigureDevices(std::vector<TDevice> &devices);
    TError ParseNetConfig(struct TNetCfg &NetCfg);
    TError PrepareNetwork(struct TNetCfg &NetCfg);
    TError PrepareTask(struct TTaskEnv *TaskEnv,
                       struct TNetCfg *NetCfg);
    void RemoveKvs();

    const std::string StripParentName(const std::string &name) const;
    void ScheduleRespawn();
    TError Respawn(TScopedLock &holder_lock);
    void StopChildren(TScopedLock &holder_lock);
    TError PrepareResources();
    void FreeResources();

    void Reap(TScopedLock &holder_lock, bool oomKilled);
    void Exit(TScopedLock &holder_lock, int status, bool oomKilled);

    void CleanupWaiters();
    void NotifyWaiters();

    // fn called for parent first then for all children (from top container to the leafs)
    TError ApplyForTreePreorder(TScopedLock &holder_lock,
                                std::function<TError (TScopedLock &holder_lock,
                                                      TContainer &container)> fn);
    // fn called for children first then for all parents (from leaf containers to the top)
    TError ApplyForTreePostorder(TScopedLock &holder_lock,
                                 std::function<TError (TScopedLock &holder_lock,
                                                       TContainer &container)> fn);

public:
    const std::shared_ptr<TContainer> Parent;
    bool PropSet[(int)EProperty::NR_PROPERTIES];
    bool PropDirty[(int)EProperty::NR_PROPERTIES];
    TCred OwnerCred;
    std::string Command;
    std::string Cwd;
    TStdStream Stdin, Stdout, Stderr;
    std::string Root;
    bool RootRo;
    mode_t Umask;
    int VirtMode = 0;
    bool BindDns;
    bool Isolate;
    std::vector<std::string> NetProp;
    std::vector<std::weak_ptr<TContainer>> Children;
    std::string Hostname;
    std::vector<std::string> EnvCfg;
    std::vector<TBindMount> BindMounts;
    std::vector<std::string> IpList;
    TCapabilities CapAmbient;   /* get at start */
    TCapabilities CapAllowed;   /* can set as ambient */
    TCapabilities CapLimit;     /* can get by suid */
    std::vector<std::string> DefaultGw;
    std::vector<std::string> ResolvConf;
    std::vector<std::string> Devices;

    int LoopDev = -1; /* legacy */
    uint64_t StartTime;
    uint64_t DeathTime;
    std::map<int, struct rlimit> Rlimit;
    std::string NsName;

    uint64_t MemLimit = 0;
    uint64_t MemGuarantee = 0;
    uint64_t NewMemGuarantee = 0;
    uint64_t AnonMemLimit = 0;
    uint64_t DirtyMemLimit = 0;

    bool RechargeOnPgfault = false;

    std::string IoPolicy;
    uint64_t IoLimit = 0;
    uint64_t IopsLimit = 0;

    std::string CpuPolicy;
    double CpuLimit;
    double CpuGuarantee;

    TUintMap NetGuarantee;
    TUintMap NetLimit;
    TUintMap NetPriority;

    bool ToRespawn;
    int MaxRespawns;
    uint64_t RespawnCount;
    std::string Private;
    uint64_t AgingTime;
    EAccessLevel AccessLevel;
    bool IsWeak;
    EContainerState State = EContainerState::Unknown;
    bool OomKilled = false;
    int ExitStatus = 0;
    TPath RootPath; /* path in host namespace */
    std::shared_ptr<TVolume> RootVolume;

    TTask Task;
    pid_t TaskVPid;
    TTask WaitTask;
    std::shared_ptr<TNetwork> Net;

    std::string GetCwd() const;
    TPath WorkPath() const;
    EContainerState GetState() const {
        return State;
    }
    bool IsValid() {
        return State != EContainerState::Unknown;
    }
    bool IsMeta() {
        return !Command.size();
    }
    TContainer(std::shared_ptr<TContainerHolder> holder,
               const std::string &name, std::shared_ptr<TContainer> parent,
               int id);
    ~TContainer();

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

    bool TestClearPropDirty(EProperty prop) {
        if (!PropDirty[(int)prop])
            return false;
        PropDirty[(int)prop] = false;
        return true;
    }

    std::string GetPortoNamespace() const;
    std::string ContainerStateName(EContainerState state);

    void AcquireForced();
    bool Acquire();
    void Release();
    bool IsAcquired() const;

    void SanitizeCapabilities();

    const std::string GetName() const;
    const std::string GetTextId(const std::string &separator = "+") const;
    const int GetId() const { return Id; }
    const int GetLevel() const { return Level; }

    uint64_t GetTotalMemGuarantee(void) const;
    uint64_t GetTotalMemLimit(const TContainer *base = nullptr) const;

    bool IsRoot() const;
    bool IsPortoRoot() const;
    bool IsChildOf(const TContainer &ct) const;

    std::shared_ptr<const TContainer> GetRoot() const;
    std::shared_ptr<TContainer> GetParent() const;
    std::shared_ptr<const TContainer> GetIsolationDomain() const;
    TError OpenNetns(TNamespaceFd &netns) const;

    TError GetNetStat(ENetStat kind, TUintMap &stat);
    uint32_t GetTrafficClass() const;

    pid_t GetPidFor(pid_t pid) const;
    std::vector<pid_t> Processes();

    void AddChild(std::shared_ptr<TContainer> child);
    TError Create(const TCred &cred);
    void Destroy(void);
    void DestroyWeak();
    TError Start(bool meta);
    TError StopOne(TScopedLock &holder_lock, uint64_t deadline);
    TError Stop(TScopedLock &holder_lock, uint64_t timeout);
    TError CheckAcquiredChild(TScopedLock &holder_lock);

    TError Pause(TScopedLock &holder_lock);
    TError Resume(TScopedLock &holder_lock);

    TError Terminate(TScopedLock &holder_lock, uint64_t deadline);
    TError Kill(int sig);

    TError GetProperty(const std::string &property, std::string &value) const;
    TError SetProperty(const std::string &property, const std::string &value);

    TError Restore(TScopedLock &holder_lock, const TKeyValue &node);
    void SyncState(TScopedLock &holder_lock);

    TError Save(void);
    TError Load(const TKeyValue &node);

    TCgroup GetCgroup(const TSubsystem &subsystem) const;
    bool CanRemoveDead() const;
    std::vector<std::string> GetChildren();
    std::shared_ptr<TContainer> FindRunningParent() const;
    void DeliverEvent(TScopedLock &holder_lock, const TEvent &event);

    static void ParsePropertyName(std::string &name, std::string &idx);
    size_t GetRunningChildren() { return RunningChildren; }

    void AddWaiter(std::shared_ptr<TContainerWaiter> waiter);

    void CleanupExpiredChildren();
    TError UpdateTrafficClasses();

    bool MayRespawn();
    bool MayReceiveOom(int fd);
    bool HasOomReceived();

    /* protected with VolumesLock */
    std::list<std::shared_ptr<TVolume>> Volumes;

    TError GetEnvironment(TEnv &env);

    static std::shared_ptr<TContainer> Find(const std::string &name);
    static TError Find(const std::string &name, std::shared_ptr<TContainer> &ct);
};

class TScopedAcquire : public TNonCopyable {
    std::shared_ptr<TContainer> Container;
    bool Acquired;

public:
    TScopedAcquire(std::shared_ptr<TContainer> c) : Container(c) {
        if (Container)
            Acquired = Container->Acquire();
        else
            Acquired = true;
    }
    ~TScopedAcquire() {
        if (Acquired && Container)
            Container->Release();
    }

    bool IsAcquired() { return Acquired; }
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
extern std::map<std::string, std::shared_ptr<TContainer>> Containers;
extern TPath ContainersKV;

static inline std::unique_lock<std::mutex> LockContainers() {
    return std::unique_lock<std::mutex>(ContainersMutex);
}
