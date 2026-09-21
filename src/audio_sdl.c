/*
 * audio_sdl.c - SDL2 audio output for FZeroRecomp on PS Vita
 *
 * Based on patterns from sm64-vita's audio_sdl.c
 * 
 * Callback-driven, mirroring the desktop host: SDL's audio thread pulls
 * RtlRenderAudio, which advances the SPC/DSP under the APU lock. That is
 * snesrecomp's own model -- the APU is a separate chip and its emulation runs
 * on its own thread, serialised by the ~125 RtlApuLock sites in the runner.
 *
 * This replaces the queue API this file was originally written against. That
 * came from sm64-vita's audio backend, which fits SM64 (whose audio engine is
 * the game's own code, synthesised from the main thread) but not a
 * continuously-running chip emulation.
 */

#include "audio_sdl.h"

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <psp2/kernel/processmgr.h>

#include "platform.h"
#include "snes/snes.h"
#include "snes/dsp.h"
#include "common_rtl.h"

/* The emulator the callback renders from; NULL until LoadAndInitSNES runs. */
extern Snes *g_snes;

/* ========================================================================
 * LOCAL DEFINITIONS
 * ======================================================================== */

/* Audio device handle */
static SDL_AudioDeviceID g_audio_device = 0;

/* Serialises APU state between the game thread and SDL's audio thread. This
 * backs RtlApuLock/RtlApuUnlock, whose call sites already bracket every APU
 * access in the runner. SDL2 mutexes are recursive, which matters because
 * RtlRenderAudio takes the lock itself while the callback already holds it. */
static SDL_mutex *g_audio_mutex = NULL;

/* One render block, handed out to the callback in device-sized pieces. */
static uint8_t *g_audiobuffer = NULL;
static uint8_t *g_audiobuffer_cur = NULL;
static uint8_t *g_audiobuffer_end = NULL;
static int g_frames_per_block = 0;

/* Audio format parameters */
#define AUDIO_FREQUENCY 48000    /* Output sample rate - Vita hardware native rate */
#define AUDIO_FORMAT AUDIO_S16LSB  /* 16-bit signed little-endian */
#define AUDIO_CHANNELS 2        /* Stereo */

/* With rate conversion handled by RtlRenderAudio, we can use any sample count.
 * Using 534 maintains compatibility with the original frame-based approach,
 * but RtlRenderAudio will convert to the output rate automatically. */
#define AUDIO_SAMPLES_PER_FRAME 534  /* Original SNES frame size */
#define AUDIO_SAMPLES 512       /* Samples per chunk */

/* Target latency in bytes (approx 100ms at 48kHz stereo 16-bit) */
#define TARGET_LATENCY_BYTES (48000 * 2 * 2 * 0.1)

/* Maximum buffer size to prevent overflow */
#define MAX_QUEUE_BYTES (48000 * 2 * 2 * 0.5)  /* ~500ms */

/* ========================================================================
 * APU LOCK  (was stubbed no-op in platform_vita.c while this port was
 * single-threaded; the runner's call sites have always been there)
 * ======================================================================== */

void RtlApuLock(void) {
    if (g_audio_mutex) SDL_LockMutex(g_audio_mutex);
}

void RtlApuUnlock(void) {
    if (g_audio_mutex) SDL_UnlockMutex(g_audio_mutex);
}

/* ========================================================================
 * AUDIO CALLBACK  (runs on SDL's audio thread)
 * ======================================================================== */

/* What the audio thread costs, measured rather than assumed: two timer reads
 * per callback, reported per game frame by the heartbeat. This also measures
 * the thread's RtlApuLock hold, which is what the APU worker contends with.
 *
 * Note SDL hands this host ONE ~800-frame callback per game frame, not the
 * 512-sample buffers AUDIO_SAMPLES asks for -- which is why the probe reports
 * calls/frame rather than assuming a rate. */
uint64_t g_audthread_us = 0, g_audthread_calls = 0, g_audthread_max_us = 0;

