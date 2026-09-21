cmake_minimum_required(VERSION 3.6)
project(fzero_recomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Vita-only build
set(VITA ON CACHE BOOL "Build for PS Vita")

# Ensure VITASDK is set
if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "Vita builds require VITASDK environment variable to be set!")
endif()

# Now include the VitaSDK configuration
include("${VITASDK}/share/vita.cmake" REQUIRED)

# Project-specific Vita configuration
include(cmake/vita.cmake)

set(SNESRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/snesrecomp" CACHE PATH
    "Path to the snesrecomp checkout used by this game")
include(${SNESRECOMP_ROOT}/runner/runner.cmake)

# Use SDL2 for Vita
set(SNESRECOMP_SDL_BACKEND "SDL2" CACHE STRING "SDL backend for Vita")

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.c")

if(NOT GENERATED_SOURCES)
    message(FATAL_ERROR "No generated C found. Run generation steps with your F-Zero (USA) ROM.")
endif()

# Platform sources - Vita only
set(PLATFORM_SOURCES src/platform_vita.c)

# Input and audio sources
set(INPUT_SOURCES src/input_vita.c)
set(AUDIO_SOURCES src/audio_sdl.c src/apu_worker.c)

# Main target for Vita
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
)

# Vita bring-up diagnostics (netlog probes: WRAM CRCs, stack write watch,
# off-rails/emulation-flip latches, the interpreter step ring, APU-port and
# paired-call bounce probes). OFF by default: when off every probe compiles
# out, including the 96KB interpreter step ring that is written on EVERY
# interpreted step, so this is a real frame-rate difference and not just log
# volume. Turn on with -DFZERO_DIAG=ON when chasing a divergence against the
# PC reference build.
option(FZERO_DIAG "Vita bring-up diagnostics (netlog probes)" OFF)
# Per-scanline rasteriser phase probes (8 timer reads per line, ~1.2 ms/frame).
# Turn ON only to re-measure a phase. OFF keeps the coarse cpu=/draw= split,
# which costs two reads per frame.
option(FZERO_PPUPROF "Per-scanline PPU phase probes" OFF)
# Alternate the compose palette-table arm every 300 frames. A Vita launch has
# no environment, so an A/B lever has to be built in; this is a MEASUREMENT
# build only -- pair it with -DFZERO_PPUPROF=ON and read consecutive blocks.
option(FZERO_PPU_LUT_AB "A/B the compose palette table every 300 frames" OFF)
# One-shot startup microbenchmark of BuildWideLine's per-scanline Ppu copy.
# Widescreen is unwired on Vita so the copy cannot be timed in situ; this
# measures the same access pattern directly. MEASUREMENT build only.
option(FZERO_MEMBENCH "Startup memcpy microbenchmark (widescreen sizing)" OFF)
# Wire the widescreen layers pipeline on Vita. Everything it needs already
# compiles into the build and runs under `if (g_layers)`; this allocates the
# layers and calls FZeroSetLayers, which nothing on Vita ever did. Expect it to
# be far below 60 FPS -- it exists to measure the cost, not to ship.
option(FZERO_WIDESCREEN "Enable the widescreen pass on Vita (measurement)" OFF)
# Split FZeroLayersProcessLine into mask/capture, BuildWideLine and MoveWideHud,
# and each of those into scratch copy / extra ppu_runLine / per-pixel loops, with
# the bytes each copy moves. Timed from outside, most of widescreen's Vita
# cost shows up as "everything else"; this opens it. Pair with
# -DFZERO_WIDESCREEN=ON. MEASUREMENT build only.
option(FZERO_WIDEPROF "Per-site widescreen layers probes" OFF)
# Alternate the shared scratch-copy arm every 300 frames. CaptureSprites and
# MoveWideHud each do a whole-Ppu memcpy that CopyPpuForWideLine
# never reached -- 149 of the 153 MB/frame this pass copies. On PC removing it
# is a NET LOSS (the memcpy prefetches for the ppu_runLine that follows); on
# Vita the membench says bytes cost ~1 us/KB and caching does not rescue them.
# That disagreement is exactly what this lever is for. MEASUREMENT build only.
option(FZERO_WIDE_SHAREDCOPY_AB "A/B the shared scratch copy every 300 frames" OFF)
# Alternate the coverage-collapse arm every 300 frames. CaptureSprites runs
# ppu_runLine 704 times a frame -- up to 9 times on one scanline -- purely to
# learn which pixels a sprite covered, which the native sprite pass already
# resolves into ppu->objSlotOf. Proven byte-identical on PC over 1,707 distinct
# wide images; this lever prices it here. MEASUREMENT build only.
option(FZERO_WIDE_FASTCOV_AB "A/B the sprite-coverage collapse every 300 frames" OFF)
# Alternate the centred-HUD concession every 300 frames. MoveWideHud spreads
# the HUD to the screen edges and re-renders the scene beneath it; leaving the
# HUD centred skips the pass entirely, measured at 38% of ProcessLine on PC.
# This one CHANGES THE PICTURE on purpose, so WIDECRC must differ between arms.
option(FZERO_WIDE_MOVEHUD_AB "A/B the centred-HUD concession every 300 frames" OFF)
# Log to ux0:/data/fzero_recomp/log.txt instead of the netlog. Required for
# Vita3K, whose sceNet is a stub: debugNetInit faults inside libnet.suprx and
# the app never boots. Harmless but pointless on hardware, where the netlog is
# live and the file would just be a slower copy of it.
option(FZERO_LOG_FILE "Log to a file on ux0 instead of debugnet" OFF)

target_compile_definitions(fzero_recomp PRIVATE
    FZERO_DIAG=$<BOOL:${FZERO_DIAG}>
    FZERO_PPUPROF=$<BOOL:${FZERO_PPUPROF}>
    FZERO_PPU_LUT_AB=$<BOOL:${FZERO_PPU_LUT_AB}>
    FZERO_MEMBENCH=$<BOOL:${FZERO_MEMBENCH}>
    FZERO_WIDESCREEN=$<BOOL:${FZERO_WIDESCREEN}>
    FZERO_WIDEPROF=$<BOOL:${FZERO_WIDEPROF}>
    FZERO_WIDE_SHAREDCOPY_AB=$<BOOL:${FZERO_WIDE_SHAREDCOPY_AB}>
    FZERO_WIDE_FASTCOV_AB=$<BOOL:${FZERO_WIDE_FASTCOV_AB}>
    FZERO_WIDE_MOVEHUD_AB=$<BOOL:${FZERO_WIDE_MOVEHUD_AB}>
    FZERO_LOG_FILE=$<BOOL:${FZERO_LOG_FILE}>
    SNESRECOMP_REVERSE_DEBUG=0
    VITA=1
    __vita__=1
    SNESRECOMP_SDL2=1
)

target_include_directories(fzero_recomp PRIVATE
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    ${SNESRECOMP_RUNNER_ROOT}/src/desktop
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VITA_INCLUDE_DIRS}
)

# Vita-specific configuration
target_link_libraries(fzero_recomp PRIVATE ${VITA_LIBRARIES})
target_compile_options(fzero_recomp PRIVATE ${VITA_COMPILE_FLAGS})
target_link_options(fzero_recomp PRIVATE ${VITA_LINKER_FLAGS})

# Create VPK for Vita
set(VITA_APP_NAME "F-Zero Recomp")
set(VITA_TITLEID "FZER00001")
set(VITA_VERSION "01.00")
vita_create_self(fzero_recomp.self fzero_recomp UNSAFE)
vita_create_vpk(fzero_recomp.vpk ${VITA_TITLEID} fzero_recomp.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
