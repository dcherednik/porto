#ifndef __CONTAINER_H__
#define __CONTAINER_H__

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>
#include <set>

#include "kvalue.hpp"
#include "property.hpp"
#include "task.hpp"

class TCgroup;
class TContainerEnv;
struct TData;
class TContainer;
class TSubsystem;

enum class EContainerState {
    Stopped,
    Dead,
    Running,
    Paused
};

struct TDataSpec {
    std::string Description;
    bool RootValid;
    std::function<std::string(TContainer& c)> Handler;
    std::set<EContainerState> Valid;
};

extern std::map<std::string, const TDataSpec> dataSpec;

class TContainer {
    const std::string Name;
    const std::shared_ptr<TContainer> Parent;
    EContainerState State;
    TContainerSpec Spec;
    bool MaybeReturnedOk = false;
    size_t TimeOfDeath;
    int Ref = 0;
    friend TData;

    std::map<std::shared_ptr<TSubsystem>, std::shared_ptr<TCgroup>> LeafCgroups;
    std::unique_ptr<TTask> Task;

    // data
    bool CheckState(EContainerState expected);
    TError PrepareCgroups();
    TError PrepareTask();
    TError KillAll();

    TContainer(const TContainer &) = delete;
    TContainer &operator=(const TContainer &) = delete;

public:
    TContainer(const std::string &name, std::shared_ptr<TContainer> parent) :
        Name(name), Parent(parent), State(EContainerState::Stopped), Spec(name) {
            if (Parent)
                Parent->Ref++;
        }
    ~TContainer();

    const std::string &GetName() const;

    bool IsRoot() const;

    std::vector<pid_t> Processes();
    bool IsAlive();

    TError Create();
    TError Start();
    TError Stop();
    TError Pause();
    TError Resume();

    TError GetProperty(const std::string &property, std::string &value) const;
    TError SetProperty(const std::string &property, const std::string &value);

    TError GetData(const std::string &data, std::string &value);
    TError Restore(const kv::TNode &node);

    bool DeliverExitStatus(int pid, int status);

    std::shared_ptr<TCgroup> GetLeafCgroup(std::shared_ptr<TSubsystem> subsys);
    void Heartbeat();
    bool CanRemoveDead() const;
    bool HasChildren() const;
};

class TContainerHolder {
    std::map <std::string, std::shared_ptr<TContainer>> Containers;

    bool ValidName(const std::string &name) const;
public:
    ~TContainerHolder();
    std::shared_ptr<TContainer> GetParent(const std::string &name) const;
    TError CreateRoot();
    TError Create(const std::string &name);
    std::shared_ptr<TContainer> Get(const std::string &name);
    TError Restore(const std::string &name, const kv::TNode &node);

    TError Destroy(const std::string &name);
    bool DeliverExitStatus(int pid, int status);

    std::vector<std::string> List() const;
    void Heartbeat();
};

#endif
