#include <sstream>
#include <fstream>
#include <memory>
#include <csignal>
#include <cstdlib>
#include <algorithm>

#include "statistics.hpp"
#include "container.hpp"
#include "config.hpp"
#include "task.hpp"
#include "cgroup.hpp"
#include "device.hpp"
#include "property.hpp"
#include "event.hpp"
#include "holder.hpp"
#include "network.hpp"
#include "context.hpp"
#include "epoll.hpp"
#include "kvalue.hpp"
#include "volume.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"
#include "util/loop.hpp"
#include "client.hpp"
#include "filesystem.hpp"

extern "C" {
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
}

std::mutex ContainersMutex;
std::map<std::string, std::shared_ptr<TContainer>> Containers;
TPath ContainersKV;

using std::string;
using std::vector;
using std::shared_ptr;
using std::unique_ptr;
using std::map;

std::shared_ptr<TContainer> TContainer::Find(const std::string &name) {
    PORTO_LOCKED(ContainersMutex);
    auto it = Containers.find(name);
    if (it == Containers.end())
        return nullptr;
    return it->second;
}

TError TContainer::Find(const std::string &name, std::shared_ptr<TContainer> &ct) {
    ct = Find(name);
    if (ct)
        return TError::Success();
    return TError(EError::ContainerDoesNotExist, "container " + name + " not found");
}

TContainer::TContainer(std::shared_ptr<TContainerHolder> holder,
                       const std::string &name, std::shared_ptr<TContainer> parent,
                       int id) :
    Holder(holder), Name(StripParentName(name)),
    Id(id), Level(parent == nullptr ? 0 : parent->GetLevel() + 1),
    Parent(parent),
    Stdin(0), Stdout(1), Stderr(2)
{
    Statistics->Containers++;
    std::fill(PropSet, PropSet + sizeof(PropSet), false);
    std::fill(PropDirty, PropDirty + sizeof(PropDirty), false);

    if (IsRoot() || IsPortoRoot())
        Cwd = "/";
    else
        Cwd = WorkPath().ToString();

    Stdin.SetOutside("/dev/null");
    Stdout.SetOutside("stdout");
    Stderr.SetOutside("stderr");
    Stdout.Limit = config().container().stdout_limit();
    Stderr.Limit = config().container().stdout_limit();
    Root = "/";
    RootPath = TPath("/");
    RootRo = false;
    Umask = 0002;
    Isolate = true;
    BindDns = true;
    VirtMode = VIRT_MODE_APP;
    NetProp = { "inherited" };
    Hostname = "";
    CapAmbient = NoCapabilities;
    CapAllowed = NoCapabilities;
    CapLimit = NoCapabilities;

    if (IsRoot())
        NsName = std::string(PORTO_ROOT_CONTAINER) + "/";
    else if (IsPortoRoot())
        NsName = "";
    else if (config().container().default_porto_namespace())
        NsName = Name + "/";
    else
        NsName = "";

    CpuPolicy = "normal";
    CpuLimit = GetNumCores();
    CpuGuarantee = 0;
    IoPolicy = "normal";

    NetPriority["default"] = NET_DEFAULT_PRIO;
    ToRespawn = false;
    MaxRespawns = -1;
    RespawnCount = 0;
    Private = "";
    AgingTime = config().container().default_aging_time_s();

    if (Parent && Parent->AccessLevel < EAccessLevel::ChildOnly)
        AccessLevel = Parent->AccessLevel;
    else
        AccessLevel = EAccessLevel::Normal;

    IsWeak = false;
}

TContainer::~TContainer() {
    // so call them explicitly in Tcontainer::Destroy()
    PORTO_ASSERT(Net == nullptr);
    Statistics->Containers--;
};

std::string TContainer::ContainerStateName(EContainerState state) {
    switch (state) {
    case EContainerState::Stopped:
        return "stopped";
    case EContainerState::Dead:
        return "dead";
    case EContainerState::Running:
        return "running";
    case EContainerState::Paused:
        return "paused";
    case EContainerState::Meta:
        return "meta";
    default:
        return "unknown";
    }
}

/* Working directory in host namespace */
TPath TContainer::WorkPath() const {
    return TPath(config().container().tmp_dir()) / GetName();
}

std::string TContainer::GetCwd() const {
    for (auto ct = shared_from_this(); ct; ct = ct->Parent) {
        if (ct->HasProp(EProperty::CWD))
            return ct->Cwd;
        if (ct->Root != "/")
            return "/";
    }
    return Cwd;
}

TError TContainer::GetNetStat(ENetStat kind, TUintMap &stat) {
    if (Net) {
        auto lock = Net->ScopedLock();
        return Net->GetTrafficStat(GetTrafficClass(), kind, stat);
    } else
        return TError(EError::NotSupported, "Network statistics is not available");
}

void TContainer::UpdateRunningChildren(size_t diff) {
    RunningChildren += diff;

    if (!RunningChildren && State == EContainerState::Meta)
        NotifyWaiters();

    if (Parent)
        Parent->UpdateRunningChildren(diff);
}

TError TContainer::UpdateSoftLimit() {
    if (IsRoot() || IsPortoRoot())
        return TError::Success();

    if (Parent)
        Parent->UpdateSoftLimit();

    if (State == EContainerState::Meta) {
        uint64_t defaultLimit;

        auto rootCg = MemorySubsystem.RootCgroup();
        TError error = MemorySubsystem.GetSoftLimit(rootCg, defaultLimit);
        if (error)
            return error;

        uint64_t limit = RunningChildren ? defaultLimit : 1 * 1024 * 1024;
        uint64_t currentLimit;

        auto cg = GetCgroup(MemorySubsystem);
        error = MemorySubsystem.GetSoftLimit(cg, currentLimit);
        if (error)
            return error;

        if (currentLimit != limit) {
            error = MemorySubsystem.SetSoftLimit(cg, limit);
            if (error)
                return error;
        }
    }

    return TError::Success();
}

void TContainer::SetState(EContainerState newState) {
    if (State == newState)
        return;

    L_ACT() << GetName() << ": change state " << ContainerStateName(State) << " -> " << ContainerStateName(newState) << std::endl;
    if (newState == EContainerState::Running) {
        UpdateRunningChildren(+1);
    } else if (State == EContainerState::Running) {
        UpdateRunningChildren(-1);
    }

    State = newState;

    if (newState != EContainerState::Running && newState != EContainerState::Meta)
        NotifyWaiters();
}

const string TContainer::StripParentName(const string &name) const {
    if (name == ROOT_CONTAINER)
        return ROOT_CONTAINER;
    else if (name == PORTO_ROOT_CONTAINER)
        return PORTO_ROOT_CONTAINER;

    std::string::size_type n = name.rfind('/');
    if (n == std::string::npos)
        return name;
    else
        return name.substr(n + 1);
}

void TContainer::RemoveKvs() {
    if (IsRoot() || IsPortoRoot())
        return;

    TPath path(ContainersKV / std::to_string(Id));
    TError error = path.Unlink();
    if (error)
        L_ERR() << "Can't remove key-value node " << path << ": " << error << std::endl;
}

void TContainer::Destroy(void) {
    L_ACT() << "Destroy " << GetName() << " " << Id << std::endl;

    while (!Volumes.empty()) {
        std::shared_ptr<TVolume> volume = Volumes.back();
        if (!volume->UnlinkContainer(*this) && volume->IsDying)
            volume->Destroy();
    }

    if (Net) {
        auto lock = Net->ScopedLock();
        Net = nullptr;
    }
    RemoveKvs();
}

