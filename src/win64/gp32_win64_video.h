#ifndef GP32EMU_WIN64_VIDEO_H
#define GP32EMU_WIN64_VIDEO_H

#include "gp32emu/gp32.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gp32_win64_video gp32_win64_video_t;

gp32_win64_video_t *gp32_win64_video_create(HWND hwnd, unsigned scale, int integer_scaling, int keep_aspect, int lcd_persistence, int frame_interpolation, const char *backend);
void gp32_win64_video_destroy(gp32_win64_video_t *v);
void gp32_win64_video_resize(gp32_win64_video_t *v, unsigned width, unsigned height);
int gp32_win64_video_present(gp32_win64_video_t *v, const gp32_framebuffer_desc_t *fb);
void gp32_win64_video_set_integer_scaling(gp32_win64_video_t *v, int enabled);
int gp32_win64_video_integer_scaling(const gp32_win64_video_t *v);
void gp32_win64_video_set_keep_aspect(gp32_win64_video_t *v, int enabled);
int gp32_win64_video_keep_aspect(const gp32_win64_video_t *v);
void gp32_win64_video_set_fullscreen(gp32_win64_video_t *v, int enabled);
void gp32_win64_video_set_lcd_persistence(gp32_win64_video_t *v, int enabled);
int gp32_win64_video_lcd_persistence(const gp32_win64_video_t *v);
void gp32_win64_video_set_frame_interpolation(gp32_win64_video_t *v, int enabled);
int gp32_win64_video_frame_interpolation(const gp32_win64_video_t *v);
const char *gp32_win64_video_active_backend(const gp32_win64_video_t *v);

#ifdef __cplusplus
}
#endif

#endif /* GP32EMU_WIN64_VIDEO_H */
