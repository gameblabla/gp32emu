#include "gp32emu/video_effects.h"

#include <stdlib.h>
#include <string.h>

static uint32_t blend_half(uint32_t a, uint32_t b) {
    const uint32_t r = ((((a >> 16) & 255u) + ((b >> 16) & 255u)) >> 1) & 255u;
    const uint32_t g = ((((a >> 8) & 255u) + ((b >> 8) & 255u)) >> 1) & 255u;
    const uint32_t bl = (((a & 255u) + (b & 255u)) >> 1) & 255u;
    return (r << 16) | (g << 8) | bl;
}

static uint32_t blend_lcd_persistence(uint32_t cur, uint32_t old) {
    /* Mild GP32 FLU-style sample-and-hold/response persistence.  The current
       frame remains dominant so menus and pixel art stay legible, while the
       previous persisted output contributes a short motion trail. */
    const uint32_t r = ((((cur >> 16) & 255u) * 3u + ((old >> 16) & 255u) + 2u) >> 2) & 255u;
    const uint32_t g = ((((cur >> 8) & 255u) * 3u + ((old >> 8) & 255u) + 2u) >> 2) & 255u;
    const uint32_t b = (((cur & 255u) * 3u + (old & 255u) + 2u) >> 2) & 255u;
    return (r << 16) | (g << 8) | b;
}

int gp32_video_effects_init(gp32_video_effects_t *fx) {
    if (!fx) return 0;
    memset(fx, 0, sizeof(*fx));
    fx->prev_raw = (uint32_t *)malloc((size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
    fx->prev_lcd = (uint32_t *)malloc((size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
    if (!fx->prev_raw || !fx->prev_lcd) {
        gp32_video_effects_shutdown(fx);
        return 0;
    }
    return 1;
}

void gp32_video_effects_shutdown(gp32_video_effects_t *fx) {
    if (!fx) return;
    free(fx->prev_raw);
    free(fx->prev_lcd);
    memset(fx, 0, sizeof(*fx));
}

void gp32_video_effects_reset(gp32_video_effects_t *fx) {
    if (!fx) return;
    fx->have_prev_raw = 0;
    fx->have_prev_lcd = 0;
}

void gp32_video_effects_set(gp32_video_effects_t *fx, int lcd_persistence, int frame_interpolation) {
    if (!fx) return;
    lcd_persistence = lcd_persistence != 0;
    frame_interpolation = frame_interpolation != 0;
    if (fx->lcd_persistence != lcd_persistence || fx->frame_interpolation != frame_interpolation) gp32_video_effects_reset(fx);
    fx->lcd_persistence = lcd_persistence;
    fx->frame_interpolation = frame_interpolation;
}

int gp32_video_effects_lcd_persistence(const gp32_video_effects_t *fx) { return fx ? fx->lcd_persistence : 0; }
int gp32_video_effects_frame_interpolation(const gp32_video_effects_t *fx) { return fx ? fx->frame_interpolation : 0; }
int gp32_video_effects_active(const gp32_video_effects_t *fx) { return fx && (fx->lcd_persistence || fx->frame_interpolation); }

int gp32_video_effects_process_320x240(gp32_video_effects_t *fx, const uint32_t *src_rgb, uint32_t *dst_rgb) {
    if (!fx || !src_rgb || !dst_rgb || !fx->prev_raw || !fx->prev_lcd) return 0;
    const int interp = fx->frame_interpolation;
    const int lcd = fx->lcd_persistence;
    if (!interp && !lcd) {
        if (dst_rgb != src_rgb) memcpy(dst_rgb, src_rgb, (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
        gp32_video_effects_reset(fx);
        return 1;
    }
    if ((interp && !fx->have_prev_raw) || (lcd && !fx->have_prev_lcd)) {
        if (dst_rgb != src_rgb) memcpy(dst_rgb, src_rgb, (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
        memcpy(fx->prev_raw, src_rgb, (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
        memcpy(fx->prev_lcd, src_rgb, (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
        fx->have_prev_raw = 1;
        fx->have_prev_lcd = 1;
        return 1;
    }
    for (uint32_t i = 0; i < GP32_VIDEO_EFFECTS_PIXELS; ++i) {
        const uint32_t raw = src_rgb[i] & 0x00ffffffu;
        uint32_t p = raw;
        if (interp) p = blend_half(p, fx->prev_raw[i]);
        fx->prev_raw[i] = raw;
        if (lcd) p = blend_lcd_persistence(p, fx->prev_lcd[i]);
        fx->prev_lcd[i] = p;
        dst_rgb[i] = p;
    }
    fx->have_prev_raw = 1;
    fx->have_prev_lcd = 1;
    return 1;
}
