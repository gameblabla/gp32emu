#include "platform/platform_internal.h"
#include "platform/sdl12/common.h"
#include "audio/gp32_audio_resampler.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GP32_AUDIO_DEFAULT_CAP_FRAMES 8192u
#define GP32_AUDIO_MAX_LATENCY_FRAMES 32768u
#define GP32_AUDIO_START_FRAMES 2048u
#define GP32_AUDIO_TARGET_FRAMES 4096u

#ifdef AUDIO_S16SYS
#define GP32_SDL12_AUDIO_S16 AUDIO_S16SYS
#else
#define GP32_SDL12_AUDIO_S16 AUDIO_S16
#endif

typedef struct sdl12_audio_backend {
    gp32_audio_backend_t base;
    int16_t *queue;
    uint32_t read_frame;
    uint32_t frame_count;
    uint32_t frame_cap;
    int16_t *tmp;
    uint32_t tmp_cap_frames;
    uint32_t sample_rate_hz;
    gp32_audio_resampler_t resampler;
    int underrun;
    int started;
    char error[160];
} sdl12_audio_backend_t;

static uint32_t queued_frames(const sdl12_audio_backend_t *a) { return a ? a->frame_count : 0u; }

static int ensure_tmp_capacity(sdl12_audio_backend_t *a, uint32_t frames) {
    if (frames <= a->tmp_cap_frames) return 1;
    int16_t *n = (int16_t *)realloc(a->tmp, (size_t)frames * 2u * sizeof(int16_t));
    if (!n) return 0;
    a->tmp = n;
    a->tmp_cap_frames = frames;
    return 1;
}

static int ensure_queue_capacity(sdl12_audio_backend_t *a, uint32_t extra_frames) {
    if (!a) return 0;
    if (extra_frames <= a->frame_cap - a->frame_count) return 1;
    uint32_t old_cap = a->frame_cap;
    uint32_t new_cap = old_cap ? old_cap : GP32_AUDIO_DEFAULT_CAP_FRAMES;
    while (new_cap - a->frame_count < extra_frames) {
        if (new_cap > UINT32_MAX / 2u) return 0;
        new_cap *= 2u;
    }
    int16_t *n = (int16_t *)malloc((size_t)new_cap * 2u * sizeof(int16_t));
    if (!n) return 0;
    for (uint32_t i = 0; i < a->frame_count; ++i) {
        uint32_t src = old_cap ? ((a->read_frame + i) % old_cap) : 0u;
        n[(uint64_t)i * 2u + 0u] = a->queue[(uint64_t)src * 2u + 0u];
        n[(uint64_t)i * 2u + 1u] = a->queue[(uint64_t)src * 2u + 1u];
    }
    free(a->queue);
    a->queue = n;
    a->frame_cap = new_cap;
    a->read_frame = 0;
    return 1;
}

static void drop_oldest_frames(sdl12_audio_backend_t *a, uint32_t frames) {
    if (!a || a->frame_cap == 0u || frames == 0u) return;
    if (frames >= a->frame_count) {
        a->read_frame = 0;
        a->frame_count = 0;
        return;
    }
    a->read_frame = (a->read_frame + frames) % a->frame_cap;
    a->frame_count -= frames;
}

static void ring_write_block(sdl12_audio_backend_t *a, const int16_t *src, uint32_t frames) {
    uint32_t done = 0;
    while (done < frames) {
        uint32_t w = (a->read_frame + a->frame_count) % a->frame_cap;
        uint32_t contiguous = a->frame_cap - w;
        uint32_t n = frames - done;
        if (n > contiguous) n = contiguous;
        memcpy(a->queue + (uint64_t)w * 2u, src + (uint64_t)done * 2u, (size_t)n * 2u * sizeof(int16_t));
        a->frame_count += n;
        done += n;
    }
}

