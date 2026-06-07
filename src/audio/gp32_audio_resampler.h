#ifndef GP32EMU_AUDIO_RESAMPLER_H
#define GP32EMU_AUDIO_RESAMPLER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gp32_audio_resampler {
    uint32_t src_rate;
    uint32_t dst_rate;
    uint64_t phase_q32;
    int16_t prev_l;
    int16_t prev_r;
    int have_prev;
    int16_t last_out_l;
    int16_t last_out_r;
    int have_last_out;
    uint32_t fade_left;
    uint32_t fade_total;
} gp32_audio_resampler_t;

void gp32_audio_resampler_init(gp32_audio_resampler_t *r);
void gp32_audio_resampler_reset(gp32_audio_resampler_t *r);
void gp32_audio_resampler_mark_gap(gp32_audio_resampler_t *r, uint32_t dst_rate_hz);
size_t gp32_audio_resampler_max_output_frames(const gp32_audio_resampler_t *r,
                                              size_t input_frames,
                                              uint32_t src_rate_hz,
                                              uint32_t dst_rate_hz,
                                              int32_t rate_adjust_ppm);
size_t gp32_audio_resampler_process(gp32_audio_resampler_t *r,
                                    const int16_t *src_s16_stereo,
                                    size_t input_frames,
                                    uint32_t src_rate_hz,
                                    uint32_t dst_rate_hz,
                                    int32_t rate_adjust_ppm,
                                    int16_t *dst_s16_stereo,
                                    size_t dst_cap_frames);

#ifdef __cplusplus
}
#endif

#endif /* GP32EMU_AUDIO_RESAMPLER_H */
