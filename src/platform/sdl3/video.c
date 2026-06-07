#include "platform/platform_internal.h"
#include "platform/sdl3/common.h"
#include "gp32emu/video_effects.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GP32_LCD_W 320u
#define GP32_LCD_H 240u

typedef struct sdl3_video_backend {
    gp32_video_backend_t base;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint32_t *pixels;
    uint32_t *stage_rgb;
    uint32_t pixels_cap;
    uint32_t scale;
    uint32_t out_w;
    uint32_t out_h;
    int rotate;
    int fullscreen;
    gp32_video_effects_t effects;
    char title[128];
    char error[160];
} sdl3_video_backend_t;

static void copy_frame_rgb(sdl3_video_backend_t *v, const gp32_framebuffer_desc_t *fb, uint32_t out_w, uint32_t out_h) {
    if (!v || !fb || !v->stage_rgb) return;
    const uint32_t *src = fb->pixels_rgba8888;
    uint32_t *dst = v->stage_rgb;
    uint32_t stride = fb->stride_pixels;
    switch (v->rotate) {
    case 1: /* 90 degrees counter-clockwise */
        for (uint32_t y = 0; y < out_h; ++y) {
            uint32_t sx = fb->width - 1u - y;
            for (uint32_t x = 0; x < out_w; ++x) dst[y * out_w + x] = src[(size_t)x * stride + sx] & 0x00ffffffu;
        }
        break;
    case 2: /* 180 degrees */
        for (uint32_t y = 0; y < out_h; ++y) {
            const uint32_t *row = src + (fb->height - 1u - y) * stride;
            for (uint32_t x = 0; x < out_w; ++x) dst[y * out_w + x] = row[fb->width - 1u - x] & 0x00ffffffu;
        }
        break;
    case 3: /* 90 degrees clockwise */
        for (uint32_t y = 0; y < out_h; ++y) {
            for (uint32_t x = 0; x < out_w; ++x) dst[y * out_w + x] = src[(fb->height - 1u - x) * stride + y] & 0x00ffffffu;
        }
        break;
    default:
        for (uint32_t y = 0; y < out_h; ++y) {
            const uint32_t *row = src + y * stride;
            for (uint32_t x = 0; x < out_w; ++x) dst[y * out_w + x] = row[x] & 0x00ffffffu;
        }
        break;
    }
}

static int ensure_texture(sdl3_video_backend_t *v, uint32_t out_w, uint32_t out_h) {
    if (v->texture && v->out_w == out_w && v->out_h == out_h) return 1;
    if (v->texture) { SDL_DestroyTexture(v->texture); v->texture = NULL; }
    uint32_t need = out_w * out_h;
    if (need > v->pixels_cap) {
        uint32_t *p = (uint32_t *)realloc(v->pixels, (size_t)need * sizeof(uint32_t));
        if (!p) { gp32_platform_set_error(v->error, sizeof(v->error), "SDL3 video pixel buffer allocation failed"); return 0; }
        v->pixels = p;
        uint32_t *s = (uint32_t *)realloc(v->stage_rgb, (size_t)need * sizeof(uint32_t));
        if (!s) { gp32_platform_set_error(v->error, sizeof(v->error), "SDL3 video staging buffer allocation failed"); return 0; }
        v->stage_rgb = s;
        v->pixels_cap = need;
    }
    v->texture = SDL_CreateTexture(v->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, (int)out_w, (int)out_h);
    if (!v->texture) { gp32_platform_set_error(v->error, sizeof(v->error), SDL_GetError()); return 0; }
    v->out_w = out_w;
    v->out_h = out_h;
    if (!v->fullscreen) (void)SDL_SetWindowSize(v->window, (int)(out_w * v->scale), (int)(out_h * v->scale));
    return 1;
}

