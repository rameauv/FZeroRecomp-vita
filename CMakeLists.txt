cmake_minimum_required(VERSION 3.6)
project(fzero_recomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(VITA ON CACHE BOOL "Build for PS Vita")

if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "Vita builds require VITASDK environment variable to be set!")
endif()

include("${VITASDK}/share/vita.cmake" REQUIRED)

include(cmake/vita.cmake)

set(SNESRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/snesrecomp" CACHE PATH
    "Path to the snesrecomp checkout used by this game")
include(${SNESRECOMP_ROOT}/runner/runner.cmake)

set(SNESRECOMP_SDL_BACKEND "SDL3" CACHE STRING "SDL backend for Vita")

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.c")

if(NOT GENERATED_SOURCES)
    message(FATAL_ERROR "No generated C found. Run generation steps with your F-Zero (USA) ROM.")
endif()

set(PLATFORM_SOURCES src/platform_vita.c)

set(INPUT_SOURCES src/input_vita.c)
set(AUDIO_SOURCES src/audio_sdl.c src/apu_worker.c)

add_executable(fzero_recomp
    ${SNESRECOMP_RUNNER_SOURCES}
    ${GENERATED_SOURCES}
    ${PLATFORM_SOURCES}
    ${INPUT_SOURCES}
    ${AUDIO_SOURCES}
    src/fzero_rtl.c
    src/fzero_layers.c
    src/fzero_vehicles.c
    src/fzero_ground.c
    src/fzero_spc_player.c
    src/main_vita.c
    src/fzero_capture_vita.c
    src/gxm_present.c
    src/fzero_gpu_ground.c
)

# Diagnostics. All OFF. Each one changes timing or the log, so build a normal
# configuration to measure frame rate.
#
# FZERO_DIAG: netlog probes (WRAM CRCs, stack write watch, off-rails latches,
# the interpreter step ring, APU-port probes). When off, every probe compiles
# out, including the 96 KB step ring written on every interpreted step.
option(FZERO_DIAG "Vita bring-up diagnostics (netlog probes)" OFF)
# Per-scanline rasteriser phase probes: 8 timer reads per line.
option(FZERO_PPUPROF "Per-scanline PPU phase probes" OFF)
# Startup microbenchmark of BuildWideLine's per-scanline Ppu copy.
option(FZERO_MEMBENCH "Startup memcpy microbenchmark (widescreen sizing)" OFF)
# Time FZeroLayersProcessLine's parts (mask/capture, BuildWideLine,
# MoveWideHud), and inside each the scratch copy, the extra ppu_runLine and the
# per-pixel loops, with the bytes each copy moves.
option(FZERO_WIDEPROF "Per-site widescreen layers probes" OFF)
# Startup benchmark: the wide pass's memory access pattern on core 0 alone, then
# with core 2 doing the same. Vita3K cannot answer this: it JITs to x86.
option(FZERO_COREBENCH "Two-core memory contention benchmark at startup" OFF)
# A front-touchscreen tap saves one frame's WRAM, PPU, wide scratch and picture
# to ux0:data/fzero_recomp/capture for replay on PC.
option(FZERO_CAPTURE "Touch-triggered one-frame capture for PC replay" OFF)
# Startup benchmark: the CPU read of one 142x224 buffer from each memory type a
# GXM render target can use.
option(FZERO_GPUBENCH "GPU readback benchmark at startup" OFF)
# Device oracle for the GPU ground lines: renders each GPU line a second time the
# normal way and compares the GPU output with it one frame later. Every 512th
# frame is a negative control. Not a timing build.
option(FZERO_GPU_GROUND_CHECK "Check the GPU ground lines against the CPU on device" OFF)
# Log to ux0:/data/fzero_recomp/log.txt instead of the netlog. Required for
# Vita3K, whose sceNet is a stub.
option(FZERO_LOG_FILE "Log to a file on ux0 instead of debugnet" OFF)

target_compile_definitions(fzero_recomp PRIVATE
    FZERO_DIAG=$<BOOL:${FZERO_DIAG}>
    FZERO_PPUPROF=$<BOOL:${FZERO_PPUPROF}>
    FZERO_MEMBENCH=$<BOOL:${FZERO_MEMBENCH}>
    FZERO_WIDEPROF=$<BOOL:${FZERO_WIDEPROF}>
    FZERO_COREBENCH=$<BOOL:${FZERO_COREBENCH}>
    FZERO_GPUBENCH=$<BOOL:${FZERO_GPUBENCH}>
    FZERO_GPU_GROUND_CHECK=$<BOOL:${FZERO_GPU_GROUND_CHECK}>
    # The additive sprite list and one render per line. fzero_layers.c keeps
    # different defaults for the desktop build.
    FZERO_WIDE_KEEPOAM_DEFAULT=1
    FZERO_WIDE_NONATIVE_DEFAULT=1
    FZERO_CAPTURE=$<BOOL:${FZERO_CAPTURE}>
    FZERO_LOG_FILE=$<BOOL:${FZERO_LOG_FILE}>
    SNESRECOMP_REVERSE_DEBUG=0
    RECOMP_UI_HOST_STB_WRITE=1
    VITA=1
    __vita__=1
    SNESRECOMP_SDL3=1
    SDL_MAIN_HANDLED
)

target_include_directories(fzero_recomp PRIVATE
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    ${SNESRECOMP_RUNNER_ROOT}/src/desktop
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VITA_INCLUDE_DIRS}
)

target_link_libraries(fzero_recomp PRIVATE ${VITA_LIBRARIES})
target_compile_options(fzero_recomp PRIVATE ${VITA_COMPILE_FLAGS})
target_link_options(fzero_recomp PRIVATE ${VITA_LINKER_FLAGS})

set(VITA_APP_NAME "F-Zero Recomp")
set(VITA_TITLEID "FZER00001")
set(VITA_VERSION "01.00")
vita_create_self(fzero_recomp.self fzero_recomp UNSAFE)
# LiveArea artwork (bubble icon, background, gate startup image), built by
# tools/make_livearea_art.py from assets/AppIcon.png and assets/boxart.jpg.
vita_create_vpk(fzero_recomp.vpk ${VITA_TITLEID} fzero_recomp.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
