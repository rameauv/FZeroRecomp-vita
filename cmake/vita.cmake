# PS Vita-specific CMake configuration
# This works with the VitaSDK toolchain ($VITASDK/share/vita.toolchain.cmake)

# VitaSDK should already be set by the toolchain
set(VITASDK "$ENV{VITASDK}")

# VitaSDK include directories
set(VITA_INCLUDE_DIRS
    "${VITASDK}/include"
    "${VITASDK}/include/psp2"
    "${VITASDK}/arm-vita-eabi/include"
    "${VITASDK}/arm-vita-eabi/include/SDL2"
)

# VitaSDK libraries - SDL2 and stub libraries
set(VITA_LIBRARIES c)

# Vita-specific stub libraries - SDL2 for Vita depends on these
list(APPEND VITA_LIBRARIES SDL2)
list(APPEND VITA_LIBRARIES
    SceSysmodule_stub
    SceDisplay_stub
    SceGxm_stub
    SceCtrl_stub
    SceIofilemgr_stub
    SceProcessmgr_stub
    ScePower_stub
    SceNet_stub
    SceNetCtl_stub
    SceAudio_stub
    SceAudioIn_stub
    SceMotion_stub
    SceTouch_stub
    SceCommonDialog_stub
    SceIme_stub
    SceHid_stub
)

# Add VitaSDK library directory to linker path
link_directories("${VITASDK}/arm-vita-eabi/lib")

# Add math and pthread at the end
list(APPEND VITA_LIBRARIES m pthread)

# Network logging library
list(APPEND VITA_LIBRARIES debugnet)

# SDL2 for Vita
list(APPEND CMAKE_PREFIX_PATH "${VITASDK}/arm-vita-eabi")
find_package(SDL2 REQUIRED)
list(APPEND VITA_INCLUDE_DIRS "${SDL2_INCLUDE_DIRS}")
list(APPEND VITA_LIBRARIES SDL2::SDL2)

# Compiler and linker flags
set(VITA_COMPILE_FLAGS
    -fdata-sections
    -ffunction-sections
    -fomit-frame-pointer
    -Wno-psabi
    -Wno-ignored-qualifiers
    -Wno-unused-command-line-argument
)

set(VITA_LINKER_FLAGS
    -L${VITASDK}/arm-vita-eabi/lib
    -Wl,--gc-sections
    -Wl,--as-needed
    -Wl,--wrap=getenv
)

# Platform-specific sources
set(VITA_SOURCES src/platform_vita.c)
