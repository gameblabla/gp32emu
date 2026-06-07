#include "platform/platform_internal.h"
#include "platform/sdl12/common.h"
#include "gp32emu/video_effects.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GP32_UNUSED
#define GP32_UNUSED(x) ((void)(x))
#endif

#define GP32_LCD_W 320u
#define GP32_LCD_H 240u

typedef struct sdl12_video_backend {
    gp32_video_backend_t base;
    SDL_Surface *surface;
    uint32_t *stage_rgb;
    uint32_t *effect_rgb;
    uint32_t pixels_cap;
    uint32_t scale;
    uint32_t out_w;
    uint32_t out_h;
    int rotate;
    int fullscreen;
    gp32_video_effects_t effects;
    char title[128];
    char error[160];
} sdl12_video_backend_t;

static uint32_t src_pixel(const gp32_framebuffer_desc_t *fb, uint32_t x, uint32_t y) {
    return fb->pixels_rgba8888[y * fb->stride_pixels + x] & 0x00ffffffu;
}

static void rotated_source_xy(const gp32_framebuffer_desc_t *fb, int rotate, uint32_t x, uint32_t y, uint32_t *sx, uint32_t *sy) {
    if (rotate == 1) {          /* 90 degrees counter-clockwise */
        *sx = fb->width - 1u - y;
        *sy = x;
    } else if (rotate == 2) {   /* 180 degrees */
        *sx = fb->width - 1u - x;
        *sy = fb->height - 1u - y;
    } else if (rotate == 3) {   /* 90 degrees clockwise */
        *sx = y;
        *sy = fb->height - 1u - x;
    } else {
        *sx = x;
        *sy = y;
    }
}

static int ensure_buffers(sdl12_video_backend_t *v, uint32_t out_w, uint32_t out_h) {
    uint32_t need = out_w * out_h;
    if (need <= v->pixels_cap) return 1;
    uint32_t *stage = (uint32_t *)realloc(v->stage_rgb, (size_t)need * sizeof(uint32_t));
    if (!stage) { gp32_platform_set_error(v->error, sizeof(v->error), "SDL 1.2 video staging allocation failed"); return 0; }
    v->stage_rgb = stage;
    uint32_t *effect = (uint32_t *)realloc(v->effect_rgb, (size_t)need * sizeof(uint32_t));
    if (!effect) { gp32_platform_set_error(v->error, sizeof(v->error), "SDL 1.2 video effect allocation failed"); return 0; }
    v->effect_rgb = effect;
    v->pixels_cap = need;
    return 1;
}

static int ensure_surface(sdl12_video_backend_t *v, uint32_t out_w, uint32_t out_h) {
    const uint32_t scaled_w = out_w * v->scale;
    const uint32_t scaled_h = out_h * v->scale;
    if (v->surface && v->out_w == out_w && v->out_h == out_h) return 1;
    uint32_t flags = SDL_SWSURFACE;
    if (v->fullscreen) flags |= SDL_FULLSCREEN;
    v->surface = SDL_SetVideoMode((int)scaled_w, (int)scaled_h, 32, flags);
    if (!v->surface) {
        gp32_platform_set_error(v->error, sizeof(v->error), SDL_GetError());
        return 0;
    }
    v->out_w = out_w;
    v->out_h = out_h;
    return 1;
}

static void stage_frame_rgb(sdl12_video_backend_t *v, const gp32_framebuffer_desc_t *fb, uint32_t out_w, uint32_t out_h) {
    for (uint32_t y = 0; y < out_h; ++y) {
        uint32_t *dst = v->stage_rgb + (size_t)y * out_w;
        for (uint32_t x = 0; x < out_w; ++x) {
            uint32_t sx, sy;
            rotated_source_xy(fb, v->rotate, x, y, &sx, &sy);
            dst[x] = src_pixel(fb, sx, sy);
        }
    }
}

