/*
 * audio_sdl.h - SDL2 audio output for FZeroRecomp on PS Vita
 *
 * Based on patterns from sm64-vita's audio_sdl.h
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct Snes;

/* ========================================================================
 * INITIALIZATION
 * ======================================================================== */

/*
 * Initialize SDL2 audio system
 * 
 * @return true if audio was initialized successfully
 */
bool audio_init(void);

/*
 * Shutdown SDL2 audio system
 */
void audio_shutdown(void);

/* ========================================================================
 * AUDIO STATUS
 * ======================================================================== */

/*
 * Check if audio queue can accept more data
 * 
 * @return true if there is space in the audio queue
 */
bool audio_can_queue(void);

/* Begin playback; call after the SNES instance exists. */
void audio_start(void);

/* ========================================================================
 * AUDIO SUBMISSION
 * ======================================================================== */

/*
 * Submit stereo 16-bit audio samples to the SDL2 audio queue
 * 
 * @param samples Pointer to interleaved stereo 16-bit samples (L, R, L, R, ...)
 * @param num_samples Number of stereo sample pairs (frames)
 * @return true if samples were queued, false if buffer is full
 */
bool audio_submit_samples(int16_t *samples, uint32_t num_samples);

/*
 * Render and submit audio from the DSP ring buffer
 * 
 * @param snes Pointer to the SNES instance
 * @param target_samples Target number of samples to render (stereo pairs)
 * @return Number of samples actually submitted
 */
int audio_render_and_submit(struct Snes *snes, int target_samples);

/*
 * Render and submit one frame's worth of audio
 * 
 * Uses snesrecomp's RtlRenderAudio for proper rate conversion.
 * This is the recommended approach.
 * 
 * @param snes Pointer to the SNES instance
 * @return true if audio was rendered and submitted
 */
bool audio_render_frame(struct Snes *snes);

/* ========================================================================
 * AUDIO CONTROL
 * ======================================================================== */

/*
 * Set audio volume (placeholder for now)
 * 
 * @param volume Volume level (0.0 to 1.0)
 */
void audio_set_volume(float volume);
