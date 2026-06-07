#include "audio/gp32_audio_resampler.h"

#include <stdint.h>
#include <string.h>

#define GP32_AUDIO_Q32_ONE 4294967296.0
#define GP32_AUDIO_FADE_MAX_FRAMES 96u
#define GP32_AUDIO_FADE_MIN_FRAMES 16u
#define GP32_AUDIO_ADJUST_MIN_PPM (-50000)
#define GP32_AUDIO_ADJUST_MAX_PPM (50000)

static int32_t clamp_ppm(int32_t ppm) {
    if (ppm < GP32_AUDIO_ADJUST_MIN_PPM) return GP32_AUDIO_ADJUST_MIN_PPM;
    if (ppm > GP32_AUDIO_ADJUST_MAX_PPM) return GP32_AUDIO_ADJUST_MAX_PPM;
    return ppm;
}

static uint64_t step_q32(uint32_t src_rate, uint32_t dst_rate, int32_t rate_adjust_ppm) {
    if (!src_rate || !dst_rate) return 0;
    rate_adjust_ppm = clamp_ppm(rate_adjust_ppm);
    double ratio = 1.0 + (double)rate_adjust_ppm / 1000000.0;
    if (ratio < 0.50) ratio = 0.50;
    double den = (double)dst_rate * ratio;
    if (den <= 1.0) den = 1.0;
    uint64_t step = (uint64_t)(((double)src_rate * GP32_AUDIO_Q32_ONE) / den + 0.5);
    return step ? step : 1u;
}

static int16_t lerp_s16_q32(int16_t a, int16_t b, uint32_t frac) {
    int64_t av = (int64_t)a;
    int64_t dv = (int64_t)b - av;
    return (int16_t)(av + ((dv * (int64_t)frac) >> 32));
}

static int16_t fade_s16(int16_t from, int16_t to, uint32_t num, uint32_t den) {
    int64_t av = (int64_t)from;
    int64_t dv = (int64_t)to - av;
    return (int16_t)(av + (dv * (int64_t)num) / (int64_t)den);
}

void gp32_audio_resampler_init(gp32_audio_resampler_t *r) {
    if (r) memset(r, 0, sizeof(*r));
}

void gp32_audio_resampler_reset(gp32_audio_resampler_t *r) {
    if (!r) return;
    int16_t last_l = r->last_out_l;
    int16_t last_r = r->last_out_r;
    int have_last = r->have_last_out;
    memset(r, 0, sizeof(*r));
    r->last_out_l = last_l;
    r->last_out_r = last_r;
    r->have_last_out = have_last;
}

void gp32_audio_resampler_mark_gap(gp32_audio_resampler_t *r, uint32_t dst_rate_hz) {
    if (!r) return;
    gp32_audio_resampler_reset(r);
    uint32_t fade = dst_rate_hz ? dst_rate_hz / 1000u : 48u; /* about 1 ms */
    if (fade < GP32_AUDIO_FADE_MIN_FRAMES) fade = GP32_AUDIO_FADE_MIN_FRAMES;
    if (fade > GP32_AUDIO_FADE_MAX_FRAMES) fade = GP32_AUDIO_FADE_MAX_FRAMES;
    r->fade_left = fade;
    r->fade_total = fade;
}

size_t gp32_audio_resampler_max_output_frames(const gp32_audio_resampler_t *r,
                                              size_t input_frames,
                                              uint32_t src_rate_hz,
                                              uint32_t dst_rate_hz,
                                              int32_t rate_adjust_ppm) {
    (void)r;
    if (!input_frames || !src_rate_hz || !dst_rate_hz) return 0;
    rate_adjust_ppm = clamp_ppm(rate_adjust_ppm);
    double ratio = 1.0 + (double)rate_adjust_ppm / 1000000.0;
    if (ratio < 0.50) ratio = 0.50;
    double frames = ((double)(input_frames + 2u) * (double)dst_rate_hz * ratio) / (double)src_rate_hz;
    if (frames < 8.0) frames = 8.0;
    return (size_t)(frames + 32.0);
}