static void FillAudioBuffer(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    uint64_t t_cb = sceKernelGetProcessTimeWide();
    /* SDL's audio thread outlives the emulator at both ends of the run, and
     * RtlRenderAudio dereferences g_snes->apu->dsp unconditionally. Opening
     * the device before LoadAndInitSNES and letting the callback fire is what
     * crashed the first build of this change. Silence is the correct output
     * when there is no chip to render from. */
    if (!g_audiobuffer || !g_snes || !g_snes->apu || !g_snes->apu->dsp) {
        memset(stream, 0, (size_t)len);
        return;
    }
    SDL_LockMutex(g_audio_mutex);
    while (len != 0) {
        if (g_audiobuffer_end - g_audiobuffer_cur == 0) {
            /* Consumer only: this renders what the SPC has produced, it does
             * not invent SPC cycles. The guest frame remains the clock. */
            RtlRenderAudio((int16_t *)g_audiobuffer, g_frames_per_block,
                           AUDIO_CHANNELS);
            g_audiobuffer_cur = g_audiobuffer;
            g_audiobuffer_end = g_audiobuffer +
                (size_t)g_frames_per_block * AUDIO_CHANNELS * sizeof(int16_t);
        }
        int n = (len < (int)(g_audiobuffer_end - g_audiobuffer_cur))
                    ? len : (int)(g_audiobuffer_end - g_audiobuffer_cur);
        memcpy(stream, g_audiobuffer_cur, (size_t)n);
        g_audiobuffer_cur += n;
        stream += n;
        len -= n;
    }
    SDL_UnlockMutex(g_audio_mutex);
    { uint64_t d = sceKernelGetProcessTimeWide() - t_cb;
      g_audthread_us += d; g_audthread_calls++;
      if (d > g_audthread_max_us) g_audthread_max_us = d; }
}

/* ========================================================================
 * INITIALIZATION
 * ======================================================================== */

bool audio_init(void) {
    if (g_audio_device != 0) {
        FZERO_LOG("INFO", "Audio already initialized");
        return true;
    }

    FZERO_LOG("INFO", "Initializing SDL2 audio for Vita");

    /* Initialize SDL audio subsystem if not already initialized */
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            FZERO_LOG("ERROR", "SDL audio initialization failed: %s", SDL_GetError());
            return false;
        }
    }

    /* Must exist before the device opens: the callback can fire as soon as
     * playback is unpaused, and the game thread takes this lock too. */
    if (!g_audio_mutex) {
        g_audio_mutex = SDL_CreateMutex();
        if (!g_audio_mutex) {
            FZERO_LOG("ERROR", "SDL_CreateMutex failed: %s", SDL_GetError());
            return false;
        }
    }

    SDL_AudioSpec want = {0};
    want.freq = AUDIO_FREQUENCY;
    want.format = AUDIO_FORMAT;
    want.channels = AUDIO_CHANNELS;
    want.samples = AUDIO_SAMPLES;
    want.callback = FillAudioBuffer;  /* APU advances on SDL's audio thread */

    SDL_AudioSpec have;
    g_audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);

    if (g_audio_device == 0) {
        FZERO_LOG("ERROR", "SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return false;
    }

    FZERO_LOG("INFO", "Audio device opened: requested freq=%d, format=%d, channels=%d",
              want.freq, want.format, want.channels);
    FZERO_LOG("INFO", "Audio device actual: freq=%d, format=%d (%s), channels=%d",
              have.freq, have.format,
              have.format == AUDIO_U8 ? "U8" :
              have.format == AUDIO_S8 ? "S8" :
              have.format == AUDIO_U16LSB ? "U16LSB" :
              have.format == AUDIO_S16LSB ? "S16LSB" :
              have.format == AUDIO_U16MSB ? "U16MSB" :
              have.format == AUDIO_S16MSB ? "S16MSB" :
              have.format == AUDIO_F32LSB ? "F32LSB" :
              have.format == AUDIO_F32MSB ? "F32MSB" : "unknown",
              have.channels);

    /* Set the output rate for snesrecomp's audio rendering to match what SDL2 gave us */
    RtlSetAudioOutputRate(have.freq);

    /* RtlRenderAudio counts frames at the OUTPUT rate, not SNES samples:
     * 32040 -> 534 1:1, 44100 -> 735, 48000 -> 800. The queue-API version of
     * this file passed 534 unconditionally while running the device at
     * 48 kHz, i.e. it produced two thirds of a frame's audio every frame. */
    g_frames_per_block =
        (AUDIO_SAMPLES_PER_FRAME * have.freq + 32040 / 2) / 32040;
    g_audiobuffer = (uint8_t *)calloc(
        (size_t)g_frames_per_block * AUDIO_CHANNELS * sizeof(int16_t), 1);
    if (!g_audiobuffer) {
        FZERO_LOG("ERROR", "audio buffer allocation failed");
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
        return false;
    }
    g_audiobuffer_cur = g_audiobuffer_end = g_audiobuffer;
    FZERO_LOG("INFO", "Audio block: %d frames/block at %d Hz (was 534)",
              g_frames_per_block, have.freq);

    /* If SDL2 didn't give us the format we requested, we may need to handle conversion.
     * For now, we assume it matches or SDL2 will handle it. The key fix is setting
     * the output rate correctly to avoid SDL2 doing rate conversion which
     * might trigger the buggy U8->F32 conversion path. */
    if (have.format != want.format) {
        FZERO_LOG("WARNING", "Audio format mismatch: requested %d, got %d",
                  want.format, have.format);
    }

    /* Deliberately NOT unpaused here. The callback renders from g_snes, which
     * LoadAndInitSNES has not built yet at this point in startup; the desktop
     * host likewise opens audio only after the SNES exists. audio_start()
     * below begins playback once there is something to play. */
    FZERO_LOG("INFO", "SDL2 audio initialized (paused until SNES is up)");
    return true;
}

