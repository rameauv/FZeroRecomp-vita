# PS Vita cross-compilation toolchain file for VitaSDK
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/vita_toolchain.cmake ..

set(CMAKE_SYSTEM_NAME PlayStationVita)
set(CMAKE_SYSTEM_PROCESSOR arm)

# VitaSDK environment
if(DEFINED ENV{VITASDK})
    set(VITASDK "$ENV{VITASDK}")
else()
    set(VITASDK "/usr/local/vitasdk")
endif()
set(VITASDK "${VITASDK}" CACHE PATH "Path to Vita SDK root")

# Toolchain prefix
set(TOOLCHAIN_PREFIX "${VITASDK}/arm-vita-eabi/bin")

# Compiler toolchain
set(CMAKE_C_COMPILER   "${TOOLCHAIN_PREFIX}/arm-vita-eabi-gcc" CACHE PATH "C compiler")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PREFIX}/arm-vita-eabi-g++" CACHE PATH "C++ compiler")
set(CMAKE_ASM_COMPILER "${TOOLCHAIN_PREFIX}/arm-vita-eabi-gcc" CACHE PATH "assembler")

# Sysroot and find paths
set(CMAKE_SYSROOT "${VITASDK}")
set(CMAKE_FIND_ROOT_PATH "${VITASDK}")

# Architecture flags for ARM Cortex-A9
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=neon")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=neon")

# Vita-specific preprocessor definitions
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -D__vita__ -DVITA -DPSP2 -D_SCE_VITA_C_")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__vita__ -DVITA -DPSP2 -D_SCE_VITA_C_")

# Linker flags for Vita ELF
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--build-id=none")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")

set(CMAKE_EXECUTABLE_SUFFIX ".elf")

# Find path modes
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Default to Release for Vita builds
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
