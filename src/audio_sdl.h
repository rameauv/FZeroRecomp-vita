/* audio_sdl.h - SDL3 audio output for FZeroRecomp on PS Vita. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct Snes;

bool audio_init(void);

void audio_shutdown(void);

bool audio_can_queue(void);

/* Begin playback; call after the SNES instance exists. */
void audio_start(void);

/* Unused: the device is callback-driven. Interleaved stereo samples;
 * num_samples counts stereo pairs. */
bool audio_submit_samples(int16_t *samples, uint32_t num_samples);

/* Returns the number of stereo pairs submitted. */
int audio_render_and_submit(struct Snes *snes, int target_samples);

/* No-op: the audio thread pulls RtlRenderAudio. */
bool audio_render_frame(struct Snes *snes);

/* Not implemented. volume is 0.0 to 1.0. */
void audio_set_volume(float volume);