/*
 * Begin playback. Call once the SNES instance exists.
 */
void audio_start(void) {
    if (g_audio_device == 0) return;
    SDL_PauseAudioDevice(g_audio_device, 0);
    FZERO_LOG("INFO", "Audio playback started");
}

void audio_shutdown(void) {
    if (g_audio_device == 0) {
        return;
    }

    FZERO_LOG("INFO", "Shutting down SDL2 audio");

    /* Stop audio playback */
    SDL_PauseAudioDevice(g_audio_device, 1);

    /* Close the device first: that joins SDL's audio thread, so nothing can
     * be inside the callback when the buffer and mutex go away. */
    SDL_CloseAudioDevice(g_audio_device);
    g_audio_device = 0;

    free(g_audiobuffer);
    g_audiobuffer = g_audiobuffer_cur = g_audiobuffer_end = NULL;

    if (g_audio_mutex) { SDL_DestroyMutex(g_audio_mutex); g_audio_mutex = NULL; }

    FZERO_LOG("INFO", "SDL2 audio shutdown complete");
}

/* ========================================================================
 * AUDIO STATUS
 * ======================================================================== */

/*
 * Get the current amount of queued audio in bytes
 */
static uint32_t audio_get_queued_bytes(void) {
    if (g_audio_device == 0) {
        return 0;
    }
    return SDL_GetQueuedAudioSize(g_audio_device);
}

/*
 * Check if audio can accept more data
 */
bool audio_can_queue(void) {
    return audio_get_queued_bytes() < MAX_QUEUE_BYTES;
}

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
bool audio_submit_samples(int16_t *samples, uint32_t num_samples) {
    /* Dead since this host went callback-driven: SDL_QueueAudio does nothing
     * on a device opened with a callback. Fail loudly rather than silently
     * dropping audio if something starts calling this again. */
    static bool warned = false;
    if (!warned) {
        warned = true;
        FZERO_LOG("ERROR", "audio_submit_samples called on a callback-driven "
                           "device: the audio thread pulls RtlRenderAudio now");
    }
    (void)samples; (void)num_samples;
    return false;
}

