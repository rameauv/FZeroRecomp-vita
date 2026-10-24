/*
 * audio_sdl.c - SDL3 audio output for FZeroRecomp on PS Vita.
 *
 * Callback-driven, as on the desktop host: SDL's audio thread pulls
 * RtlRenderAudio, which advances the SPC/DSP under the APU lock. The APU is a
 * separate chip, so its emulation runs on its own thread, serialised by the
 * RtlApuLock sites in the runner.
 *
 * SDL3 audio uses the stream API: SDL_OpenAudioDeviceStream opens a device,
 * creates a stream, and binds them. The callback pushes rendered blocks into
 * the stream with SDL_PutAudioStreamData; SDL converts to the device format
 * and feeds the hardware.
 */

#include "audio_sdl.h"

#define SDL_ENABLE_OLD_NAMES
#include <SDL3/SDL.h>
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

/* SDL3 stream-based audio: the stream is the primary handle; the device ID
 * is derived from it for pause/resume. */
static SDL_AudioStream *g_audio_stream = NULL;
static SDL_AudioDeviceID g_audio_device = 0;

/* Serialises APU state between the game thread and SDL's audio thread. This
 * backs RtlApuLock/RtlApuUnlock, whose call sites already bracket every APU
 * access in the runner. SDL3 mutexes are recursive, which matters because
 * RtlRenderAudio takes the lock itself while the callback already holds it. */
static SDL_Mutex *g_audio_mutex = NULL;

/* One render block, handed out to the callback in device-sized pieces. */
static uint8_t *g_audiobuffer = NULL;
static uint8_t *g_audiobuffer_cur = NULL;
static uint8_t *g_audiobuffer_end = NULL;
static int g_frames_per_block = 0;

#define AUDIO_FREQUENCY 48000    /* the Vita's native output rate */
#define AUDIO_FORMAT SDL_AUDIO_S16LE
#define AUDIO_CHANNELS 2

#define AUDIO_SAMPLES_PER_FRAME 534  /* SNES samples per frame at 32,040 Hz */

/* About 100 ms at 48 kHz stereo 16-bit. */
#define TARGET_LATENCY_BYTES (48000 * 2 * 2 * 0.1)

/* About 500 ms. */
#define MAX_QUEUE_BYTES (48000 * 2 * 2 * 0.5)

/* Silence buffer for the callback when no emulator is running. Sized for a
 * typical SDL3 callback request (a few thousand bytes). */
#define SILENCE_BUF_SIZE 8192
static uint8_t g_silence_buf[SILENCE_BUF_SIZE];

void RtlApuLock(void) {
    if (g_audio_mutex) SDL_LockMutex(g_audio_mutex);
}

void RtlApuUnlock(void) {
    if (g_audio_mutex) SDL_UnlockMutex(g_audio_mutex);
}

/* The audio thread's cost, including its RtlApuLock hold, which the APU
 * worker contends with. Two timer reads per callback; the heartbeat reports
 * it per game frame. SDL gives this host one ~800-frame callback per game
 * frame, not the 512-sample buffers AUDIO_SAMPLES asks for, so the probe
 * counts calls instead of assuming a rate. */
uint64_t g_audthread_us = 0, g_audthread_calls = 0, g_audthread_max_us = 0;

/* SDL3 audio callback: called on SDL's audio thread when the device wants
 * more data. additional_amount is how many bytes SDL wants now; we render
 * blocks and push them into the stream with SDL_PutAudioStreamData. */