void TContainer::DestroyWeak() {
    if (IsWeak) {
        TEvent event(EEventType::DestroyWeak, shared_from_this());
        Holder->Queue->Add(0, event);
    }
}

const std::string TContainer::GetName() const {
    if (IsRoot() || IsPortoRoot() || Parent->IsPortoRoot())
        return Name;
    return Parent->GetName() + "/" + Name;
}

const std::string TContainer::GetTextId(const std::string &separator) const {
     if (IsRoot() || IsPortoRoot() || Parent->IsPortoRoot())
         return Name;
     return Parent->GetTextId(separator) + separator + Name;
}

bool TContainer::IsRoot() const {
    return Id == ROOT_CONTAINER_ID;
}

bool TContainer::IsPortoRoot() const {
    return Id == PORTO_ROOT_CONTAINER_ID;
}

std::shared_ptr<const TContainer> TContainer::GetRoot() const {
    if (Parent)
        return Parent->GetRoot();
    else
        return shared_from_this();
}

bool TContainer::IsChildOf(const TContainer &ct) const {
    for (auto ptr = Parent.get(); ptr; ptr = ptr->Parent.get()) {
        if (ptr == &ct)
            return true;
    }
    return false;
}

std::shared_ptr<TContainer> TContainer::GetParent() const {
    return Parent;
}

std::shared_ptr<const TContainer> TContainer::GetIsolationDomain() const {
    auto domain = shared_from_this();
    while (!domain->Isolate && domain->Parent)
        domain = domain->Parent;
    return domain;
}

pid_t TContainer::GetPidFor(pid_t pid) const {
    if (!Task.Pid)
        return 0;
    if (InPidNamespace(pid, getpid()))
        return Task.Pid;
    if (WaitTask.Pid != Task.Pid && InPidNamespace(pid, WaitTask.Pid))
        return TaskVPid;
    if (InPidNamespace(pid, Task.Pid)) {
        if (!Isolate)
            return TaskVPid;
        if (VirtMode == VIRT_MODE_OS)
            return 1;
        return 2;
    }
    return 0;
}

TError TContainer::OpenNetns(TNamespaceFd &netns) const {
    if (Task.Pid)
        return netns.Open(Task.Pid, "ns/net");
    if (Net == GetRoot()->Net)
        return netns.Open(GetTid(), "ns/net");
    return TError(EError::InvalidValue, "Cannot open netns: container not running");
}

uint64_t TContainer::GetTotalMemGuarantee(void) const {
    uint64_t sum = 0lu;

    for (auto iter : Children)
        if (auto child = iter.lock())
            sum += child->GetTotalMemGuarantee();

    return std::max(NewMemGuarantee, sum);
}

uint64_t TContainer::GetTotalMemLimit(const TContainer *base) const {
    uint64_t lim = 0;

    /* Container without load limited with total limit of childrens */
    if (Command.empty() && VirtMode == VIRT_MODE_APP) {
        for (auto it : Children) {
            if (auto child = it.lock()) {
                auto child_lim = child->GetTotalMemLimit(this);
                if (!child_lim || child_lim > UINT64_MAX - lim) {
                    lim = 0;
                    break;
                }
                lim += child_lim;
            }
        }
    }

    for (auto p = this; p && p != base; p = p->Parent.get()) {
        if (p->MemLimit && (p->MemLimit < lim || !lim))
            lim = p->MemLimit;
    }

    return lim;
}

vector<pid_t> TContainer::Processes() {
    auto cg = GetCgroup(FreezerSubsystem);

    vector<pid_t> ret;
    cg.GetProcesses(ret);
    return ret;
}

TError TContainer::ApplyDynamicProperties() {
    auto memcg = GetCgroup(MemorySubsystem);
    TError error;

    if (TestClearPropDirty(EProperty::MEM_GUARANTEE)) {
        error = MemorySubsystem.SetGuarantee(memcg, MemGuarantee);
        if (error) {
            L_ERR() << "Can't set " << P_MEM_GUARANTEE << ": " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::MEM_LIMIT)) {
        error = MemorySubsystem.SetLimit(memcg, MemLimit);
        if (error) {
            if (error.GetErrno() == EBUSY)
                return TError(EError::InvalidValue, std::to_string(MemLimit) + " is too low");

            L_ERR() << "Can't set " << P_MEM_LIMIT << ": " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::ANON_LIMIT)) {
        error = MemorySubsystem.SetAnonLimit(memcg, AnonMemLimit);
        if (error) {
            L_ERR() << "Can't set " << P_ANON_LIMIT << ": " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::DIRTY_LIMIT)) {
        error = MemorySubsystem.SetDirtyLimit(memcg, DirtyMemLimit);
        if (error) {
            L_ERR() << "Can't set " << P_DIRTY_LIMIT << ": " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::RECHARGE_ON_PGFAULT)) {
        error = MemorySubsystem.RechargeOnPgfault(memcg, RechargeOnPgfault);
        if (error) {
            L_ERR() << "Can't set " << P_RECHARGE_ON_PGFAULT << ": " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::IO_LIMIT)) {
        error = MemorySubsystem.SetIoLimit(memcg, IoLimit);
        if (error) {
            L_ERR() << "Can't set " << P_IO_LIMIT << ": " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::IO_OPS_LIMIT)) {
        error = MemorySubsystem.SetIopsLimit(memcg, IopsLimit);
        if (error) {
            L_ERR() << "Can't set " << P_IO_OPS_LIMIT << ": " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::IO_POLICY)) {
        auto blkcg = GetCgroup(BlkioSubsystem);
        error = BlkioSubsystem.SetIoPolicy(blkcg, IoPolicy);
        if (error) {
            L_ERR() << "Can't set " << P_IO_POLICY << ": " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::CPU_POLICY) |
            TestClearPropDirty(EProperty::CPU_LIMIT) |
            TestClearPropDirty(EProperty::CPU_GUARANTEE)) {
        auto cpucg = GetCgroup(CpuSubsystem);
        error = CpuSubsystem.SetCpuPolicy(cpucg, CpuPolicy,
                                          CpuGuarantee, CpuLimit);
        if (error) {
            L_ERR() << "Cannot set cpu policy: " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::NET_PRIO) |
            TestClearPropDirty(EProperty::NET_LIMIT) |
            TestClearPropDirty(EProperty::NET_GUARANTEE)) {
        error = UpdateTrafficClasses();
        if (error) {
            L_ERR() << "Cannot update tc : " << error << std::endl;
            return error;
        }
    }

    return TError::Success();
}

std::shared_ptr<TContainer> TContainer::FindRunningParent() const {
    auto p = Parent;
    while (p) {
        if (p->Task.Pid)
            return p;
        p = p->Parent;
    }

    return nullptr;
}

void TContainer::ShutdownOom() {
    if (Source)
        Holder->EpollLoop->RemoveSource(Source->Fd);
    Source = nullptr;
    OomEvent.Close();
}

TError TContainer::PrepareOomMonitor() {
    TCgroup memoryCg = GetCgroup(MemorySubsystem);
    TError error;

    error = MemorySubsystem.SetupOOMEvent(memoryCg, OomEvent);
    if (error)
        return error;

    Source = std::make_shared<TEpollSource>(Holder->EpollLoop, OomEvent.Fd,
                                            EPOLL_EVENT_OOM, shared_from_this());

    error = Holder->EpollLoop->AddSource(Source);
    if (error)
        ShutdownOom();

    return error;
}

