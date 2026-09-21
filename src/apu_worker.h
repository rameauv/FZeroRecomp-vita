/*
 * apu_worker.h - SNES APU on the second core. See apu_worker.c.
 */
#ifndef FZERO_APU_WORKER_H
#define FZERO_APU_WORKER_H

#include <stdbool.h>

/* Start the worker thread and arm the runner's worker path. Call AFTER
 * LoadAndInitSNES. Honours FZERO_APU_WORKER=0 to start disabled. */
bool apu_worker_start(void);

/* Disarm and join. Call BEFORE any emulator state is freed. */
void apu_worker_stop(void);

/* Re-log the two affinity masks and whether they are disjoint. The netlog is
 * usually read as a tail, so the startup line alone is not reliable. */
void apu_worker_log_affinity(void);

/* Runtime A/B toggle. */
void apu_worker_set(bool on);
bool apu_worker_enabled(void);

#endif /* FZERO_APU_WORKER_H */
