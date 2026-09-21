# Architecture

The ROM and the reviewed cfg files in `config/` feed snesrecomp's analyser,
which emits C for proven `(pc24, M, X)` execution variants into `generated/`.
Unresolved execution falls back to snesrecomp's shared 65816 interpreter. The
Vita host schedules execution, renders the SNES PPU output through SDL2, and
runs the APU on the second CPU core.

## Ownership

| Component | Responsibility |
| --- | --- |
| `src/main_vita.c` | ROM verification, SDL2 video setup, the frame loop, pacing, and shutdown |
| `src/platform_vita.c`, `src/platform.h` | Paths under `ux0:/data/fzero_recomp/`, clock setup, logging |
| `src/input_vita.c` | Vita pad to SNES controller mapping and the quit combo |
| `src/audio_sdl.c`, `src/apu_worker.c` | SDL2 audio output and the APU thread on the second core |
| `src/fzero_rtl.c`, `src/fzero_spc_player.c` | Game scheduling and SPC player integration |
| `src/fzero_layers.c`, `src/fzero_ground.c`, `src/fzero_vehicles.c` | Scanline hook and the (disabled) widescreen pass |
| `snesrecomp/` | Analysis, C emission, interpreter, and SNES devices |
| `tools/regenerate.sh` | ROM check, snesrecomp patching, and generation of `generated/` |

Generic dependency changes belong in snesrecomp. The patch series applied
before generation is described in [SNESRECOMP_PATCHES.md](SNESRECOMP_PATCHES.md).

## Frame scheduling

Reset/mainline execution uses the cooperative interpreter bridge. The host
calls the configured NMI and IRQ entries and preserves their hardware stack
model. Each scanline is drawn before the next HDMA/IRQ update, so a handler's
register changes affect the following row.

During this scanline walk the host calls
`snes_set_hdma_beam_enabled(g_snes, false)` so the dependency's beam simulator
does not run a second HDMA engine, then restores the prior setting. Preserve
this ordering when changing the scheduler.

## Pacing

The loop targets the NTSC frame rate, a period of 16,639 µs (about 60.0988 Hz).
The deadline advances by exactly one period each frame and is not resampled
after the sleep. Resampling folds the scheduler's wake-up lateness into the
period and runs the game about 1% slow. That starves the audio ring, which the
audio servo can only correct within ±0.5%. If the loop falls more than two
periods behind, it resyncs instead of running fast to catch up.

`platform_vita.c` requests the maximum rated CPU, bus and GPU clocks at startup
and logs what was actually granted.

## Video

Each frame the PPU renders 256x224 into an ARGB8888 streaming texture. SDL2's
Vita renderer (GXM) scales it to the 960x544 screen, keeping its aspect ratio,
and centres it with bars at the sides.

## Audio

The SPC700 and S-DSP run on a worker thread pinned to the second core
(`apu_worker.c`). The shared state it guards is in snesrecomp's `common_rtl.c`.
The SDL2 audio callback pulls rendered samples. Running the APU on the game
thread costs about 5.7 ms of the 16.6 ms frame, and moving it off costs the
frame only a lock acquisition.

## Saves

SRAM is written back when the app exits, including after the Start + Select
quit combo. The ROM is read from `ux0:/data/fzero_recomp/fzero.sfc` and
checked against the expected SHA-256 before it runs.

## Build options

Every option is off by default. The default build is the one that ships.

| Option | Purpose |
| --- | --- |
| `FZERO_LOG_FILE` | Log to `ux0:/data/fzero_recomp/log.txt` instead of debugnet; needed under Vita3K |
| `FZERO_DIAG` | Bring-up probes (WRAM CRCs, interpreter step ring); costs frame rate |
| `FZERO_PPUPROF` | Per-scanline PPU phase timing |
| `FZERO_WIDESCREEN` | Experimental 398x224 widescreen pass; far below 60 FPS on Vita |
| `FZERO_WIDEPROF`, `FZERO_MEMBENCH`, `FZERO_*_AB` | Measurement probes and A/B switches for the options above |