TError TContainer::ConfigureDevices(std::vector<TDevice> &devices) {
    auto cg = GetCgroup(DevicesSubsystem);
    TDevice device;
    TError error;

    if (IsRoot() || IsPortoRoot())
        return TError::Success();

    if (Parent->IsPortoRoot() &&
            (HasProp(EProperty::DEVICES) || !OwnerCred.IsRootUser())) {
        error = DevicesSubsystem.ApplyDefault(cg);
        if (error)
            return error;
    }

    for (auto &cfg: Devices) {
        error = device.Parse(cfg);
        if (error)
            return TError(error, "device: " + cfg);

        error = device.Permitted(OwnerCred);
        if (error)
            return TError(error, "device: " + cfg);

        error = DevicesSubsystem.ApplyDevice(cg, device);
        if (error)
            return TError(error, "device: " + cfg);

        devices.push_back(device);
    }

    return TError::Success();
}

TError TContainer::PrepareCgroups() {
    TError error;

    for (auto hy: Hierarchies) {
        TCgroup cg = GetCgroup(*hy);

        if (cg.Exists()) //FIXME kludge for root and restore
            continue;

        error = cg.Create();
        if (error)
            return error;
    }

    if (IsPortoRoot()) {
        error = GetCgroup(MemorySubsystem).SetBool(MemorySubsystem.USE_HIERARCHY, true);
        if (error)
            return error;
    }

    if (!IsRoot() && !IsPortoRoot()) {
        error = PrepareOomMonitor();
        if (error) {
            L_ERR() << "Can't prepare OOM monitoring: " << error << std::endl;
            return error;
        }
    }

    return TError::Success();
}

void TContainer::CleanupExpiredChildren() {
    for (auto iter = Children.begin(); iter != Children.end();) {
        auto child = iter->lock();
        if (child) {
            iter++;
            continue;
        }

        iter = Children.erase(iter);
    }
}

uint32_t TContainer::GetTrafficClass() const {
    return TcHandle(ROOT_TC_MAJOR, Id);
}

TError TContainer::ParseNetConfig(struct TNetCfg &NetCfg) {
    TError error;

    NetCfg.Parent = Parent;
    NetCfg.Id = Id;
    NetCfg.Hostname = Hostname;
    NetCfg.NetUp = VirtMode != VIRT_MODE_OS;
    NetCfg.OwnerCred = OwnerCred;

    error = NetCfg.ParseNet(NetProp);
    if (error)
        return error;

    error = NetCfg.ParseIp(IpList);
    if (error)
        return error;

    error = NetCfg.ParseGw(DefaultGw);
    if (error)
        return error;

    if (Parent)
        NetCfg.ParentNet = Parent->Net;

    if (Net)
        NetCfg.Net = Net;

    return TError::Success();
}

TError TContainer::PrepareNetwork(struct TNetCfg &NetCfg) {
    TError error;

    error = NetCfg.PrepareNetwork();
    if (error)
        return error;

    if (NetCfg.SaveIp) {
        std::vector<std::string> lines;
        error = NetCfg.FormatIp(lines);
        if (error)
            return error;
        IpList = lines;
    }

    Net = NetCfg.Net;

    error = UpdateTrafficClasses();
    if (error) {
        L_ACT() << "Refresh network" << std::endl;
        Net->RefreshClasses(true);
        error = UpdateTrafficClasses();
        if (error) {
            L_ERR() << "Network refresh failed" << std::endl;
            return error;
        }
    }

    if (!IsRoot()) {
        auto netcls = GetCgroup(NetclsSubsystem);
        error = netcls.Set("net_cls.classid",
                           std::to_string(GetTrafficClass()));
        if (error) {
            L_ERR() << "Can't set classid: " << error << std::endl;
            return error;
        }
    }

    return TError::Success();
}

TError TContainer::GetEnvironment(TEnv &env) {
    env.ClearEnv();

    env.SetEnv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    env.SetEnv("HOME", GetCwd());
    env.SetEnv("USER", UserName(OwnerCred.Uid));

    env.SetEnv("container", "lxc");

    /* lock these two */
    env.SetEnv("PORTO_NAME", GetName(), true, true);
    env.SetEnv("PORTO_HOST", GetHostName(), true, true);

    /* Inherit environment from containts in isolation domain */
    bool overwrite = true;
    for (auto ct = this; ct; ct = ct->Parent.get()) {
        TError error = env.Parse(EnvCfg, overwrite);
        if (error && overwrite)
            return error;
        overwrite = false;

        if (ct->Isolate)
            break;
    }

    return TError::Success();
}

TError TContainer::PrepareTask(struct TTaskEnv *taskEnv,
                               struct TNetCfg *NetCfg) {
    auto user = UserName(OwnerCred.Uid);
    auto parent = FindRunningParent();
    TError error;

    taskEnv->CT = shared_from_this();
    taskEnv->Client = CurrentClient;

    for (auto hy: Hierarchies)
        taskEnv->Cgroups.push_back(GetCgroup(*hy));

    taskEnv->Mnt.Cwd = GetCwd();
    taskEnv->Mnt.ParentCwd = Parent->GetCwd();

    if (RootVolume)
        taskEnv->Mnt.Root = Parent->RootPath.InnerPath(RootVolume->Path);
    else
        taskEnv->Mnt.Root = Root;

    taskEnv->Mnt.RootRdOnly = RootRo;

    taskEnv->Mnt.RunSize = (GetTotalMemLimit() ?: GetTotalMemory()) / 2;

    taskEnv->Mnt.OwnerCred = OwnerCred;

    if (VirtMode == VIRT_MODE_OS) {
        user = "root";
        taskEnv->Cred = TCred(0, 0);
    } else {
        taskEnv->Cred = OwnerCred;
        error = taskEnv->Cred.LoadGroups(user);
        if (error)
            return error;
    }

    error = GetEnvironment(taskEnv->Env);
    if (error)
        return error;

    taskEnv->TripleFork = false;
    taskEnv->QuadroFork = (VirtMode == VIRT_MODE_APP) &&
                          Isolate && !Command.empty();

    taskEnv->Mnt.BindMounts = BindMounts;
    taskEnv->Mnt.BindPortoSock = AccessLevel != EAccessLevel::None;


    error = ConfigureDevices(taskEnv->Devices);
    if (error) {
        L_ERR() << "Cannot configure devices: " << error << std::endl;
        return error;
    }

    if (parent) {
        pid_t parent_pid = parent->Task.Pid;

        error = taskEnv->ParentNs.Open(parent_pid);
        if (error)
            return error;

        /* one more fork for creating nested pid-namespace */
        if (Isolate && !InPidNamespace(parent_pid, getpid()))
            taskEnv->TripleFork = true;
    }

    if (NetCfg && NetCfg->NetNs.IsOpened())
        taskEnv->ParentNs.Net.EatFd(NetCfg->NetNs);

    if (NetCfg)
        taskEnv->Autoconf = NetCfg->Autoconf;

    if (Command.empty() || taskEnv->TripleFork || taskEnv->QuadroFork) {
        TPath exe("/proc/self/exe");
        TPath path;
        TError error = exe.ReadLink(path);
        if (error)
            return error;
        path = path.DirName() / "portoinit";
        error = taskEnv->PortoInit.OpenRead(path);
        if (error)
            return error;
    }

    // Create new mount namespaces if we have to make any changes
    taskEnv->NewMountNs = Isolate ||
                          taskEnv->Mnt.BindMounts.size() ||
                          Hostname.size() ||
                          ResolvConf.size() ||
                          !taskEnv->Mnt.Root.IsRoot() ||
                          taskEnv->Mnt.RootRdOnly ||
                          !NetCfg->Inherited;

    return TError::Success();
}

