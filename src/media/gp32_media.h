#ifndef GP32EMU_MEDIA_H
#define GP32EMU_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#include "gp32emu/gp32.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GP32_MEDIA_LCD_W 320u
#define GP32_MEDIA_LCD_H 240u

int gp32_media_frame_to_bgra_320x240(const gp32_framebuffer_desc_t *fb, uint32_t *out_bgra);
int gp32_media_write_bmp_320x240(const char *path, const gp32_framebuffer_desc_t *fb, char *err, size_t err_len);

typedef struct gp32_media_recorder gp32_media_recorder_t;

gp32_media_recorder_t *gp32_media_recorder_open(const char *path, uint32_t audio_rate_hz, char *err, size_t err_len);
int gp32_media_recorder_add_frame(gp32_media_recorder_t *r, const gp32_framebuffer_desc_t *fb, uint64_t frame_index);
int gp32_media_recorder_add_audio(gp32_media_recorder_t *r, const gp32_audio_desc_t *audio);
int gp32_media_recorder_close(gp32_media_recorder_t *r);
void gp32_media_recorder_abort(gp32_media_recorder_t *r);
const char *gp32_media_recorder_error(const gp32_media_recorder_t *r);

#ifdef __cplusplus
}
#endif

#endif
