#include "platform/platform_internal.h"
#include "platform/sdl3/common.h"
#include "audio/gp32_audio_resampler.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GP32_SDL3_AUDIO_MAX_LATENCY_FRAMES 32768u
#define GP32_SDL3_AUDIO_START_FRAMES 4096u
#define GP32_SDL3_AUDIO_SOFT_LIMIT_FRAMES 24576u

typedef struct sdl3_audio_backend {
    gp32_audio_backend_t base;
    SDL_AudioStream *stream;
    int16_t *tmp;
    uint32_t tmp_cap_frames;
    uint32_t sample_rate_hz;
    gp32_audio_resampler_t resampler;
    int started;
    char error[160];
} sdl3_audio_backend_t;

static uint32_t bytes_to_frames(int bytes) {
    return bytes > 0 ? (uint32_t)bytes / (2u * (uint32_t)sizeof(int16_t)) : 0u;
}

static int ensure_tmp_capacity(sdl3_audio_backend_t *a, uint32_t frames) {
    if (frames <= a->tmp_cap_frames) return 1;
    int16_t *n = (int16_t *)realloc(a->tmp, (size_t)frames * 2u * sizeof(int16_t));
    if (!n) return 0;
    a->tmp = n;
    a->tmp_cap_frames = frames;
    return 1;
}

static int32_t audio_queue_rate_adjust_ppm(uint32_t queued_frames) {
    const int32_t target = 4096;
    const int32_t span = 4096;
    const int32_t max_slowdown = 35000; /* +3.5% output frames when the queue is low. */
    const int32_t max_speedup = -25000; /* -2.5% output frames when the queue is high. */
    int32_t err = target - (int32_t)queued_frames;
    if (err > span) err = span;
    if (err < -span) err = -span;
    int32_t ppm = (err * max_slowdown) / span;
    if (ppm < max_speedup) ppm = max_speedup;
    if (ppm > max_slowdown) ppm = max_slowdown;
    return ppm;
}

static gp32_status_t sdl3_audio_submit(gp32_audio_backend_t *backend, const gp32_audio_desc_t *audio) {
    sdl3_audio_backend_t *a = (sdl3_audio_backend_t *)backend;
    if (!a || !audio) return GP32_ERR_INVALID_ARGUMENT;
    if (!audio->samples_s16_interleaved || audio->frame_count == 0) return GP32_OK;
    if (audio->frame_count > UINT32_MAX / 2u) {
        gp32_platform_set_error(a->error, sizeof(a->error), "audio chunk too large");
        return GP32_ERR_INVALID_ARGUMENT;
    }

    uint32_t src_rate = audio->sample_rate_hz ? audio->sample_rate_hz : a->sample_rate_hz;
    uint32_t dst_rate = a->sample_rate_hz ? a->sample_rate_hz : src_rate;
    if (!src_rate || !dst_rate) return GP32_OK;

    int queued_bytes = SDL_GetAudioStreamQueued(a->stream);
    uint32_t queued_frames = bytes_to_frames(queued_bytes);
    if (a->started && queued_frames == 0u) gp32_audio_resampler_mark_gap(&a->resampler, dst_rate);

    int32_t adjust_ppm = a->started ? audio_queue_rate_adjust_ppm(queued_frames) : 0;
    size_t max_out = gp32_audio_resampler_max_output_frames(&a->resampler,
                                                            (size_t)audio->frame_count,
                                                            src_rate,
                                                            dst_rate,
                                                            adjust_ppm);
    if (max_out > UINT32_MAX) {
        gp32_platform_set_error(a->error, sizeof(a->error), "resampled audio chunk too large");
        return GP32_ERR_INVALID_ARGUMENT;
    }
    if (!ensure_tmp_capacity(a, (uint32_t)max_out)) {
        gp32_platform_set_error(a->error, sizeof(a->error), "SDL3 audio resample buffer allocation failed");
        return GP32_ERR_NO_MEMORY;
    }

    size_t out_frames_sz = gp32_audio_resampler_process(&a->resampler,
                                                        audio->samples_s16_interleaved,
                                                        (size_t)audio->frame_count,
                                                        src_rate,
                                                        dst_rate,
                                                        adjust_ppm,
                                                        a->tmp,
                                                        max_out);
    if (!out_frames_sz) return GP32_OK;
    if (out_frames_sz > UINT32_MAX) {
        gp32_platform_set_error(a->error, sizeof(a->error), "resampled audio chunk too large");
        return GP32_ERR_INVALID_ARGUMENT;
    }
    uint32_t out_frames = (uint32_t)out_frames_sz;
    size_t bytes = out_frames_sz * 2u * sizeof(int16_t);
    if (bytes > (size_t)INT_MAX) {
        gp32_platform_set_error(a->error, sizeof(a->error), "SDL3 audio chunk too large");
        return GP32_ERR_INVALID_ARGUMENT;
    }

    const int max_queued = (int)(GP32_SDL3_AUDIO_MAX_LATENCY_FRAMES * 2u * sizeof(int16_t));
    const int soft_queued = (int)(GP32_SDL3_AUDIO_SOFT_LIMIT_FRAMES * 2u * sizeof(int16_t));
    if (queued_bytes > max_queued) {
        (void)SDL_ClearAudioStream(a->stream);
        gp32_audio_resampler_mark_gap(&a->resampler, dst_rate);
        queued_bytes = 0;
    } else if (queued_bytes > soft_queued) {
        return GP32_OK;
    }

    if (out_frames && !SDL_PutAudioStreamData(a->stream, a->tmp, (int)bytes)) {
        gp32_platform_set_error(a->error, sizeof(a->error), SDL_GetError());
        return GP32_ERR_IO;
    }
    queued_bytes = SDL_GetAudioStreamQueued(a->stream);
    if (!a->started && queued_bytes >= (int)(GP32_SDL3_AUDIO_START_FRAMES * 2u * sizeof(int16_t))) {
        a->started = 1;
        if (!SDL_ResumeAudioStreamDevice(a->stream)) {
            gp32_platform_set_error(a->error, sizeof(a->error), SDL_GetError());
            return GP32_ERR_IO;
        }
    }
    return GP32_OK;
}