void TContainer::AddChild(std::shared_ptr<TContainer> child) {
    Children.push_back(child);
}

TError TContainer::Create(const TCred &cred) {
    L_ACT() << "Create " << GetName() << " with id " << Id << " uid " << cred.Uid << " gid " << cred.Gid << std::endl;

    OwnerCred = cred;
    TError error = OwnerCred.LoadGroups(OwnerCred.User());
    if (error)
        return error;
    SetProp(EProperty::USER);
    SetProp(EProperty::GROUP);

    SanitizeCapabilities();

    SetState(EContainerState::Stopped);
    SetProp(EProperty::STATE);

    RespawnCount = 0;
    SetProp(EProperty::RESPAWN_COUNT);

    return Save(); /* Serialize on creation  */
}

void TContainer::SanitizeCapabilities() {
    TCapabilities allowed, limit;

    /* root user can allow any capabilities in own containers */
    if (OwnerCred.IsRootUser()) {
        allowed = AllCapabilities;
        limit = AllCapabilities;
    } else {
        if (VirtMode == VIRT_MODE_OS) {
            allowed = OsModeCapabilities;
            limit = OsModeCapabilities;
        } else {
            allowed = AppModeCapabilities;
            limit = SuidCapabilities;
        }
        for (auto p = Parent; p; p = p->Parent)
            limit.Permitted &= p->CapLimit.Permitted;
    }

    if (!HasProp(EProperty::CAPABILITIES)) {
        CapLimit = limit;
    } else {
        CapLimit.Permitted &= limit.Permitted;
        limit.Permitted &= CapLimit.Permitted;
    }

    if (HasAmbientCapabilities) {
        allowed.Permitted &= limit.Permitted;
        CapAllowed = allowed;
        CapAmbient.Permitted &= allowed.Permitted;
    }
}

TError TContainer::Start(bool meta) {
    TError error;

    if (State != EContainerState::Stopped)
        return TError(EError::InvalidState, "Container not stopped");

    if (Parent) {
        auto cg = Parent->GetCgroup(FreezerSubsystem);
        if (FreezerSubsystem.IsFrozen(cg))
            return TError(EError::InvalidState, "Parent container is paused");
    }

    /* Normalize root path */
    if (Parent) {
        TPath path(Root);

        path = path.NormalPath();
        if (path.IsDotDot())
            return TError(EError::Permission, "root path with ..");

        RootPath = Parent->RootPath / path;
    }

    if (VirtMode == VIRT_MODE_OS && !OwnerCred.IsRootUser()) {
        if (GetIsolationDomain()->IsRoot())
            return TError(EError::Permission, "virt_mode=os must be isolated from host");
        if (!Isolate && OwnerCred.Uid != Parent->OwnerCred.Uid)
            return TError(EError::Permission, "virt_mode=os without isolation only for root or owner");
        if (RootPath.IsRoot())
            return TError(EError::Permission, "virt_mode=os without chroot only for root");
    }

    /* virt_mode=os overrides some defaults */
    if (VirtMode == VIRT_MODE_OS) {
        if (!HasProp(EProperty::CWD))
            Cwd = "/";

        if (!HasProp(EProperty::COMMAND))
            Command = "/sbin/init";

        if (!HasProp(EProperty::STDOUT))
            Stdout.SetOutside("/dev/null");

        if (!HasProp(EProperty::STDERR))
            Stderr.SetOutside("/dev/null");

        if (!HasProp(EProperty::BIND_DNS))
            BindDns = false;

        if (!HasProp(EProperty::NET))
            NetProp = { "none" };
    }

    /* Non-isolated container inherits policy from parent */
    if (!Isolate && Parent) {
        if (!HasProp(EProperty::CPU_POLICY))
            CpuPolicy = Parent->CpuPolicy;

        if (!HasProp(EProperty::IO_POLICY))
            IoPolicy = Parent->IoPolicy;

        if (!HasProp(EProperty::RECHARGE_ON_PGFAULT))
            RechargeOnPgfault = Parent->RechargeOnPgfault;

        if (!HasProp(EProperty::NET_PRIO))
            NetPriority = Parent->NetPriority;

        if (!HasProp(EProperty::ULIMIT))
            Rlimit = Parent->Rlimit;

        if (!HasProp(EProperty::UMASK))
            Umask = Parent->Umask;
    }

    if (!meta && !Command.length())
        return TError(EError::InvalidValue, "container command is empty");

    /* apply parent limits for capabilities */
    SanitizeCapabilities();

    /*  PidNsCapabilities must be isolated from host pid-namespace */
    if (!Isolate && (CapAmbient.Permitted & PidNsCapabilities.Permitted) &&
            !CurrentClient->IsSuperUser() && GetIsolationDomain()->IsRoot())
        return TError(EError::Permission, "Capabilities require pid isolation: " +
                                          PidNsCapabilities.Format());

    /* MemCgCapabilities requires memory limit */
    if (!MemLimit && (CapAmbient.Permitted & MemCgCapabilities.Permitted) &&
            !CurrentClient->IsSuperUser()) {
        bool limited = false;
        for (auto p = Parent; p; p = p->Parent)
            limited = limited || p->MemLimit;
        if (!limited)
            return TError(EError::Permission, "Capabilities require memory limit: " +
                          MemCgCapabilities.Format());
    }

    /* Propogate lower access levels into child */
    if (Parent && Parent->AccessLevel < EAccessLevel::ChildOnly &&
                  Parent->AccessLevel < AccessLevel)
        AccessLevel = Parent->AccessLevel;

    L_ACT() << "Start " << GetName() << " " << Id << std::endl;

    StartTime = GetCurrentTimeMs();
    SetProp(EProperty::START_TIME);

    error = PrepareResources();
    if (error)
        return error;

    struct TTaskEnv TaskEnv;
    struct TNetCfg NetCfg;

    error = ParseNetConfig(NetCfg);
    if (error)
        return error;

    error = PrepareNetwork(NetCfg);
    if (error)
        goto error;

    if (!IsRoot()) {
        error = ApplyDynamicProperties();
        if (error)
            return error;
    }

    /* NetNsCapabilities must be isoalted from host net-namespace */
    if (Net == GetRoot()->Net && !CurrentClient->IsSuperUser()) {
        if (CapAmbient.Permitted & NetNsCapabilities.Permitted) {
            error = TError(EError::Permission, "Capabilities require net isolation: " +
                                               NetNsCapabilities.Format());
            goto error;
        }
        if (VirtMode == VIRT_MODE_OS) {
            error = TError(EError::Permission, "virt_mode=os must be isolated from host network");
            goto error;
        }
    }

    if (!meta || (meta && Isolate)) {

        error = PrepareTask(&TaskEnv, &NetCfg);
        if (error)
            goto error;

        error = TaskEnv.Start();

        /* Always report OOM stuation if any */
        if (error && HasOomReceived()) {
            if (error)
                L() << "Start error: " << error << std::endl;
            error = TError(EError::InvalidValue, ENOMEM,
                           "OOM, memory limit too low");
        }

        if (error)
            goto error;

        L() << GetName() << " started " << std::to_string(Task.Pid) << std::endl;
        SetProp(EProperty::ROOT_PID);
    }

    if (meta)
        SetState(EContainerState::Meta);
    else
        SetState(EContainerState::Running);
    Statistics->Started++;
    error = UpdateSoftLimit();
    if (error)
        L_ERR() << "Can't update meta soft limit: " << error << std::endl;

    return Save();

error:
    FreeResources();
    return error;
}