static void get_sample(const gp32_audio_resampler_t *r, const int16_t *src, size_t input_frames, int have_prev, size_t idx, int16_t *l, int16_t *rr) {
    if (have_prev) {
        if (idx == 0u) { *l = r->prev_l; *rr = r->prev_r; return; }
        idx--;
    }
    if (idx >= input_frames) idx = input_frames ? input_frames - 1u : 0u;
    *l = src[idx * 2u + 0u];
    *rr = src[idx * 2u + 1u];
}

size_t gp32_audio_resampler_process(gp32_audio_resampler_t *r,
                                    const int16_t *src_s16_stereo,
                                    size_t input_frames,
                                    uint32_t src_rate_hz,
                                    uint32_t dst_rate_hz,
                                    int32_t rate_adjust_ppm,
                                    int16_t *dst_s16_stereo,
                                    size_t dst_cap_frames) {
    if (!r || !src_s16_stereo || !dst_s16_stereo || !input_frames || !src_rate_hz || !dst_rate_hz || !dst_cap_frames) return 0;

    if (r->src_rate != src_rate_hz || r->dst_rate != dst_rate_hz) {
        r->src_rate = src_rate_hz;
        r->dst_rate = dst_rate_hz;
        r->phase_q32 = 0;
        r->have_prev = 0;
    }

    int have_prev = r->have_prev;
    size_t total_samples = input_frames + (have_prev ? 1u : 0u);
    if (total_samples < 2u) {
        r->prev_l = src_s16_stereo[(input_frames - 1u) * 2u + 0u];
        r->prev_r = src_s16_stereo[(input_frames - 1u) * 2u + 1u];
        r->have_prev = 1;
        return 0;
    }

    size_t intervals = total_samples - 1u;
    uint64_t limit_q32 = (uint64_t)intervals << 32;
    uint64_t step = step_q32(src_rate_hz, dst_rate_hz, rate_adjust_ppm);
    size_t produced = 0;

    while (r->phase_q32 < limit_q32 && produced < dst_cap_frames) {
        size_t idx = (size_t)(r->phase_q32 >> 32);
        uint32_t frac = (uint32_t)r->phase_q32;
        int16_t l0, r0, l1, r1;
        get_sample(r, src_s16_stereo, input_frames, have_prev, idx, &l0, &r0);
        get_sample(r, src_s16_stereo, input_frames, have_prev, idx + 1u, &l1, &r1);
        int16_t l = lerp_s16_q32(l0, l1, frac);
        int16_t rr = lerp_s16_q32(r0, r1, frac);

        if (r->fade_left && r->fade_total) {
            uint32_t done = r->fade_total - r->fade_left + 1u;
            uint32_t den = r->fade_total + 1u;
            int16_t from_l = r->have_last_out ? r->last_out_l : 0;
            int16_t from_r = r->have_last_out ? r->last_out_r : 0;
            l = fade_s16(from_l, l, done, den);
            rr = fade_s16(from_r, rr, done, den);
            r->fade_left--;
        }

        dst_s16_stereo[produced * 2u + 0u] = l;
        dst_s16_stereo[produced * 2u + 1u] = rr;
        r->last_out_l = l;
        r->last_out_r = rr;
        r->have_last_out = 1;
        produced++;
        r->phase_q32 += step;
    }

    if (r->phase_q32 >= limit_q32) r->phase_q32 -= limit_q32;
    else {
        /* Output capacity was too small.  Do not carry a phase beyond this
         * block without retaining the full input; clamp to the final interval
         * instead of risking a bad next-block index.  Normal callers use the
         * max-output helper and should not hit this path. */
        r->phase_q32 = 0;
    }

    r->prev_l = src_s16_stereo[(input_frames - 1u) * 2u + 0u];
    r->prev_r = src_s16_stereo[(input_frames - 1u) * 2u + 1u];
    r->have_prev = 1;
    return produced;
}
