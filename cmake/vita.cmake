# PS Vita configuration, for use with $VITASDK/share/vita.toolchain.cmake.

set(VITASDK "$ENV{VITASDK}")

set(VITA_INCLUDE_DIRS
    "${VITASDK}/include"
    "${VITASDK}/include/psp2"
    "${VITASDK}/arm-vita-eabi/include"
)

set(VITA_LIBRARIES c)

# SDL3 for Vita links against these stubs.
list(APPEND VITA_LIBRARIES SDL3)
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

link_directories("${VITASDK}/arm-vita-eabi/lib")

list(APPEND VITA_LIBRARIES m pthread)

list(APPEND VITA_LIBRARIES debugnet)

list(APPEND CMAKE_PREFIX_PATH "${VITASDK}/arm-vita-eabi")
find_package(SDL3 REQUIRED)
list(APPEND VITA_LIBRARIES SDL3::SDL3)

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

set(VITA_SOURCES src/platform_vita.c)