TError TContainer::ApplyForTreePreorder(TScopedLock &holder_lock,
                                std::function<TError (TScopedLock &holder_lock,
                                                      TContainer &container)> fn) {
    for (auto iter : Children)
        if (auto child = iter.lock()) {
            TNestedScopedLock lock(*child, holder_lock);
            if (child->IsValid()) {
                TError error = fn(holder_lock, *child);
                if (error)
                    return error;

                error = child->ApplyForTreePreorder(holder_lock, fn);
                if (error)
                    return error;
            }
        }

    return TError::Success();
}

TError TContainer::ApplyForTreePostorder(TScopedLock &holder_lock,
                                std::function<TError (TScopedLock &holder_lock,
                                                      TContainer &container)> fn) {
    for (auto iter : Children)
        if (auto child = iter.lock()) {
            TNestedScopedLock lock(*child, holder_lock);
            if (child->IsValid()) {
                TError error = child->ApplyForTreePostorder(holder_lock, fn);
                if (error)
                    return error;

                error = fn(holder_lock, *child);
                if (error)
                    return error;
            }
        }

    return TError::Success();
}

TError TContainer::PrepareWorkDir() {
    if (IsRoot() || IsPortoRoot())
        return TError::Success();

    TPath work = WorkPath();
    if (work.Exists())
        return TError::Success(); /* FIXME kludge for restore */

    TError error = work.Mkdir(0755);
    if (!error)
        error = work.Chown(OwnerCred);
    return error;
}

TError TContainer::PrepareResources() {
    TError error;

    error = PrepareWorkDir();
    if (error) {
        if (error.GetErrno() == ENOSPC)
            L() << "Cannot create working dir: " << error << std::endl;
        else
            L_ERR() << "Cannot create working dir: " << error << std::endl;
        FreeResources();
        return error;
    }

    error = PrepareCgroups();
    if (error) {
        L_ERR() << "Can't prepare task cgroups: " << error << std::endl;
        FreeResources();
        return error;
    }

    if (HasProp(EProperty::ROOT) && RootPath.IsRegularFollow()) {
        TStringMap cfg;

        cfg[V_BACKEND] = "loop";
        cfg[V_STORAGE] = RootPath.ToString();
        cfg[V_READ_ONLY] = BoolToString(RootRo);

        RootPath = Parent->RootPath;

        error = TVolume::Create(TPath(), cfg, *this, OwnerCred, RootVolume);
        if (error) {
            L_ERR() << "Cannot create root volume: " << error << std::endl;
            FreeResources();
            return error;
        }

        RootPath = RootVolume->Path;
    }

    return TError::Success();
}

void TContainer::FreeResources() {
    TError error;

    ShutdownOom();

    if (!IsRoot()) {
        for (auto hy: Hierarchies) {
            auto cg = GetCgroup(*hy);

            error = cg.Remove();
            (void)error; //Logged inside
        }
    }

    if (Net) {
        struct TNetCfg NetCfg;

        error = ParseNetConfig(NetCfg);
        if (!error)
            error = NetCfg.DestroyNetwork();
        if (NetCfg.SaveIp) {
            std::vector<std::string> lines;
            if (!NetCfg.FormatIp(lines))
                IpList = lines;
        }
        if (error)
            L_ERR() << "Cannot free network resources: " << error << std::endl;

        auto net_lock = Net->ScopedLock();
        error = Net->DestroyTC(GetTrafficClass());
        if (error)
            L_ERR() << "Can't remove traffic class: " << error << std::endl;
        net_lock.unlock();

        if (Net != HostNetwork) {
            net_lock = HostNetwork->ScopedLock();
            error = HostNetwork->DestroyTC(GetTrafficClass());
            if (error)
                L_ERR() << "Can't remove traffic class: " << error << std::endl;
        }
    }

    if (Net && IsRoot()) {
        error = Net->Destroy();
        if (error)
            L_ERR() << "Cannot destroy network: " << error << std::endl;
    }
    Net = nullptr;

    if (IsRoot() || IsPortoRoot())
        return;

    /* Legacy non-volume root on loop device */
    if (LoopDev >= 0) {
        error = PutLoopDev(LoopDev);
        if (error)
            L_ERR() << "Can't put loop device " << LoopDev << ": " << error << std::endl;
        LoopDev = -1;
        SetProp(EProperty::LOOP_DEV);

        TPath tmp(config().container().tmp_dir() + "/" + std::to_string(Id));
        if (tmp.Exists()) {
            error = tmp.RemoveAll();
            if (error)
                L_ERR() << "Can't remove " << tmp << ": " << error << std::endl;
        }
    }

    if (RootVolume) {
        RootVolume->UnlinkContainer(*this);
        RootVolume->Destroy();
        RootVolume = nullptr;
    }

    TPath work_path = WorkPath();
    if (work_path.Exists()) {
        error = work_path.RemoveAll();
        if (error)
            L_ERR() << "Cannot remove working dir: " << error << std::endl;
    }

    Stdout.Remove(*this);
    Stderr.Remove(*this);
}

void TContainer::AcquireForced() {
    if (Verbose)
        L() << "Acquire " << GetName() << " (forced)" << std::endl;
    Acquired++;
}

bool TContainer::Acquire() {
    if (!IsAcquired()) {
        if (Verbose)
            L() << "Acquire " << GetName() << std::endl;
        Acquired++;
        return true;
    }
    return false;
}

void TContainer::Release() {
    if (Verbose)
        L() << "Release " << GetName() << std::endl;
    PORTO_ASSERT(Acquired > 0);
    Acquired--;
}

bool TContainer::IsAcquired() const {
    return (Acquired || (Parent && Parent->IsAcquired()));
}

TError TContainer::Kill(int sig) {
    if (State != EContainerState::Running)
        return TError(EError::InvalidState, "invalid container state ");

    L_ACT() << "Kill " << GetName() << " pid " << Task.Pid << std::endl;
    return Task.Kill(sig);
}

TError TContainer::Terminate(TScopedLock &holder_lock, uint64_t deadline) {
    auto cg = GetCgroup(FreezerSubsystem);
    TError error;

    if (IsRoot())
        return TError(EError::Permission, "Cannot terminate root container");

    L_ACT() << "Terminate tasks in " << GetName() << std::endl;

    if (cg.IsEmpty())
        return TError::Success();

    if (FreezerSubsystem.IsFrozen(cg))
        return TError(EError::Permission, "Cannot terminate paused container");

    if (Task.Pid && deadline && State != EContainerState::Meta) {
        error = Task.Kill(SIGTERM);
        if (!error) {
            TScopedUnlock unlock(holder_lock);
            L_ACT() << "Wait task " << Task.Pid << " after SIGTERM in " << GetName() << std::endl;
            while (Task.Exists() && !Task.IsZombie() &&
                    !WaitDeadline(deadline));
        }
    }

    for (int pass = 0; pass < 3; pass++) {
        if (cg.IsEmpty())
            return TError::Success();
        error = cg.KillAll(SIGKILL);
        if (error)
            return error;
    }

    error = FreezerSubsystem.Freeze(cg);
    if (error)
        return error;
    error = cg.KillAll(SIGKILL);
    if (!FreezerSubsystem.Thaw(cg) && !error) {
        TScopedUnlock unlock(holder_lock);
        while (!cg.IsEmpty() && !WaitDeadline(deadline));
    }

    return error;
}

