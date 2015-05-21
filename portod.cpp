#include <vector>
#include <string>
#include <algorithm>
#include <csignal>
#include <sstream>
#include <iomanip>

#include "portod.hpp"
#include "rpc.hpp"
#include "cgroup.hpp"
#include "config.hpp"
#include "event.hpp"
#include "qdisc.hpp"
#include "context.hpp"
#include "client.hpp"
#include "epoll.hpp"
#include "util/log.hpp"
#include "util/file.hpp"
#include "util/folder.hpp"
#include "util/protobuf.hpp"
#include "util/signal.hpp"
#include "util/unix.hpp"
#include "util/string.hpp"
#include "util/crash.hpp"
#include "util/cred.hpp"
#include "batch.hpp"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <grp.h>
#define GNU_SOURCE
#include <sys/socket.h>
#include <sys/resource.h>
}

using std::string;
using std::map;
using std::vector;

static pid_t slavePid;
static bool stdlog = false;
static bool failsafe = false;
static bool noNetwork = false;

TStatistics *Statistics;
static void AllocStatistics() {
    Statistics = (TStatistics *)mmap(nullptr, sizeof(*Statistics),
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (!Statistics)
        throw std::bad_alloc();
}

static void DaemonOpenLog(bool master) {
    const auto &log = master ? config().master_log() : config().slave_log();

    TLogger::CloseLog();
    TLogger::OpenLog(stdlog, log.path(), log.perm());
}

static int DaemonSyncConfig(bool master) {
    config.Load();

    if (noNetwork)
        config().mutable_network()->set_enabled(false);
    TNl::EnableDebug(config().network().debug());

    const auto &pid = master ? config().master_pid() : config().slave_pid();

    DaemonOpenLog(master);

    if (CreatePidFile(pid.path(), pid.perm())) {
        L_ERR() << "Can't create pid file " << pid.path() << "!" << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int DaemonPrepare(bool master) {
    const string procName = master ? "portod" : "portod-slave";

    SetProcessName(procName.c_str());

    int ret = DaemonSyncConfig(master);
    if (ret)
        return ret;

    L_SYS() << string(80, '-') << std::endl;
    L_SYS() << "Started " << GIT_TAG << " " << GIT_REVISION << " " << GetPid() << std::endl;
    L_SYS() << config().DebugString() << std::endl;

    return EXIT_SUCCESS;
}

static void DaemonShutdown(bool master, int ret) {
    const auto &pid = master ? config().master_pid() : config().slave_pid();

    L_SYS() << "Stopped " << ret << std::endl;

    TLogger::CloseLog();
    RemovePidFile(pid.path());

    if (ret < 0)
        RaiseSignal(-ret);

    if (master) {
        TFile f(config().daemon().pidmap().path());
        (void)f.Remove();
    }
}

static void RemoveRpcServer(const string &path) {
    TFile f(path);
    TError error = f.Remove();
    if (error)
        L_ERR() << "Can't remove socket file: " << error << std::endl;
}

static bool HandleRequest(TContext &context, std::shared_ptr<TClient> client) {
    uint32_t slaveReadTimeout = config().daemon().slave_read_timeout_s();
    InterruptibleInputStream pist(client->GetFd());

    rpc::TContainerRequest request;

    if (slaveReadTimeout)
        (void)alarm(slaveReadTimeout);

    bool haveData = ReadDelimitedFrom(&pist, &request);

    if (slaveReadTimeout)
        (void)alarm(0);

    if (pist.Interrupted()) {
        uint8_t *buf;
        size_t pos;
        pist.GetBuf(&buf, &pos);

        std::stringstream ss;
        ss << std::setfill('0') << std::hex;
        for (size_t i = 0; i < pos; i++)
            ss << std::setw(2) << (int)buf[i];

        L_WRN() << "Interrupted read from " << client->GetFd()
                << ", partial message: " << ss.str() << std:: endl;
        Statistics->InterruptedReads++;
        return true;
    }

    if (pist.GetLeftovers())
        L_WRN() << "Message is greater that expected from " << client->GetFd()
                << ", skipped " << pist.GetLeftovers() << std:: endl;

    if (!haveData)
        return true;

    if (client->Identify(*context.Cholder, false))
        return true;

    HandleRpcRequest(context, request, client);

    return false;
}

static int AcceptClient(TContext &context, int sfd,
                        std::map<int, std::shared_ptr<TClient>> &clients, int &fd) {
    int cfd;
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;

    peer_addr_size = sizeof(struct sockaddr_un);
    cfd = accept4(sfd, (struct sockaddr *) &peer_addr,
                  &peer_addr_size, SOCK_CLOEXEC);
    if (cfd < 0) {
        if (errno == EAGAIN)
            return 0;

        L_ERR() << "accept() error: " << strerror(errno) << std::endl;
        return -1;
    }

    auto client = std::make_shared<TClient>(cfd);
    int ret = client->Identify(*context.Cholder);
    if (ret)
        return ret;

    fd = cfd;
    clients[cfd] = client;
    return 0;
}

static void RemoveClient(int cfd, std::map<int, std::shared_ptr<TClient>> &clients) {
    close(cfd);
    clients.erase(cfd);
}

static bool AnotherInstanceRunning(const string &path) {
    int fd;
    if (ConnectToRpcServer(path, fd))
        return false;

    close(fd);
    return true;
}

void AckExitStatus(int pid) {
    if (!pid)
        return;

    int ret = write(REAP_ACK_FD, &pid, sizeof(pid));
    if (ret == sizeof(pid)) {
        L() << "Acknowledge exit status for " << std::to_string(pid) << std::endl;
    } else {
        TError error(EError::Unknown, errno, "write(): returned " + std::to_string(ret));
        if (error)
            L_ERR() << "Can't acknowledge exit status for " << pid << ": " << error << std::endl;
        if (ret < 0)
            Crash();
    }
}

static int ReapSpawner(int fd, TContainerHolder &cholder) {
    struct pollfd fds[1];
    int nr = 1000;

    fds[0].fd = fd;
    fds[0].events = POLLIN | POLLHUP;

    while (nr--) {
        int ret = poll(fds, 1, 0);
        if (ret < 0) {
            L_ERR() << "poll() error: " << strerror(errno) << std::endl;
            return ret;
        }

        if (!fds[0].revents || (fds[0].revents & POLLHUP))
            return 0;

        int pid, status;
        if (read(fd, &pid, sizeof(pid)) < 0) {
            L_ERR() << "read(pid): " << strerror(errno) << std::endl;
            return 0;
        }
retry:
        if (read(fd, &status, sizeof(status)) < 0) {
            if (errno == EAGAIN)
                goto retry;
            L_ERR() << "read(status): " << strerror(errno) << std::endl;
            return 0;
        }

        TEvent e(EEventType::Exit);
        e.Exit.Pid = pid;
        e.Exit.Status = status;
        (void)cholder.DeliverEvent(e);
        AckExitStatus(pid);
    }

    return 0;
}

static inline int EncodeSignal(int sig) {
    return -sig;
}

static int SlaveRpc(TContext &context) {
    int ret = 0;
    int sfd;
    std::map<int, std::shared_ptr<TClient>> clients;
    TContainerHolder &cholder = *context.Cholder;

    TCred cred(getuid(), getgid());

    TGroup g(config().rpc_sock().group().c_str());
    TError error = g.Load();
    if (error)
        L_ERR() << "Can't get gid for " << config().rpc_sock().group() << ": " << error << std::endl;

    if (!error)
        cred.Gid = g.GetId();

    error = CreateRpcServer(config().rpc_sock().file().path(),
                            config().rpc_sock().file().perm(),
                            cred, sfd);
    if (error) {
        L_ERR() << "Can't create RPC server: " << error.GetMsg() << std::endl;
        return EXIT_FAILURE;
    }

    error = context.EpollLoop->AddFd(sfd);
    if (error) {
        L_ERR() << "Can't add RPC server fd to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

    error = context.EpollLoop->AddFd(REAP_EVT_FD);
    if (error && !failsafe) {
        L_ERR() << "Can't add master fd to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

    if (context.NetEvt) {
        error = context.EpollLoop->AddFd(context.NetEvt->GetFd());
        if (error) {
            L_ERR() << "Can't add netlink events fd to epoll: " << error << std::endl;
            return EXIT_FAILURE;
        }
    }

    std::vector<int> signals;
    std::vector<struct epoll_event> events;

    while (true) {
        int timeout = context.Queue->GetNextTimeout();
        Statistics->SlaveTimeoutMs = timeout;

        error = context.EpollLoop->GetEvents(signals, events, timeout);
        if (error) {
            L_ERR() << "slave: epoll error " << error << std::endl;
            ret = EXIT_FAILURE;
            goto exit;
        }

        context.Queue->DeliverEvents(*context.Cholder);

        for (auto s : signals) {
            switch (s) {
            case SIGINT:
                context.Destroy();
                // no break here
            case SIGTERM:
                ret = EncodeSignal(s);
                goto exit;
            case updateSignal:
                L_EVT() << "Updating" << std::endl;
                ret = EncodeSignal(s);
                goto exit;
            case rotateSignal:
                DaemonOpenLog(false);
                break;
            case SIGCHLD:
                int status;
                pid_t pid;

                while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                    if (WIFEXITED(status)) {
                        if (context.Posthooks.find(pid) != context.Posthooks.end()) {
                            int fd = context.PosthooksError.at(pid);
                            TError error;
                            if (!TError::Deserialize(fd, error))
                                error = TError(EError::Unknown, "Didn't get any result from batch task");
                            close(fd);
                            context.Posthooks[pid](error);
                            context.Posthooks.erase(pid);
                            context.PosthooksError.erase(pid);
                        }
                    } else {
                        L_ERR() << "Batch task died on signal " << WTERMSIG(status) << std::endl;
                    }
                }
                break;
            default:
                /* Ignore other signals */
                break;
            }
        }

        if (!failsafe) {
            ret = ReapSpawner(REAP_EVT_FD, *context.Cholder);
            if (ret)
                goto exit;
        }

        for (auto ev : events) {
            if (ev.data.fd == sfd) {
                if (clients.size() > config().daemon().max_clients()) {
                    L_WRN() << "Skip connection attempt" << std::endl;
                    continue;
                }

                int fd = -1;
                ret = AcceptClient(context, sfd, clients, fd);
                if (ret < 0)
                    goto exit;

                error = context.EpollLoop->AddFd(fd);
                if (error) {
                    L_ERR() << "Can't add client fd to epoll: " << error << std::endl;
                    ret = EXIT_FAILURE;
                    goto exit;
                }
            } else if (ev.data.fd == REAP_EVT_FD) {
                // we handled all events from the master before events
                // from the clients (so clients see updated view of the
                // world as soon as possible)
                continue;
            } else if (context.NetEvt && context.NetEvt->GetFd() == ev.data.fd) {
                L() << "Refresh list of available network interfaces" << std::endl;
                context.NetEvt->FlushEvents();

                TError error = context.Net->Update();
                if (error)
                    L_ERR() << "Can't refresh list of network interfaces: " << error << std::endl;
            } else if (clients.find(ev.data.fd) != clients.end()) {
                auto client = clients[ev.data.fd];
                bool needClose = false;

                if (ev.events & EPOLLIN)
                    needClose = HandleRequest(context, client);

                if ((ev.events & EPOLLHUP) || needClose)
                    RemoveClient(ev.data.fd, clients);

            } else {
                TEvent e(EEventType::OOM);
                e.OOM.Fd = ev.data.fd;
                (void)cholder.DeliverEvent(e);
            }
        }
    }

exit:
    for (auto pair : clients)
        close(pair.first);

    close(sfd);

    return ret;
}

static void KvDump() {
    TKeyValueStorage containers(TMount("tmpfs", config().keyval().file().path(), "tmpfs", { config().keyval().size() }));
    TError error = containers.MountTmpfs();
    if (error)
        L_ERR() << "Can't mount containers key-value storage: " << error << std::endl;
    else
        containers.Dump();

    TKeyValueStorage volumes(TMount("tmpfs", config().volumes().keyval().file().path(), "tmpfs", { config().volumes().keyval().size() }));
    error = volumes.MountTmpfs();
    if (error)
        L_ERR() << "Can't mount volumes key-value storage: " << error << std::endl;
    else
        volumes.Dump();
}

static int TuneLimits() {
    struct rlimit rlim;

    // we need FD for each container to monitor OOM event, plus some spare ones
    int maxFd = config().container().max_total() + 100;

    rlim.rlim_max = maxFd;
    rlim.rlim_cur = maxFd;

    int ret = setrlimit(RLIMIT_NOFILE, &rlim);
    if (ret)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

static int SlaveMain() {
    SetDieOnParentExit(SIGTERM);

    if (failsafe)
        AllocStatistics();

    Statistics->SlaveStarted = GetCurrentTimeMs();

    int ret = DaemonPrepare(false);
    if (ret)
        return ret;

    ret = TuneLimits();
    if (ret) {
        L_ERR() << "Can't set correct limits: " << strerror(errno) << std::endl;
        return ret;
    }

    if (config().network().enabled()) {
        if (system("modprobe cls_cgroup")) {
            L_ERR() << "Can't load cls_cgroup kernel module: " << strerror(errno) << std::endl;
            if (!failsafe)
                return EXIT_FAILURE;

            config().mutable_network()->set_enabled(false);
        }
    }

    if (fcntl(REAP_EVT_FD, F_SETFD, FD_CLOEXEC) < 0) {
        L_ERR() << "Can't set close-on-exec flag on REAP_EVT_FD: " << strerror(errno) << std::endl;
        if (!failsafe)
            return EXIT_FAILURE;
    }

    if (fcntl(REAP_ACK_FD, F_SETFD, FD_CLOEXEC) < 0) {
        L_ERR() << "Can't set close-on-exec flag on REAP_ACK_FD: " << strerror(errno) << std::endl;
        if (!failsafe)
            return EXIT_FAILURE;
    }

    umask(0);

    TError error = SetOomScoreAdj(0);
    if (error)
        L_ERR() << "Can't adjust OOM score: " << error << std::endl;

    TContext context;
    try {
        TCgroupSnapshot cs;
        error = cs.Create();
        if (error)
            L_ERR() << "Can't create cgroup snapshot: " << error << std::endl;

        error = context.Initialize();
        if (error) {
            L_ERR() << "Initialization error: " << error << std::endl;
            return EXIT_FAILURE;
        }

        bool restored = context.Cholder->RestoreFromStorage();
        context.Vholder->RestoreFromStorage();

        L() << "Done restoring" << std::endl;

        cs.Destroy();

        if (!restored) {
            L() << "Remove container leftovers from previous run..." << std::endl;
            RemoveIf(config().container().tmp_dir(),
                     EFileType::Directory,
                     [](const std::string &name, const TPath &path) {
                        return name != TPath(config().volumes().resource_dir()).BaseName() &&
                               name != TPath(config().volumes().volume_dir()).BaseName();
                     });
        }

        ret = SlaveRpc(context);
        L_SYS() << "Shutting down..." << std::endl;

        RemoveRpcServer(config().rpc_sock().file().path());
    } catch (string s) {
        std::cerr << s << std::endl;
        ret = EXIT_FAILURE;
    } catch (const char *s) {
        std::cerr << s << std::endl;
        ret = EXIT_FAILURE;
    } catch (const std::exception &exc) {
        std::cerr << exc.what() << std::endl;
        ret = EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Uncaught exception!" << std::endl;
        ret = EXIT_FAILURE;
    }

    DaemonShutdown(false, ret);
    context.Destroy();

    return ret;
}

static void DeliverPidStatus(int fd, int pid, int status, size_t queued) {
    L_EVT() << "Deliver " << pid << " status " << status << " (" << queued << " queued)" << std::endl;

    if (write(fd, &pid, sizeof(pid)) < 0)
        L_ERR() << "write(pid): " << strerror(errno) << std::endl;
    if (write(fd, &status, sizeof(status)) < 0)
        L_ERR() << "write(status): " << strerror(errno) << std::endl;
}

static void Reap(int pid) {
    (void)waitpid(pid, NULL, 0);
}

static int ReapDead(int fd, map<int,int> &exited, int slavePid, int &slaveStatus, std::set<int> &acked) {
    while (true) {
        siginfo_t info = { 0 };
        if (waitid(P_ALL, -1, &info, WNOHANG | WNOWAIT | WEXITED) < 0)
            break;

        if (info.si_pid <= 0)
            break;

        int status = 0;
        if (info.si_code == CLD_KILLED) {
            status = info.si_status;
        } else if (info.si_code == CLD_DUMPED) {
            status = info.si_status | (1 << 7);
        } else { // CLD_EXITED
            status = info.si_status << 8;
        }

        if (info.si_pid == slavePid) {
            slaveStatus = status;
            Reap(info.si_pid);
            return -1;
        }

        if (acked.find(info.si_pid) != acked.end()) {
            acked.erase(info.si_pid);
            Reap(info.si_pid);
            continue;
        }

        if (exited.find(info.si_pid) != exited.end())
            return 0;

        exited[info.si_pid] = status;
        DeliverPidStatus(fd, info.si_pid, status, exited.size());
        Statistics->QueuedStatuses = exited.size();
    }

    return 0;
}

static int ReceiveAcks(int fd, std::map<int,int> &exited,
                       std::set<int> &acked) {
    int pid;
    int nr = 0;

    if (read(fd, &pid, sizeof(pid)) == sizeof(pid)) {
        if (pid <= 0)
            return nr;

        L_EVT() << "Got acknowledge for " << pid << " (" << exited.size() << " queued)" << std::endl;

        if (exited.find(pid) == exited.end()) {
            acked.insert(pid);
        } else {
            exited.erase(pid);
            Reap(pid);
        }

        Statistics->QueuedStatuses = exited.size();
        nr++;
    }

    return nr;
}

static void SaveStatuses(map<int, int> &exited) {
    TFile f(config().daemon().pidmap().path());
    if (f.Exists()) {
        TError error = f.Remove();
        if (error) {
            L_ERR() << "Can't save pid map: " << error << std::endl;
            return;
        }
    }

    for (auto &kv : exited) {
        TError error = f.AppendString(std::to_string(kv.first) + " " + std::to_string(kv.second) + "\n");
        if (error)
            L_ERR() << "Can't save pid map: " << error << std::endl;
    }
}

static void RestoreStatuses(map<int, int> &exited) {
    TFile f(config().daemon().pidmap().path());
    if (!f.Exists())
        return;

    vector<string> lines;
    TError error = f.AsLines(lines);
    if (error) {
        L_ERR() << "Can't restore pid map: " << error << std::endl;
        return;
    }

    for (auto &line : lines) {
        vector<string> tokens;
        error = SplitString(line, ' ', tokens);
        if (error) {
            L_ERR() << "Can't restore pid map: " << error << std::endl;
            continue;
        }

        if (tokens.size() != 2) {
            continue;
        }

        int pid, status;

        error = StringToInt(tokens[0], pid);
        if (error) {
            L_ERR() << "Can't restore pid map: " << error << std::endl;
            continue;
        }

        error = StringToInt(tokens[0], status);
        if (error) {
            L_ERR() << "Can't restore pid map: " << error << std::endl;
            continue;
        }

        exited[pid] = status;
    }
}

static int SpawnSlave(TEpollLoop &loop, map<int,int> &exited) {
    int evtfd[2];
    int ackfd[2];
    int ret = EXIT_FAILURE;
    TError error;

    slavePid = 0;

    if (pipe2(evtfd, O_NONBLOCK) < 0) {
        L_ERR() << "pipe(): " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    if (pipe2(ackfd, O_NONBLOCK) < 0) {
        L_ERR() << "pipe(): " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    slavePid = fork();
    if (slavePid < 0) {
        L_ERR() << "fork(): " << strerror(errno) << std::endl;
        ret = EXIT_FAILURE;
        goto exit;
    } else if (slavePid == 0) {
        close(evtfd[1]);
        close(ackfd[0]);
        TLogger::CloseLog();
        loop.Destroy();
        dup2(evtfd[0], REAP_EVT_FD);
        dup2(ackfd[1], REAP_ACK_FD);
        close(evtfd[0]);
        close(ackfd[1]);

        exit(SlaveMain());
    }

    close(evtfd[0]);
    close(ackfd[1]);

    L_SYS() << "Spawned slave " << slavePid << std::endl;
    Statistics->Spawned++;

    for (auto &pair : exited)
        DeliverPidStatus(evtfd[1], pair.first, pair.second, exited.size());

    error = loop.AddFd(ackfd[0]);
    if (error) {
        L_ERR() << "Can't add ackfd[0] to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

    while (true) {
        std::vector<int> signals;
        std::vector<struct epoll_event> events;

        error = loop.GetEvents(signals, events, -1);
        if (error) {
            L_ERR() << "master: epoll error " << error << std::endl;
            return EXIT_FAILURE;
        }

        for (auto s : signals) {
            switch (s) {
            case SIGINT:
            case SIGTERM:
                if (kill(slavePid, s) < 0)
                    L_ERR() << "Can't send " << s << " to slave" << std::endl;

                L() << "Waiting for slave to exit..." << std::endl;
                (void)RetryFailed(10, 50,
                [&]() { return waitpid(slavePid, nullptr, WNOHANG) != slavePid; });

                ret = EncodeSignal(s);
                goto exit;
            case updateSignal:
            {
                int ret = DaemonSyncConfig(true);
                if (ret)
                    return ret;

                L_SYS() << "Updating" << std::endl;

                const char *stdlogArg = nullptr;
                if (stdlog)
                    stdlogArg = "--stdlog";

                SaveStatuses(exited);

                if (kill(slavePid, updateSignal) < 0) {
                    L_ERR() << "Can't send " << updateSignal << " to slave: " << strerror(errno) << std::endl;
                } else {
                    if (waitpid(slavePid, NULL, 0) != slavePid)
                        L_ERR() << "Can't wait for slave exit status: " << strerror(errno) << std::endl;
                }
                TLogger::CloseLog();
                close(evtfd[1]);
                close(ackfd[0]);
                loop.Destroy();
                execlp(program_invocation_name, program_invocation_name, stdlogArg, nullptr);
                std::cerr << "Can't execlp(" << program_invocation_name << ", " << program_invocation_name << ", NULL)" << strerror(errno) << std::endl;
                ret = EXIT_FAILURE;
                goto exit;
                break;
            }
            case rotateSignal:
                DaemonOpenLog(true);
                break;
            default:
                /* Ignore other signals */
                break;
            }
        }

        std::set<int> acked;
        for (auto ev : events) {
            if (ev.data.fd == ackfd[0]) {
                if (!ReceiveAcks(ackfd[0], exited, acked)) {
                    ret = EXIT_FAILURE;
                    goto exit;
                }
            } else {
                L_WRN() << "master received unknown epoll event: " << ev.data.fd << std::endl;
                loop.RemoveFd(ev.data.fd);
            }
        }

        int status;
        if (ReapDead(evtfd[1], exited, slavePid, status, acked)) {
            L_SYS() << "Slave exited with " << status << std::endl;
            ret = EXIT_SUCCESS;
            goto exit;
        }
    }

exit:
    close(evtfd[0]);
    close(evtfd[1]);

    close(ackfd[0]);
    close(ackfd[1]);

    return ret;
}

void CheckVersion(int &prevMaj, int &prevMin) {
    std::string prevVer;

    prevMaj = 0;
    prevMin = 0;

    TFile f(config().version().path(), config().version().perm());

    TError error = f.AsString(prevVer);
    if (!error)
        (void)sscanf(prevVer.c_str(), "v%d.%d", &prevMaj, &prevMin);

    error = f.WriteStringNoAppend(GIT_TAG);
    if (error)
        L_ERR() << "Can't update current version" << std::endl;
}

static int MasterMain() {
    AllocStatistics();
    Statistics->MasterStarted = GetCurrentTimeMs();

    int ret = DaemonPrepare(true);
    if (ret)
        return ret;

    int prevMaj, prevMin;
    CheckVersion(prevMaj, prevMin);
    L_SYS() << "Updating from previous version v" << prevMaj << "." << prevMin << std::endl;

    TEpollLoop ELoop;
    TError error = ELoop.Create();
    if (error)
        return error;

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        TError error(EError::Unknown, errno, "prctl(PR_SET_CHILD_SUBREAPER,)");
        L_ERR() << "Can't set myself as a subreaper: " << error << std::endl;
        return EXIT_FAILURE;
    }

    error = SetOomScoreAdj(-1000);
    if (error)
        L_ERR() << "Can't adjust OOM score: " << error << std::endl;

    map<int,int> exited;
    RestoreStatuses(exited);

    while (true) {
        size_t started = GetCurrentTimeMs();
        size_t next = started + config().container().respawn_delay_ms();
        ret = SpawnSlave(ELoop, exited);
        L() << "Returned " << ret << std::endl;
        if (next >= GetCurrentTimeMs())
            usleep((next - GetCurrentTimeMs()) * 1000);

        if (slavePid) {
            (void)kill(slavePid, SIGKILL);
            Reap(slavePid);
        }
        if (ret < 0)
            break;
    }

    DaemonShutdown(true, ret);

    return ret;
}

int main(int argc, char * const argv[]) {
    bool slaveMode = false;
    int argn;

    if (getuid() != 0) {
        std::cerr << "Need root privileges to start" << std::endl;
        return EXIT_FAILURE;
    }

    config.Load();

    for (argn = 1; argn < argc; argn++) {
        string arg(argv[argn]);

        if (arg == "-v" || arg == "--version") {
            std::cout << GIT_TAG << " " << GIT_REVISION <<std::endl;
            return EXIT_SUCCESS;
        } else if (arg == "--kv-dump") {
            KvDump();
            return EXIT_SUCCESS;
        } else if (arg == "--slave") {
            slaveMode = true;
        } else if (arg == "--stdlog") {
            stdlog = true;
        } else if (arg == "--failsafe") {
            failsafe = true;
        } else if (arg == "--nonet") {
            noNetwork = true;
        } else if (arg == "-t") {
            if (argn + 1 >= argc)
                return EXIT_FAILURE;
            return config.Test(argv[argn + 1]);
        } else {
            std::cerr << "Unknown option " << arg << std::endl;
            return EXIT_FAILURE;
        }
    }

    if (!slaveMode && AnotherInstanceRunning(config().rpc_sock().file().path())) {
        std::cerr << "Another instance of portod is running!" << std::endl;
        return EXIT_FAILURE;
    }

    if (slaveMode)
        return SlaveMain();
    else
        return MasterMain();
}