static gp32_status_t sdl12_video_present(gp32_video_backend_t *backend, const gp32_framebuffer_desc_t *fb) {
    sdl12_video_backend_t *v = (sdl12_video_backend_t *)backend;
    if (!v || !fb || !fb->pixels_rgba8888 || fb->width == 0 || fb->height == 0 || fb->stride_pixels < fb->width) return GP32_ERR_INVALID_ARGUMENT;
    const uint32_t out_w = (v->rotate == 1 || v->rotate == 3) ? fb->height : fb->width;
    const uint32_t out_h = (v->rotate == 1 || v->rotate == 3) ? fb->width : fb->height;
    if (!ensure_surface(v, out_w, out_h) || !ensure_buffers(v, out_w, out_h)) return GP32_ERR_IO;
    stage_frame_rgb(v, fb, out_w, out_h);
    const uint32_t *present = v->stage_rgb;
    if (gp32_video_effects_active(&v->effects) && out_w == GP32_LCD_W && out_h == GP32_LCD_H) {
        if (!gp32_video_effects_process_320x240(&v->effects, v->stage_rgb, v->effect_rgb)) return GP32_ERR_IO;
        present = v->effect_rgb;
    } else if (gp32_video_effects_active(&v->effects)) {
        gp32_video_effects_reset(&v->effects);
    }
    if (SDL_MUSTLOCK(v->surface) && SDL_LockSurface(v->surface) != 0) {
        gp32_platform_set_error(v->error, sizeof(v->error), SDL_GetError());
        return GP32_ERR_IO;
    }
    uint8_t *dst_base = (uint8_t *)v->surface->pixels;
    for (uint32_t y = 0; y < out_h; ++y) {
        const uint32_t *src = present + (size_t)y * out_w;
        for (uint32_t x = 0; x < out_w; ++x) {
            uint32_t p = src[x];
            uint32_t mapped = SDL_MapRGB(v->surface->format, (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p);
            for (uint32_t yy = 0; yy < v->scale; ++yy) {
                uint32_t *row = (uint32_t *)(void *)(dst_base + ((y * v->scale + yy) * (uint32_t)v->surface->pitch));
                for (uint32_t xx = 0; xx < v->scale; ++xx) row[x * v->scale + xx] = mapped;
            }
        }
    }
    if (SDL_MUSTLOCK(v->surface)) SDL_UnlockSurface(v->surface);
    SDL_Flip(v->surface);
    return GP32_OK;
}

static gp32_status_t sdl12_video_set_title(gp32_video_backend_t *backend, const char *title) {
    sdl12_video_backend_t *v = (sdl12_video_backend_t *)backend;
    if (!v) return GP32_ERR_INVALID_ARGUMENT;
    if (!title || !title[0]) title = "gp32emu SDL 1.2";
    snprintf(v->title, sizeof(v->title), "%s", title);
    SDL_WM_SetCaption(v->title, NULL);
    return GP32_OK;
}

static const char *sdl12_video_error(const gp32_video_backend_t *backend) {
    const sdl12_video_backend_t *v = (const sdl12_video_backend_t *)backend;
    return (v && v->error[0]) ? v->error : "";
}

static void sdl12_video_destroy(gp32_video_backend_t *backend) {
    sdl12_video_backend_t *v = (sdl12_video_backend_t *)backend;
    if (!v) return;
    gp32_video_effects_shutdown(&v->effects);
    free(v->stage_rgb);
    free(v->effect_rgb);
    gp32_sdl12_quit_subsystem(SDL_INIT_VIDEO);
    free(v);
}

gp32_video_backend_t *gp32_video_sdl12_create(const gp32_video_options_t *options) {
    if (gp32_sdl12_init_subsystem(SDL_INIT_VIDEO) != 0) return NULL;
    sdl12_video_backend_t *v = (sdl12_video_backend_t *)calloc(1, sizeof(*v));
    if (!v) {
        gp32_sdl12_quit_subsystem(SDL_INIT_VIDEO);
        return NULL;
    }
    v->base.destroy = sdl12_video_destroy;
    v->base.present = sdl12_video_present;
    v->base.set_title = sdl12_video_set_title;
    v->base.error = sdl12_video_error;
    v->scale = options && options->scale ? options->scale : 2u;
    if (v->scale > 8u) v->scale = 8u;
    v->rotate = options ? options->rotate : 1;
    if (v->rotate < 0 || v->rotate > 3) v->rotate = 0;
    v->fullscreen = options ? options->fullscreen : 0;
    if (!gp32_video_effects_init(&v->effects)) { gp32_platform_set_error(v->error, sizeof(v->error), "SDL 1.2 video effect buffer allocation failed"); sdl12_video_destroy(&v->base); return NULL; }
    gp32_video_effects_set(&v->effects, options ? options->lcd_persistence : 0, options ? options->frame_interpolation : 0);
    (void)sdl12_video_set_title(&v->base, (options && options->title) ? options->title : "gp32emu SDL 1.2");
    return &v->base;
}