TError TContainer::StopOne(TScopedLock &holder_lock, uint64_t deadline) {
    TError error;

    if (State == EContainerState::Stopped)
        return TError(EError::InvalidState, "Container already stopped");

    L_ACT() << "Stop " << GetName() << std::endl;

    if (!IsRoot()) {
        error = Terminate(holder_lock, deadline);
        if (error) {
            L_ERR() << "Cannot termiante tasks in container: " << error << std::endl;
            return error;
        }
    }

    Task.Pid = 0;
    TaskVPid = 0;
    WaitTask.Pid = 0;
    ClearProp(EProperty::ROOT_PID);

    DeathTime = 0;
    ClearProp(EProperty::DEATH_TIME);

    ExitStatus = 0;
    ClearProp(EProperty::EXIT_STATUS);

    OomKilled = false;
    ClearProp(EProperty::OOM_KILLED);

    SetState(EContainerState::Stopped);
    FreeResources();

    return Save();
}

TError TContainer::Stop(TScopedLock &holder_lock, uint64_t timeout) {
    uint64_t deadline = timeout ? GetCurrentTimeMs() + timeout : 0;
    auto cg = GetCgroup(FreezerSubsystem);
    TError error;

    if (FreezerSubsystem.IsFrozen(cg)) {
        if (FreezerSubsystem.IsParentFreezing(cg))
            return TError(EError::InvalidState, "Parent container is paused");

        L_ACT() << "Terminate paused container " << GetName() << std::endl;

        error = cg.KillAll(SIGKILL);
        if (error)
            return error;

        ApplyForTreePostorder(holder_lock, [&] (TScopedLock &holder_lock, TContainer &child) {
            auto cg = child.GetCgroup(FreezerSubsystem);
            error = cg.KillAll(SIGKILL);
            if (error)
                return error;
            return FreezerSubsystem.Thaw(cg, false);
        });

        error = FreezerSubsystem.Thaw(cg);
        if (error)
            return error;
    }

    error = ApplyForTreePostorder(holder_lock, [&] (TScopedLock &holder_lock, TContainer &child) {
        if (child.State != EContainerState::Stopped)
            return child.StopOne(holder_lock, deadline);
        return TError::Success();
    });

    if (error)
        return error;

    error = StopOne(holder_lock, deadline);
    if (error)
        return error;

    error = UpdateSoftLimit();
    if (error)
        L_ERR() << "Can't update meta soft limit: " << error << std::endl;

    return TError::Success();
}

void TContainer::Reap(TScopedLock &holder_lock, bool oomKilled) {
    TError error;

    error = Terminate(holder_lock, 0);
    if (error)
        L_WRN() << "Cannot terminate container " << GetName() << " : " << error << std::endl;

    ShutdownOom();

    DeathTime = GetCurrentTimeMs();
    SetProp(EProperty::DEATH_TIME);

    if (oomKilled) {
        OomKilled = oomKilled;
        SetProp(EProperty::OOM_KILLED);
    }

    Task.Pid = 0;
    TaskVPid = 0;
    WaitTask.Pid = 0;
    ClearProp(EProperty::ROOT_PID);

    Stdout.Rotate(*this);
    Stderr.Rotate(*this);

    if (State == EContainerState::Meta)
        SetState(EContainerState::Stopped);
    else
        SetState(EContainerState::Dead);

    error = Save();
    if (error)
        L_WRN() << "Cannot save container state after exit: " << error << std::endl;

    if (MayRespawn())
        ScheduleRespawn();
}

void TContainer::Exit(TScopedLock &holder_lock, int status, bool oomKilled) {

    /* Detect fatal signals: portoinit cannot kill itself */
    if (Isolate && VirtMode == VIRT_MODE_APP && WIFEXITED(status) &&
            WEXITSTATUS(status) > 128 && WEXITSTATUS(status) < 128 + SIGRTMIN)
        status = WEXITSTATUS(status) - 128;

    L_EVT() << "Exit " << GetName() << " " << FormatExitStatus(status)
            << (oomKilled ? " invoked by OOM" : "") << std::endl;

    ExitStatus = status;
    SetProp(EProperty::EXIT_STATUS);

    ApplyForTreePreorder(holder_lock, [&] (TScopedLock &holder_lock,
                                           TContainer &child) {
        if (child.State != EContainerState::Stopped &&
                child.State != EContainerState::Dead)
            child.Reap(holder_lock, oomKilled);
        return TError::Success();
    });

    Reap(holder_lock, oomKilled);
}

TError TContainer::CheckAcquiredChild(TScopedLock &holder_lock) {
    return ApplyForTreePreorder(holder_lock, [] (TScopedLock &holder_lock,
                                                 TContainer &child) {
        if (child.Acquired)
            return TError(EError::Busy, "child " + child.GetName() + " is busy");
        return TError::Success();
    });
}

TError TContainer::Pause(TScopedLock &holder_lock) {
    if (State != EContainerState::Running && State != EContainerState::Meta)
        return TError(EError::InvalidState, "Contaner not running");

    // some child subtree may be in stop/destroy and we don't want
    // to freeze parent in that moment
    TError error = CheckAcquiredChild(holder_lock);
    if (error)
        return error;

    auto cg = GetCgroup(FreezerSubsystem);
    error = FreezerSubsystem.Freeze(cg);
    if (error)
        return error;

    SetState(EContainerState::Paused);
    ApplyForTreePreorder(holder_lock, [&] (TScopedLock &holder_lock,
                                           TContainer &child) {
        if (child.State == EContainerState::Running ||
                child.State == EContainerState::Meta)
            child.SetState(EContainerState::Paused);
        return child.Save();
    });

    return Save();
}

TError TContainer::Resume(TScopedLock &holder_lock) {
    auto cg = GetCgroup(FreezerSubsystem);

    if (FreezerSubsystem.IsParentFreezing(cg))
        return TError(EError::InvalidState, "Parent container is paused");

    if (!FreezerSubsystem.IsSelfFreezing(cg))
        return TError(EError::InvalidState, "Container not paused");

    TError error = FreezerSubsystem.Thaw(cg);
    if (error)
        return error;

    if (State == EContainerState::Paused)
        SetState(Command.size() ? EContainerState::Running :
                                  EContainerState::Meta);

    ApplyForTreePreorder(holder_lock, [&] (TScopedLock &holder_lock,
                                           TContainer &child) {
        auto cg = child.GetCgroup(FreezerSubsystem);
        if (FreezerSubsystem.IsSelfFreezing(cg))
            FreezerSubsystem.Thaw(cg);
        if (child.State == EContainerState::Paused)
            child.SetState(child.Command.size() ? EContainerState::Running :
                                                  EContainerState::Meta);
        return child.Save();
    });

    return Save();
}

void TContainer::ParsePropertyName(std::string &name, std::string &idx) {
    std::vector<std::string> tokens;
    TError error = SplitString(name, '[', tokens);
    if (error || tokens.size() != 2)
        return;

    name = tokens[0];
    idx = StringTrim(tokens[1], " \t\n]");
}