static gp32_status_t sdl3_video_present(gp32_video_backend_t *backend, const gp32_framebuffer_desc_t *fb) {
    sdl3_video_backend_t *v = (sdl3_video_backend_t *)backend;
    if (!v || !fb || !fb->pixels_rgba8888 || fb->width == 0 || fb->height == 0 || fb->stride_pixels < fb->width) return GP32_ERR_INVALID_ARGUMENT;
    const uint32_t out_w = (v->rotate == 1 || v->rotate == 3) ? fb->height : fb->width;
    const uint32_t out_h = (v->rotate == 1 || v->rotate == 3) ? fb->width : fb->height;
    if (!ensure_texture(v, out_w, out_h)) return GP32_ERR_IO;
    copy_frame_rgb(v, fb, out_w, out_h);
    uint32_t *present = v->stage_rgb;
    if (gp32_video_effects_active(&v->effects) && out_w == GP32_LCD_W && out_h == GP32_LCD_H) {
        if (!gp32_video_effects_process_320x240(&v->effects, v->stage_rgb, v->pixels)) return GP32_ERR_IO;
        present = v->pixels;
    } else if (gp32_video_effects_active(&v->effects)) {
        gp32_video_effects_reset(&v->effects);
    }
    for (uint32_t i = 0, n = out_w * out_h; i < n; ++i) v->pixels[i] = 0xff000000u | (present[i] & 0x00ffffffu);
    if (!SDL_UpdateTexture(v->texture, NULL, v->pixels, (int)(out_w * sizeof(uint32_t)))) { gp32_platform_set_error(v->error, sizeof(v->error), SDL_GetError()); return GP32_ERR_IO; }
    if (!SDL_RenderClear(v->renderer)) { gp32_platform_set_error(v->error, sizeof(v->error), SDL_GetError()); return GP32_ERR_IO; }
    SDL_FRect dst;
    dst.x = 0.0f; dst.y = 0.0f; dst.w = (float)(out_w * v->scale); dst.h = (float)(out_h * v->scale);
    if (!SDL_RenderTexture(v->renderer, v->texture, NULL, &dst)) { gp32_platform_set_error(v->error, sizeof(v->error), SDL_GetError()); return GP32_ERR_IO; }
    if (!SDL_RenderPresent(v->renderer)) { gp32_platform_set_error(v->error, sizeof(v->error), SDL_GetError()); return GP32_ERR_IO; }
    return GP32_OK;
}

static gp32_status_t sdl3_video_set_title(gp32_video_backend_t *backend, const char *title) {
    sdl3_video_backend_t *v = (sdl3_video_backend_t *)backend;
    if (!v) return GP32_ERR_INVALID_ARGUMENT;
    if (!title || !title[0]) title = "gp32emu SDL3";
    snprintf(v->title, sizeof(v->title), "%s", title);
    if (!SDL_SetWindowTitle(v->window, v->title)) { gp32_platform_set_error(v->error, sizeof(v->error), SDL_GetError()); return GP32_ERR_IO; }
    return GP32_OK;
}

static const char *sdl3_video_error(const gp32_video_backend_t *backend) {
    const sdl3_video_backend_t *v = (const sdl3_video_backend_t *)backend;
    return (v && v->error[0]) ? v->error : "";
}

static void sdl3_video_destroy(gp32_video_backend_t *backend) {
    sdl3_video_backend_t *v = (sdl3_video_backend_t *)backend;
    if (!v) return;
    gp32_video_effects_shutdown(&v->effects);
    if (v->texture) SDL_DestroyTexture(v->texture);
    if (v->renderer) SDL_DestroyRenderer(v->renderer);
    if (v->window) SDL_DestroyWindow(v->window);
    free(v->pixels);
    free(v->stage_rgb);
    gp32_sdl3_quit_subsystem(SDL_INIT_VIDEO);
    free(v);
}

gp32_video_backend_t *gp32_video_sdl3_create(const gp32_video_options_t *options) {
    if (gp32_sdl3_init_subsystem(SDL_INIT_VIDEO) != 0) return NULL;
    sdl3_video_backend_t *v = (sdl3_video_backend_t *)calloc(1, sizeof(*v));
    if (!v) { gp32_sdl3_quit_subsystem(SDL_INIT_VIDEO); return NULL; }
    v->base.destroy = sdl3_video_destroy;
    v->base.present = sdl3_video_present;
    v->base.set_title = sdl3_video_set_title;
    v->base.error = sdl3_video_error;
    v->scale = options && options->scale ? options->scale : 2u;
    if (v->scale > 8u) v->scale = 8u;
    v->rotate = options ? options->rotate : 1;
    if (v->rotate < 0 || v->rotate > 3) v->rotate = 0;
    v->fullscreen = options ? options->fullscreen : 0;
    if (!gp32_video_effects_init(&v->effects)) { gp32_platform_set_error(v->error, sizeof(v->error), "SDL3 video effect buffer allocation failed"); sdl3_video_destroy(&v->base); return NULL; }
    gp32_video_effects_set(&v->effects, options ? options->lcd_persistence : 0, options ? options->frame_interpolation : 0);
    SDL_WindowFlags flags = v->fullscreen ? SDL_WINDOW_FULLSCREEN : 0;
    const char *title = (options && options->title) ? options->title : "gp32emu SDL3";
    v->window = SDL_CreateWindow(title, (int)(320u * v->scale), (int)(240u * v->scale), flags);
    if (!v->window) { gp32_platform_set_error(v->error, sizeof(v->error), SDL_GetError()); sdl3_video_destroy(&v->base); return NULL; }
    v->renderer = SDL_CreateRenderer(v->window, NULL);
    if (!v->renderer) { gp32_platform_set_error(v->error, sizeof(v->error), SDL_GetError()); sdl3_video_destroy(&v->base); return NULL; }
    (void)sdl3_video_set_title(&v->base, title);
    return &v->base;
}
