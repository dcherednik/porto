project(util)

add_library(util STATIC error.cpp locks.cpp namespace.cpp netlink.cpp log.cpp loop.cpp path.cpp signal.cpp unix.cpp cred.cpp string.cpp crc32.cpp quota.cpp)
add_dependencies(util config rpc_proto)

if(NOT USE_SYSTEM_LIBNL)
add_dependencies(util libnl)
endif()