TError TContainer::GetProperty(const string &origProperty, string &value) const {
    std::string property = origProperty;
    auto dot = property.find('.');
    TError error;

    if (dot != string::npos) {
        std::string type = property.substr(0, dot);
        if (State == EContainerState::Stopped)
            return TError(EError::InvalidState,
                          "Not available in stopped state: " + property);
        for (auto subsys: Subsystems) {
            if (subsys->Type == type) {
                auto cg = GetCgroup(*subsys);
                if (!cg.Has(property))
                    break;
                return cg.Get(property, value);
            }
        }
        return TError(EError::InvalidProperty,
                      "Unknown cgroup attribute: " + property);
    }

    std::string idx;
    ParsePropertyName(property, idx);

    auto it = ContainerProperties.find(property);
    if (it == ContainerProperties.end())
        return TError(EError::InvalidProperty,
                              "Unknown container property: " + property);
    auto prop = it->second;

    if (!prop->IsSupported)
        return TError(EError::NotSupported, "Not supported: " + property);

    CurrentContainer = const_cast<TContainer *>(this);
    if (idx.length())
        error = prop->GetIndexed(idx, value);
    else
        error = prop->Get(value);
    CurrentContainer = nullptr;

    return error;
}

TError TContainer::SetProperty(const string &origProperty,
                               const string &origValue) {
    if (IsRoot() || IsPortoRoot())
        return TError(EError::Permission, "System containers are read only");

    string property = origProperty;
    std::string idx;
    ParsePropertyName(property, idx);
    std::string value = StringTrim(origValue);
    TError error;

    auto it = ContainerProperties.find(property);
    if (it == ContainerProperties.end())
        return TError(EError::Unknown, "Invalid property " + property);
    auto prop = it->second;

    if (!prop->IsSupported)
        return TError(EError::NotSupported, property + " is not supported");

    CurrentContainer = this;

    std::string oldValue;
    error = prop->Get(oldValue);

    if (!error) {
        if (idx.length())
            error = prop->SetIndexed(idx, value);
        else
            error = prop->Set(value);
    }

    if (!error && (State == EContainerState::Running ||
                   State == EContainerState::Meta ||
                   State == EContainerState::Paused)) {
        error = ApplyDynamicProperties();
        if (error) {
            (void)prop->Set(oldValue);
            (void)TestClearPropDirty(prop->Prop);
        }
    }

    CurrentContainer = nullptr;

    if (!error)
        error = Save();

    return error;
}

TError TContainer::RestoreNetwork() {
    TNamespaceFd netns;
    TError error;

    error = OpenNetns(netns);
    if (error)
        return error;

    Net = TNetwork::GetNetwork(netns.GetInode());

    /* Create a new one */
    if (!Net) {
        Net = std::make_shared<TNetwork>();
        PORTO_ASSERT(Net);

        error = Net->ConnectNetns(netns);
        if (error)
            return error;

        TNetwork::AddNetwork(netns.GetInode(), Net);

        error = Net->RefreshDevices();
        if (error)
            return error;
        Net->NewManagedDevices = false;
    }

    error = UpdateTrafficClasses();
    if (error)
        return error;

    return TError::Success();
}

TError TContainer::Save(void) {
    TKeyValue node(ContainersKV / std::to_string(Id));
    TError error;

    /* These are not properties */
    node.Set(P_RAW_ID, std::to_string(Id));
    node.Set(P_RAW_NAME, GetName());

    CurrentContainer = this;

    for (auto knob : ContainerProperties) {
        std::string value;

        /* Skip knobs without a value */
        if (knob.second->Prop == EProperty::NONE || !HasProp(knob.second->Prop))
            continue;

        error = knob.second->GetToSave(value);
        if (error)
            break;

        node.Set(knob.first, value);
    }

    CurrentContainer = nullptr;

    if (error)
        return error;

    return node.Save();
}

TError TContainer::Load(const TKeyValue &node) {
    std::string container_state;
    TError error;

    CurrentContainer = this;

    for (auto &kv: node.Data) {
        std::string key = kv.first;
        std::string value = kv.second;

        if (key == D_STATE) {
            /*
             * We need to set state at the last moment
             * because properties depends on the current value
             */
            container_state = value;
            continue;
        }

        if (key == P_RAW_ID || key == P_RAW_NAME)
            continue;

        auto it = ContainerProperties.find(key);
        if (it == ContainerProperties.end()) {
            L_WRN() << "Unknown property: " << key << ", skipped" << std::endl;
            continue;
        }
        auto prop = it->second;

        error = prop->SetFromRestore(value);
        if (error) {
            L_ERR() << "Cannot load " << key << ", skipped" << std::endl;
            continue;
        }

        SetProp(prop->Prop);
    }

    if (container_state.size()) {
        error = ContainerProperties[D_STATE]->SetFromRestore(container_state);
        SetProp(EProperty::STATE);
    } else
        error = TError(EError::Unknown, "Container has no state");

    CurrentContainer = nullptr;

    return error;
}

void TContainer::SyncState(TScopedLock &holder_lock) {
    TCgroup taskCg, freezerCg = GetCgroup(FreezerSubsystem);
    TError error;

    L_ACT() << "Sync " << GetName() << " state " << ContainerStateName(State) << std::endl;

    if (!freezerCg.Exists()) {
        if (State != EContainerState::Stopped)
            L_WRN() << "Freezer not found" << std::endl;
        State = EContainerState::Stopped;
        return;
    }

    if (State == EContainerState::Stopped) {
        L() << "Found unexpected freezer" << std::endl;
        Reap(holder_lock, false);
    } else if (State == EContainerState::Meta && !WaitTask.Pid && !Isolate) {
        /* meta container */
    } else if (!WaitTask.Exists()) {
        if (State != EContainerState::Dead)
            L() << "Task no found" << std::endl;
        Reap(holder_lock, false);
    } else if (WaitTask.GetPPid() != getppid()) {
        L() << "Wrong ppid " << WaitTask.GetPPid() << " " << getppid() << std::endl;
        Reap(holder_lock, false);
    } else if (WaitTask.IsZombie()) {
        L() << "Task is zombie" << std::endl;
        Task.Pid = 0;
    } else if (FreezerSubsystem.TaskCgroup(WaitTask.Pid, taskCg)) {
        L() << "Cannot check freezer" << std::endl;
        Reap(holder_lock, false);
    } else if (taskCg != freezerCg) {
        L() << "Task in wrong freezer" << std::endl;
        WaitTask.Kill(SIGKILL);
        Task.Kill(SIGKILL);
        Reap(holder_lock, false);
    }

    std::vector<pid_t> tasks;
    error = freezerCg.GetTasks(tasks);
    if (error)
        L_WRN() << "Cannot dump cgroups " << freezerCg << " " << error << std::endl;

    for (pid_t pid: tasks) {
        for (auto hy: Hierarchies) {
            TCgroup currentCg, correctCg = GetCgroup(*hy);
            error = hy->TaskCgroup(pid, currentCg);
            if (error || currentCg == correctCg)
                continue;

            /* Recheck freezer cgroup */
            TCgroup currentFr;
            error = FreezerSubsystem.TaskCgroup(pid, currentFr);
            if (error || currentFr != freezerCg)
                continue;

            L_WRN() << "Task " << pid << " in " << currentCg
                    << " while should be in " << correctCg << std::endl;
            (void)correctCg.Attach(pid);
        }
    }
}

TError TContainer::Restore(TScopedLock &holder_lock, const TKeyValue &node) {
    TError error;

    L_ACT() << "Restore container " << GetName() << std::endl;

    SystemClient.StartRequest();

    error = Load(node);
    if (error)
        goto out;

    SyncState(holder_lock);

    if (Task.Pid) {
        error = RestoreNetwork();
        if (error && !WaitTask.IsZombie()) {
            L_WRN() << "Cannot restore network: " << error << std::endl;
            goto out;
        }
    }

    if (MayRespawn())
            ScheduleRespawn();

    if (Parent)
        Parent->AddChild(shared_from_this());

    error = ApplyDynamicProperties();

out:
    SystemClient.FinishRequest();

    if (!error)
        error = Save();

    return error;
}