bool audio_submit_samples_unused_(int16_t *samples, uint32_t num_samples) {
    if (g_audio_device == 0) {
        return false;
    }

    /* Check if we have room in the queue */
    if (!audio_can_queue()) {
        return false;
    }

    /* Queue the audio data */
    size_t bytes = num_samples * sizeof(int16_t) * 2;  /* stereo */
    int result = SDL_QueueAudio(g_audio_device, samples, bytes);

    if (result != 0) {
        FZERO_LOG("ERROR", "SDL_QueueAudio failed: %s", SDL_GetError());
        return false;
    }

    return true;
}

/*
 * Render and submit audio from the DSP ring buffer
 *
 * This function should be called periodically (e.g., each frame) to
 * extract audio samples from snesrecomp's DSP and submit them to SDL2.
 *
 * @param snes Pointer to the SNES instance
 * @param target_samples Target number of samples to render (stereo pairs)
 * @return Number of samples actually submitted
 */
int audio_render_and_submit(Snes *snes, int target_samples) {
    if (g_audio_device == 0 || snes == NULL || snes->apu == NULL || snes->apu->dsp == NULL) {
        return 0;
    }

    Dsp *dsp = snes->apu->dsp;

    /* Check how many samples are available in the DSP ring buffer */
    uint32_t available = dsp_available(dsp);
    if (available == 0) {
        return 0;
    }

    /* Limit to what we need and what the queue can accept */
    uint32_t samples_to_convert = target_samples;
    if (samples_to_convert > available) {
        samples_to_convert = available;
    }

    /* Check queue space before converting */
    size_t bytes_needed = samples_to_convert * sizeof(int16_t) * 2;
    if (audio_get_queued_bytes() + bytes_needed > MAX_QUEUE_BYTES) {
        /* Not enough space, try fewer samples */
        size_t available_space = MAX_QUEUE_BYTES - audio_get_queued_bytes();
        samples_to_convert = available_space / (sizeof(int16_t) * 2);
        if (samples_to_convert == 0) {
            return 0;
        }
        samples_to_convert = (samples_to_convert > available) ? available : samples_to_convert;
    }

    /* Temporary buffer for interleaved stereo samples (L, R, L, R, ...) */
    int16_t temp_buffer[samples_to_convert * 2];

    for (uint32_t i = 0; i < samples_to_convert; i++) {
        int16_t left, right;
        dsp_peek(dsp, i, &left, &right);
        temp_buffer[i * 2] = left;
        temp_buffer[i * 2 + 1] = right;
    }

    /* Advance the read pointer in the DSP ring buffer */
    dsp_advance(dsp, samples_to_convert);

    /* Submit to SDL2 */
    if (audio_submit_samples(temp_buffer, samples_to_convert)) {
        return samples_to_convert;
    }

    return 0;
}

/*
 * Simplified version that uses RtlRenderAudio directly
 * This is the recommended approach as it handles rate conversion internally
 *
 * @param snes Pointer to the SNES instance
 * @return true if audio was rendered and submitted
 */
bool audio_render_frame(Snes *snes) {
    /* Deliberately does nothing now. The audio thread pulls RtlRenderAudio;
     * rendering here as well would consume the DSP ring from two threads and
     * make the host callback a second, competing emulation clock -- exactly
     * what common_rtl.c:2280 warns against. Kept as a no-op so the game loop
     * needs no change and the call site still documents where audio is due. */
    (void)snes;
    return true;
}

/* ========================================================================
 * AUDIO VOLUME CONTROL
 * ======================================================================== */

void audio_set_volume(float volume) {
    if (g_audio_device == 0) {
        return;
    }

    /* SDL2 doesn't have direct volume control for queued audio */
    /* Volume is controlled at the sample level before submission */
    /* For now, this is a placeholder */
    FZERO_LOG("INFO", "Audio volume set to: %.2f", volume);
}