static int32_t audio_queue_rate_adjust_ppm(uint32_t queued) {
    const int32_t target = GP32_AUDIO_TARGET_FRAMES;
    const int32_t span = GP32_AUDIO_TARGET_FRAMES;
    const int32_t max_slowdown = 35000;
    const int32_t max_speedup = -25000;
    int32_t err = target - (int32_t)queued;
    if (err > span) err = span;
    if (err < -span) err = -span;
    int32_t ppm = (err * max_slowdown) / span;
    if (ppm < max_speedup) ppm = max_speedup;
    if (ppm > max_slowdown) ppm = max_slowdown;
    return ppm;
}

static void sdl12_audio_callback(void *userdata, Uint8 *stream, int len) {
    sdl12_audio_backend_t *a = (sdl12_audio_backend_t *)userdata;
    int16_t *out = (int16_t *)(void *)stream;
    uint32_t frames_req = (uint32_t)len / (2u * (uint32_t)sizeof(int16_t));
    uint32_t frames_copy = a->frame_count < frames_req ? a->frame_count : frames_req;
    uint32_t copied = 0;
    while (copied < frames_copy) {
        uint32_t contiguous = a->frame_cap - a->read_frame;
        uint32_t n = frames_copy - copied;
        if (n > contiguous) n = contiguous;
        memcpy(out + (uint64_t)copied * 2u, a->queue + (uint64_t)a->read_frame * 2u, (size_t)n * 2u * sizeof(int16_t));
        a->read_frame = (a->read_frame + n) % a->frame_cap;
        a->frame_count -= n;
        copied += n;
    }
    if (frames_copy < frames_req) {
        a->underrun = 1;
        memset(out + (uint64_t)frames_copy * 2u, 0, (size_t)(frames_req - frames_copy) * 2u * sizeof(int16_t));
    }
}

static gp32_status_t sdl12_audio_submit(gp32_audio_backend_t *backend, const gp32_audio_desc_t *audio) {
    sdl12_audio_backend_t *a = (sdl12_audio_backend_t *)backend;
    if (!a || !audio) return GP32_ERR_INVALID_ARGUMENT;
    if (!audio->samples_s16_interleaved || audio->frame_count == 0) return GP32_OK;
    if (audio->frame_count > UINT32_MAX / 2u) {
        gp32_platform_set_error(a->error, sizeof(a->error), "audio chunk too large");
        return GP32_ERR_INVALID_ARGUMENT;
    }
    uint32_t src_rate = audio->sample_rate_hz ? audio->sample_rate_hz : a->sample_rate_hz;
    uint32_t dst_rate = a->sample_rate_hz ? a->sample_rate_hz : src_rate;
    if (!src_rate || !dst_rate) return GP32_OK;

    SDL_LockAudio();
    uint32_t q0 = queued_frames(a);
    int underrun = a->underrun;
    a->underrun = 0;
    int started = a->started;
    SDL_UnlockAudio();

    if (underrun) gp32_audio_resampler_mark_gap(&a->resampler, dst_rate);
    int32_t adjust_ppm = started ? audio_queue_rate_adjust_ppm(q0) : 0;
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
        gp32_platform_set_error(a->error, sizeof(a->error), "audio resample buffer allocation failed");
        return GP32_ERR_NO_MEMORY;
    }
    size_t out_sz = gp32_audio_resampler_process(&a->resampler,
                                                 audio->samples_s16_interleaved,
                                                 (size_t)audio->frame_count,
                                                 src_rate,
                                                 dst_rate,
                                                 adjust_ppm,
                                                 a->tmp,
                                                 max_out);
    if (!out_sz) return GP32_OK;
    if (out_sz > UINT32_MAX) {
        gp32_platform_set_error(a->error, sizeof(a->error), "resampled audio chunk too large");
        return GP32_ERR_INVALID_ARGUMENT;
    }
    uint32_t out_frames = (uint32_t)out_sz;

    SDL_LockAudio();
    uint32_t q = queued_frames(a);
    if (q + out_frames > GP32_AUDIO_MAX_LATENCY_FRAMES) {
        drop_oldest_frames(a, q + out_frames - GP32_AUDIO_MAX_LATENCY_FRAMES);
    }
    if (!ensure_queue_capacity(a, out_frames)) {
        SDL_UnlockAudio();
        gp32_platform_set_error(a->error, sizeof(a->error), "audio queue allocation failed");
        return GP32_ERR_NO_MEMORY;
    }
    ring_write_block(a, a->tmp, out_frames);
    if (!a->started && a->frame_count >= GP32_AUDIO_START_FRAMES) {
        a->started = 1;
        SDL_PauseAudio(0);
    }
    SDL_UnlockAudio();
    return GP32_OK;
}

