#pragma once

#include "util/error.hpp"

#define __STDC_LIMIT_MACROS
#include <cstdint>
#undef __STDC_LIMIT_MACROS

#define noinline __attribute__((noinline))

#define BIT(nr) (1ULL << (nr))

class TNonCopyable {
protected:
    TNonCopyable() = default;
    ~TNonCopyable() = default;
private:
    TNonCopyable(TNonCopyable const&) = delete;
    TNonCopyable& operator= (TNonCopyable const&) = delete;
    TNonCopyable(TNonCopyable const&&) = delete;
    TNonCopyable& operator= (TNonCopyable const&&) = delete;
};

enum class EAccessLevel {
    None,
    ReadOnly,
    ChildOnly,
    Normal,
    SuperUser,
    Internal,
};

constexpr int ROOT_TC_MAJOR = 1;
constexpr int ROOT_TC_MINOR = 0;
constexpr int DEFAULT_TC_MINOR = 2;
constexpr int DEFAULT_TC_MAJOR = 2;
constexpr int CONTAINER_TC_MINOR = 0;

constexpr uint64_t NET_DEFAULT_PRIO = 3;
constexpr uint64_t NET_MAX_RATE = 2000000000; /* 16Gbit */;

constexpr uint64_t ROOT_CONTAINER_ID = 1;
constexpr uint64_t PORTO_ROOT_CONTAINER_ID = 3;

constexpr const char *ROOT_CONTAINER = "/";
constexpr const char *DOT_CONTAINER = ".";
constexpr const char *SELF_CONTAINER = "self";
constexpr const char *PORTO_ROOT_CONTAINER = "/porto";

constexpr const char *PORTO_ROOT_CGROUP = "/porto";
constexpr const char *PORTO_DAEMON_CGROUP = "/portod";

constexpr const char *PORTO_GROUP_NAME = "porto";
constexpr const char *PORTO_CT_GROUP_NAME = "porto-containers";
constexpr const char *USER_CT_SUFFIX = "-containers";
constexpr const char *PORTO_SOCKET_PATH = "/run/portod.socket";
constexpr uint64_t PORTO_SOCKET_MODE = 0666;

constexpr int  REAP_EVT_FD = 128;
constexpr int  REAP_ACK_FD = 129;
constexpr int  PORTO_SK_FD = 130;

constexpr const char *PORTO_VERSION_FILE = "/run/portod.version";

constexpr uint64_t CONTAINER_NAME_MAX = 128;
constexpr uint64_t CONTAINER_PATH_MAX = 200;
constexpr uint64_t CONTAINER_ID_MAX = 16384;
constexpr uint64_t CONTAINER_LEVEL_MAX = 7;
constexpr uint64_t RUN_SUBDIR_LIMIT = 100u;

constexpr const char *PORTO_NAME_CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-@:.";

extern void AckExitStatus(int pid);

extern std::string PreviousVersion;
