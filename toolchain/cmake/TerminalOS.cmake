# CMake toolchain file for Terminal OS (x86_64-pc-terminalos, mlibc, static).
#   cmake -DCMAKE_TOOLCHAIN_FILE=/opt/terminalos/share/cmake/TerminalOS.cmake ...
set(CMAKE_SYSTEM_NAME TerminalOS)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
# Platform/TerminalOS.cmake (next to this file): a Unix with static libraries.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

set(CMAKE_C_COMPILER x86_64-pc-terminalos-gcc)
set(CMAKE_CXX_COMPILER x86_64-pc-terminalos-g++)
set(CMAKE_AR x86_64-pc-terminalos-ar CACHE FILEPATH "")
set(CMAKE_RANLIB x86_64-pc-terminalos-ranlib CACHE FILEPATH "")
set(CMAKE_STRIP x86_64-pc-terminalos-strip CACHE FILEPATH "")

set(CMAKE_SYSROOT /opt/terminalos/sysroot)
set(CMAKE_FIND_ROOT_PATH /opt/terminalos/sysroot /opt/terminalos/sysroot/usr)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "")
set(CMAKE_POSITION_INDEPENDENT_CODE OFF CACHE BOOL "")