static uint32_t sdl12_audio_buffered_frames(gp32_audio_backend_t *backend) {
    sdl12_audio_backend_t *a = (sdl12_audio_backend_t *)backend;
    if (!a) return 0;
    SDL_LockAudio();
    uint32_t frames = queued_frames(a);
    SDL_UnlockAudio();
    return frames;
}

static const char *sdl12_audio_error(const gp32_audio_backend_t *backend) {
    const sdl12_audio_backend_t *a = (const sdl12_audio_backend_t *)backend;
    return (a && a->error[0]) ? a->error : "";
}

static void sdl12_audio_destroy(gp32_audio_backend_t *backend) {
    sdl12_audio_backend_t *a = (sdl12_audio_backend_t *)backend;
    if (!a) return;
    SDL_CloseAudio();
    gp32_sdl12_quit_subsystem(SDL_INIT_AUDIO);
    free(a->queue);
    free(a->tmp);
    free(a);
}

gp32_audio_backend_t *gp32_audio_sdl12_create(const gp32_audio_options_t *options) {
    if (gp32_sdl12_init_subsystem(SDL_INIT_AUDIO) != 0) return NULL;
    sdl12_audio_backend_t *a = (sdl12_audio_backend_t *)calloc(1, sizeof(*a));
    if (!a) {
        gp32_sdl12_quit_subsystem(SDL_INIT_AUDIO);
        return NULL;
    }
    a->base.destroy = sdl12_audio_destroy;
    a->base.submit = sdl12_audio_submit;
    a->base.buffered_frames = sdl12_audio_buffered_frames;
    a->base.error = sdl12_audio_error;
    a->sample_rate_hz = options && options->sample_rate_hz ? options->sample_rate_hz : 44100u;
    gp32_audio_resampler_init(&a->resampler);

    SDL_AudioSpec want;
    SDL_AudioSpec have;
    memset(&want, 0, sizeof(want));
    memset(&have, 0, sizeof(have));
    want.freq = (int)a->sample_rate_hz;
    want.format = GP32_SDL12_AUDIO_S16;
    want.channels = 2;
    want.samples = (Uint16)((options && options->buffer_frames) ? options->buffer_frames : 2048u);
    want.callback = sdl12_audio_callback;
    want.userdata = a;
    if (SDL_OpenAudio(&want, &have) != 0) {
        gp32_platform_set_error(a->error, sizeof(a->error), SDL_GetError());
        free(a);
        gp32_sdl12_quit_subsystem(SDL_INIT_AUDIO);
        return NULL;
    }
    if (have.format != GP32_SDL12_AUDIO_S16 || have.channels != 2) {
        gp32_platform_set_error(a->error, sizeof(a->error), "SDL audio device did not provide signed 16-bit stereo");
        SDL_CloseAudio();
        free(a);
        gp32_sdl12_quit_subsystem(SDL_INIT_AUDIO);
        return NULL;
    }
    a->sample_rate_hz = (uint32_t)have.freq;
    if (!ensure_queue_capacity(a, GP32_AUDIO_DEFAULT_CAP_FRAMES)) {
        gp32_platform_set_error(a->error, sizeof(a->error), "audio queue allocation failed");
        SDL_CloseAudio();
        free(a);
        gp32_sdl12_quit_subsystem(SDL_INIT_AUDIO);
        return NULL;
    }
    SDL_PauseAudio(1);
    return &a->base;
}