static void SDLCALL FillAudioBuffer(void *userdata, SDL_AudioStream *stream,
                                    int additional_amount, int total_amount) {
    (void)userdata;
    (void)total_amount;
    uint64_t t_cb = sceKernelGetProcessTimeWide();
    /* SDL's audio thread outlives the emulator at both ends of the run, and
     * RtlRenderAudio dereferences g_snes->apu->dsp unconditionally. Output
     * silence when there is no chip to render from. */
    if (!g_audiobuffer || !g_snes || !g_snes->apu || !g_snes->apu->dsp) {
        while (additional_amount > 0) {
            int n = additional_amount < SILENCE_BUF_SIZE ? additional_amount : SILENCE_BUF_SIZE;
            SDL_PutAudioStreamData(stream, g_silence_buf, n);
            additional_amount -= n;
        }
        return;
    }
    SDL_LockMutex(g_audio_mutex);
    while (additional_amount > 0) {
        if (g_audiobuffer_end - g_audiobuffer_cur == 0) {
            /* Consumer only: this renders what the SPC has produced, it does
             * not invent SPC cycles. The guest frame remains the clock. */
            RtlRenderAudio((int16_t *)g_audiobuffer, g_frames_per_block,
                           AUDIO_CHANNELS);
            g_audiobuffer_cur = g_audiobuffer;
            g_audiobuffer_end = g_audiobuffer +
                (size_t)g_frames_per_block * AUDIO_CHANNELS * sizeof(int16_t);
        }
        int avail = (int)(g_audiobuffer_end - g_audiobuffer_cur);
        int n = (additional_amount < avail) ? additional_amount : avail;
        SDL_PutAudioStreamData(stream, g_audiobuffer_cur, n);
        g_audiobuffer_cur += n;
        additional_amount -= n;
    }
    SDL_UnlockMutex(g_audio_mutex);
    { uint64_t d = sceKernelGetProcessTimeWide() - t_cb;
      g_audthread_us += d; g_audthread_calls++;
      if (d > g_audthread_max_us) g_audthread_max_us = d; }
}

bool audio_init(void) {
    if (g_audio_stream != NULL) {
        FZERO_LOG("INFO", "Audio already initialized");
        return true;
    }

    FZERO_LOG("INFO", "Initializing SDL3 audio for Vita");

    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
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

    /* SDL3: the spec describes the format WE push. SDL converts to the
     * device's native format. No callback/samples fields in SDL3's spec. */
    SDL_AudioSpec spec = { AUDIO_FORMAT, AUDIO_CHANNELS, AUDIO_FREQUENCY };

    g_audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, FillAudioBuffer, NULL);

    if (!g_audio_stream) {
        FZERO_LOG("ERROR", "SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
        return false;
    }

    g_audio_device = SDL_GetAudioStreamDevice(g_audio_stream);

    FZERO_LOG("INFO", "Audio stream opened: freq=%d, format=S16LE, channels=%d",
              spec.freq, spec.channels);

    /* The output rate is what we pushed; SDL resamples to the device. */
    RtlSetAudioOutputRate(spec.freq);

    /* RtlRenderAudio counts frames at the OUTPUT rate, not SNES samples:
     * 32040 -> 534 1:1, 44100 -> 735, 48000 -> 800. */
    g_frames_per_block =
        (AUDIO_SAMPLES_PER_FRAME * spec.freq + 32040 / 2) / 32040;
    g_audiobuffer = (uint8_t *)calloc(
        (size_t)g_frames_per_block * AUDIO_CHANNELS * sizeof(int16_t), 1);
    if (!g_audiobuffer) {
        FZERO_LOG("ERROR", "audio buffer allocation failed");
        SDL_DestroyAudioStream(g_audio_stream);
        g_audio_stream = NULL;
        g_audio_device = 0;
        return false;
    }
    g_audiobuffer_cur = g_audiobuffer_end = g_audiobuffer;
    FZERO_LOG("INFO", "Audio block: %d frames/block at %d Hz (was 534)",
              g_frames_per_block, spec.freq);

    /* Not unpaused here: the callback renders from g_snes, which
     * LoadAndInitSNES has not built yet. audio_start() begins playback.
     * SDL3 opens the device in a paused state. */
    FZERO_LOG("INFO", "SDL3 audio initialized (paused until SNES is up)");
    return true;
}

/* Begin playback. Call once the SNES instance exists. */
void audio_start(void) {
    if (g_audio_stream == NULL) return;
    SDL_ResumeAudioStreamDevice(g_audio_stream);
    FZERO_LOG("INFO", "Audio playback started");
}