static uint32_t sdl3_audio_buffered_frames(gp32_audio_backend_t *backend) {
    sdl3_audio_backend_t *a = (sdl3_audio_backend_t *)backend;
    if (!a || !a->stream) return 0;
    int bytes = SDL_GetAudioStreamQueued(a->stream);
    return bytes_to_frames(bytes);
}

static const char *sdl3_audio_error(const gp32_audio_backend_t *backend) {
    const sdl3_audio_backend_t *a = (const sdl3_audio_backend_t *)backend;
    return (a && a->error[0]) ? a->error : "";
}

static void sdl3_audio_destroy(gp32_audio_backend_t *backend) {
    sdl3_audio_backend_t *a = (sdl3_audio_backend_t *)backend;
    if (!a) return;
    if (a->stream) SDL_DestroyAudioStream(a->stream);
    free(a->tmp);
    gp32_sdl3_quit_subsystem(SDL_INIT_AUDIO);
    free(a);
}

gp32_audio_backend_t *gp32_audio_sdl3_create(const gp32_audio_options_t *options) {
    if (gp32_sdl3_init_subsystem(SDL_INIT_AUDIO) != 0) return NULL;
    sdl3_audio_backend_t *a = (sdl3_audio_backend_t *)calloc(1, sizeof(*a));
    if (!a) { gp32_sdl3_quit_subsystem(SDL_INIT_AUDIO); return NULL; }
    a->base.destroy = sdl3_audio_destroy;
    a->base.submit = sdl3_audio_submit;
    a->base.buffered_frames = sdl3_audio_buffered_frames;
    a->base.error = sdl3_audio_error;
    a->sample_rate_hz = options && options->sample_rate_hz ? options->sample_rate_hz : 44100u;
    gp32_audio_resampler_init(&a->resampler);
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = (int)a->sample_rate_hz;
    a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!a->stream) { gp32_platform_set_error(a->error, sizeof(a->error), SDL_GetError()); sdl3_audio_destroy(&a->base); return NULL; }
    (void)options;
    return &a->base;
}
