#ifndef GP32EMU_VIDEO_EFFECTS_H
#define GP32EMU_VIDEO_EFFECTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GP32_VIDEO_EFFECTS_W 320u
#define GP32_VIDEO_EFFECTS_H 240u
#define GP32_VIDEO_EFFECTS_PIXELS (GP32_VIDEO_EFFECTS_W * GP32_VIDEO_EFFECTS_H)

typedef struct gp32_video_effects {
    uint32_t *prev_raw;
    uint32_t *prev_lcd;
    int have_prev_raw;
    int have_prev_lcd;
    int lcd_persistence;
    int frame_interpolation;
} gp32_video_effects_t;

int gp32_video_effects_init(gp32_video_effects_t *fx);
void gp32_video_effects_shutdown(gp32_video_effects_t *fx);
void gp32_video_effects_reset(gp32_video_effects_t *fx);
void gp32_video_effects_set(gp32_video_effects_t *fx, int lcd_persistence, int frame_interpolation);
int gp32_video_effects_lcd_persistence(const gp32_video_effects_t *fx);
int gp32_video_effects_frame_interpolation(const gp32_video_effects_t *fx);
int gp32_video_effects_active(const gp32_video_effects_t *fx);
int gp32_video_effects_process_320x240(gp32_video_effects_t *fx, const uint32_t *src_rgb, uint32_t *dst_rgb);

#ifdef __cplusplus
}
#endif

#endif /* GP32EMU_VIDEO_EFFECTS_H */