void audio_shutdown(void) {
    if (g_audio_stream == NULL) {
        return;
    }

    FZERO_LOG("INFO", "Shutting down SDL3 audio");

    /* Pause the device first so the callback stops firing. */
    if (g_audio_device != 0) {
        SDL_PauseAudioDevice(g_audio_device);
    }

    /* Destroy the stream: that joins SDL's audio thread, so nothing can
     * be inside the callback when the buffer and mutex go away. */
    SDL_DestroyAudioStream(g_audio_stream);
    g_audio_stream = NULL;
    g_audio_device = 0;

    free(g_audiobuffer);
    g_audiobuffer = g_audiobuffer_cur = g_audiobuffer_end = NULL;

    if (g_audio_mutex) { SDL_DestroyMutex(g_audio_mutex); g_audio_mutex = NULL; }

    FZERO_LOG("INFO", "SDL3 audio shutdown complete");
}

static uint32_t audio_get_queued_bytes(void) {
    if (g_audio_stream == NULL) {
        return 0;
    }
    return (uint32_t)SDL_GetAudioStreamQueued(g_audio_stream);
}

bool audio_can_queue(void) {
    return audio_get_queued_bytes() < MAX_QUEUE_BYTES;
}

/* Interleaved stereo samples; num_samples counts stereo pairs. */
bool audio_submit_samples(int16_t *samples, uint32_t num_samples) {
    /* The stream is callback-driven, so report the call instead of dropping
     * the audio silently. */
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
    if (g_audio_stream == NULL) {
        return false;
    }

    if (!audio_can_queue()) {
        return false;
    }

    size_t bytes = num_samples * sizeof(int16_t) * 2;
    if (!SDL_PutAudioStreamData(g_audio_stream, samples, (int)bytes)) {
        FZERO_LOG("ERROR", "SDL_PutAudioStreamData failed: %s", SDL_GetError());
        return false;
    }

    return true;
}

/* Move up to target_samples stereo pairs from the DSP ring to the queue.
 * Returns the number submitted. */
int audio_render_and_submit(Snes *snes, int target_samples) {
    if (g_audio_stream == NULL || snes == NULL || snes->apu == NULL || snes->apu->dsp == NULL) {
        return 0;
    }

    Dsp *dsp = snes->apu->dsp;

    uint32_t available = dsp_available(dsp);
    if (available == 0) {
        return 0;
    }

    uint32_t samples_to_convert = target_samples;
    if (samples_to_convert > available) {
        samples_to_convert = available;
    }

    size_t bytes_needed = samples_to_convert * sizeof(int16_t) * 2;
    if (audio_get_queued_bytes() + bytes_needed > MAX_QUEUE_BYTES) {
        size_t available_space = MAX_QUEUE_BYTES - audio_get_queued_bytes();
        samples_to_convert = available_space / (sizeof(int16_t) * 2);
        if (samples_to_convert == 0) {
            return 0;
        }
        samples_to_convert = (samples_to_convert > available) ? available : samples_to_convert;
    }

    int16_t temp_buffer[samples_to_convert * 2];

    for (uint32_t i = 0; i < samples_to_convert; i++) {
        int16_t left, right;
        dsp_peek(dsp, i, &left, &right);
        temp_buffer[i * 2] = left;
        temp_buffer[i * 2 + 1] = right;
    }

    dsp_advance(dsp, samples_to_convert);

    if (audio_submit_samples(temp_buffer, samples_to_convert)) {
        return samples_to_convert;
    }

    return 0;
}

bool audio_render_frame(Snes *snes) {
    /* No-op: the audio thread pulls RtlRenderAudio. Rendering here too would
     * consume the DSP ring from two threads and make the host a second,
     * competing emulation clock. */
    (void)snes;
    return true;
}

void audio_set_volume(float volume) {
    if (g_audio_stream == NULL) {
        return;
    }

    /* Not implemented: SDL3 has no volume control for a device stream. */
    FZERO_LOG("INFO", "Audio volume set to: %.2f", volume);
}