TCgroup TContainer::GetCgroup(const TSubsystem &subsystem) const {
    if (IsRoot())
        return subsystem.RootCgroup();
    if (IsPortoRoot())
        return subsystem.Cgroup(PORTO_ROOT_CGROUP);
    return subsystem.Cgroup(std::string(PORTO_ROOT_CGROUP) + "/" + GetName());
}

bool TContainer::MayRespawn() {
    if (State != EContainerState::Dead)
        return false;

    if (!ToRespawn)
        return false;

    if (Parent->State != EContainerState::Running &&
        Parent->State != EContainerState::Meta) {

        /*FIXME: respawn for hierarchies is broken */

        return false;
    }

    return MaxRespawns < 0 || RespawnCount < (uint64_t)MaxRespawns;
}

bool TContainer::MayReceiveOom(int fd) {
    if (OomEvent.Fd != fd)
        return false;

    if (!Task.Pid)
        return false;

    if (State == EContainerState::Dead)
        return false;

    return true;
}

// Works only once
bool TContainer::HasOomReceived() {
    uint64_t val;

    return read(OomEvent.Fd, &val, sizeof(val)) == sizeof(val) && val != 0;
}

void TContainer::ScheduleRespawn() {
    TEvent e(EEventType::Respawn, shared_from_this());
    Holder->Queue->Add(config().container().respawn_delay_ms(), e);
}

TError TContainer::Respawn(TScopedLock &holder_lock) {
    TScopedAcquire acquire(shared_from_this());
    if (!acquire.IsAcquired())
        return TError(EError::Busy, "Can't respawn busy container");

    TError error = Stop(holder_lock, config().container().kill_timeout_ms());
    if (error)
        return error;


    SystemClient.StartRequest();
    error = Start(false);
    SystemClient.FinishRequest();
    RespawnCount++;
    SetProp(EProperty::RESPAWN_COUNT);

    if (error)
        return error;

    return Save();
}

bool TContainer::CanRemoveDead() const {
    return State == EContainerState::Dead &&
        DeathTime / 1000 +
        AgingTime <= GetCurrentTimeMs() / 1000;
}

std::vector<std::string> TContainer::GetChildren() {
    std::vector<std::string> vec;

    for (auto weakChild : Children)
        if (auto child = weakChild.lock())
            vec.push_back(child->GetName());

    return vec;
}

void TContainer::DeliverEvent(TScopedLock &holder_lock, const TEvent &event) {
    TError error;

    switch (event.Type) {
        case EEventType::Exit:
            {
                uint64_t failcnt = 0lu;
                auto cg = GetCgroup(MemorySubsystem);
                error = MemorySubsystem.GetFailCnt(cg, failcnt);
                if (error)
                    L_WRN() << "Can't get container memory.failcnt" << std::endl;

                Exit(holder_lock, event.Exit.Status,
                     FdHasEvent(OomEvent.Fd) || failcnt);
            }
            break;
        case EEventType::RotateLogs:
            if (State == EContainerState::Running) {
                Stdout.Rotate(*this);
                Stderr.Rotate(*this);
            }
            break;
        case EEventType::Respawn:
            error = Respawn(holder_lock);
            if (error)
                L_WRN() << "Can't respawn container: " << error << std::endl;
            else
                L() << "Respawned " << GetName() << std::endl;
            break;
        case EEventType::OOM:
            Exit(holder_lock, SIGKILL, true);
            break;
        default:
            break;
    }
}

std::string TContainer::GetPortoNamespace() const {
    if (Parent)
        return Parent->GetPortoNamespace() + NsName;
    else
        return "";
}

void TContainer::AddWaiter(std::shared_ptr<TContainerWaiter> waiter) {
    CleanupWaiters();
    Waiters.push_back(waiter);
}

void TContainer::NotifyWaiters() {
    CleanupWaiters();
    for (auto &w : Waiters) {
        auto waiter = w.lock();
        if (waiter)
            waiter->WakeupWaiter(this);
    }
    if (!IsRoot() && !IsPortoRoot())
        TContainerWaiter::WakeupWildcard(this);
}

void TContainer::CleanupWaiters() {
    for (auto iter = Waiters.begin(); iter != Waiters.end();) {
        if (iter->expired()) {
            iter = Waiters.erase(iter);
            continue;
        }
        iter++;
    }
}

TError TContainer::UpdateTrafficClasses() {
    uint32_t handle, parent;
    TError error;

    handle = GetTrafficClass();
    parent = TcHandle(ROOT_TC_MAJOR, ROOT_TC_MINOR);

    /* link class to closest meta container */
    for (auto p = Parent; p; p = p->Parent) {
        if (p->State == EContainerState::Meta) {
            parent = p->GetTrafficClass();
            break;
        }
        if (p->State == EContainerState::Stopped)
            return TError::Success();
    }

    auto net_lock = HostNetwork->ScopedLock();
    error = HostNetwork->CreateTC(handle, parent, !IsMeta(),
                                  NetPriority, NetGuarantee, NetLimit);
    if (error)
        return error;
    net_lock.unlock();

    if (Net && Net != HostNetwork) {
        parent = TcHandle(ROOT_TC_MAJOR, PORTO_ROOT_CONTAINER_ID);
        for (auto p = Parent; p; p = p->Parent) {
            if (p->State == EContainerState::Meta && p->Net == Net) {
                parent = p->GetTrafficClass();
                break;
            }
        }
        auto net_lock = Net->ScopedLock();
        error = Net->CreateTC(handle, parent, !IsMeta(),
                              NetPriority, NetGuarantee, NetLimit);
    }

    return error;
}

TContainerWaiter::TContainerWaiter(std::shared_ptr<TClient> client,
                                   std::function<void (std::shared_ptr<TClient>,
                                                       TError, std::string)> callback) :
    Client(client), Callback(callback) {
}

void TContainerWaiter::WakeupWaiter(const TContainer *who, bool wildcard) {
    std::shared_ptr<TClient> client = Client.lock();
    if (client) {
        std::string name;
        TError err;
        if (who)
            err = client->ComposeRelativeName(who->GetName(), name);
        if (wildcard && (err || !MatchWildcard(name)))
            return;
        Callback(client, err, name);
        Client.reset();
        client->Waiter = nullptr;
    }
}

std::mutex TContainerWaiter::WildcardLock;
std::list<std::weak_ptr<TContainerWaiter>> TContainerWaiter::WildcardWaiters;

void TContainerWaiter::WakeupWildcard(const TContainer *who) {
    WildcardLock.lock();
    for (auto &w : WildcardWaiters) {
        auto waiter = w.lock();
        if (waiter)
            waiter->WakeupWaiter(who, true);
    }
    WildcardLock.unlock();
}

void TContainerWaiter::AddWildcard(std::shared_ptr<TContainerWaiter> &waiter) {
    WildcardLock.lock();
    for (auto iter = WildcardWaiters.begin(); iter != WildcardWaiters.end();) {
        if (iter->expired()) {
            iter = WildcardWaiters.erase(iter);
            continue;
        }
        iter++;
    }
    WildcardWaiters.push_back(waiter);
    WildcardLock.unlock();
}

bool TContainerWaiter::MatchWildcard(const std::string &name) {
    for (const auto &wildcard: Wildcards)
        if (StringMatch(name, wildcard))
            return true;
    return false;
}
