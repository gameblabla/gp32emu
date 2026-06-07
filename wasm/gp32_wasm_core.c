#include "gp32emu/gp32.h"
#include "audio/gp32_audio_resampler.h"
#include "gp32emu/video_effects.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define GP32_WASM_STATUS_EMPTY   0u
#define GP32_WASM_STATUS_READY   1u
#define GP32_WASM_STATUS_RUNNING 2u
#define GP32_WASM_STATUS_ERROR   5u

#define GP32_WASM_ERR_NONE       0u
#define GP32_WASM_ERR_ALLOC      1u
#define GP32_WASM_ERR_BAD_BIOS   2u
#define GP32_WASM_ERR_BAD_MEDIA  3u
#define GP32_WASM_ERR_CPU        4u
#define GP32_WASM_ERR_BAD_STATE  5u

#define GP32_WASM_W 320u
#define GP32_WASM_H 240u
#define GP32_WASM_AUDIO_MAX_FRAMES 8192u

extern void __pcfx_wasm_heap_reset(void);
extern uint32_t __pcfx_wasm_heap_used(void);
extern uint32_t pcfx_wasm_vfs_add_file(uint32_t path_ptr, uint32_t path_len, uint32_t data_ptr, uint32_t size);

static gp32_t *g_gp32;
static uint32_t g_status = GP32_WASM_STATUS_EMPTY;
static uint32_t g_error = GP32_WASM_ERR_NONE;
static char g_error_text[256];
static uint8_t g_frame_rgba[GP32_WASM_W * GP32_WASM_H * 4u];
static uint32_t g_frame_rgb[GP32_WASM_W * GP32_WASM_H];
static uint32_t g_frame_effect_rgb[GP32_WASM_W * GP32_WASM_H];
static int16_t g_audio[GP32_WASM_AUDIO_MAX_FRAMES * 2u];
static uint32_t g_audio_frames;
static uint32_t g_audio_rate = 44100u;
static uint8_t *g_save_state_buf;
static uint32_t g_save_state_size;
static uint32_t g_save_state_capacity;
static uint64_t g_frame_count;
static uint64_t g_cycle_accum;
static int g_have_bios;
static int g_have_media;
static gp32_audio_resampler_t g_resampler;
static gp32_video_effects_t g_video_effects;
static int g_video_effects_ready;

static void set_error(uint32_t code, const char *msg) {
    g_error = code;
    if (msg && msg[0]) snprintf(g_error_text, sizeof(g_error_text), "%s", msg);
    else g_error_text[0] = '\0';
    if (code != GP32_WASM_ERR_NONE) g_status = GP32_WASM_STATUS_ERROR;
}

static void clear_error(void) { set_error(GP32_WASM_ERR_NONE, ""); if (g_gp32) g_status = GP32_WASM_STATUS_READY; }

static void destroy_machine(void) {
    if (g_gp32) { gp32_destroy(g_gp32); g_gp32 = NULL; }
    g_have_bios = 0;
    g_have_media = 0;
    g_audio_frames = 0;
    g_save_state_size = 0;
    g_frame_count = 0;
    g_cycle_accum = 0;
    gp32_audio_resampler_reset(&g_resampler);
    if (g_video_effects_ready) gp32_video_effects_reset(&g_video_effects);
}

static int ensure_machine(void) {
    if (g_gp32) return 1;
    gp32_options_t opt;
    memset(&opt, 0, sizeof(opt));
    g_gp32 = gp32_create(&opt);
    if (!g_gp32) { set_error(GP32_WASM_ERR_ALLOC, "gp32_create failed"); return 0; }
    gp32_set_jit(g_gp32, 0); /* WASM always uses the portable bytecode interpreter. */
    g_status = GP32_WASM_STATUS_READY;
    g_error = GP32_WASM_ERR_NONE;
    g_error_text[0] = '\0';
    return 1;
}

static void label_from_ptr(uint32_t ptr, uint32_t len, char *dst, size_t dst_len) {
    if (!dst || !dst_len) return;
    dst[0] = '\0';
    if (!ptr || !len) return;
    if (len >= dst_len) len = (uint32_t)dst_len - 1u;
    memcpy(dst, (const void *)(uintptr_t)ptr, len);
    dst[len] = '\0';
}

static int ends_ci(const char *s, const char *suffix) {
    if (!s || !suffix) return 0;
    size_t ns = strlen(s), nf = strlen(suffix);
    if (ns < nf) return 0;
    s += ns - nf;
    for (size_t i = 0; i < nf; ++i) {
        char a = s[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

static void flush_frame_rgba(const uint32_t *rgb) {
    if (!rgb) rgb = g_frame_rgb;
    for (uint32_t i = 0; i < GP32_WASM_W * GP32_WASM_H; ++i) {
        uint32_t p = rgb[i];
        uint8_t *dst = g_frame_rgba + (size_t)i * 4u;
        dst[0] = (uint8_t)(p >> 16);
        dst[1] = (uint8_t)(p >> 8);
        dst[2] = (uint8_t)p;
        dst[3] = 255u;
    }
}

static void stage_frame(void) {
    if (!g_gp32) return;
    gp32_framebuffer_desc_t fb;
    if (gp32_get_framebuffer(g_gp32, &fb) != GP32_OK || !fb.pixels_rgba8888) return;

    if (fb.width == 240u && fb.height == 320u) {
        for (uint32_t y = 0; y < GP32_WASM_H; ++y) {
            const uint32_t sx = fb.width - 1u - y;
            uint32_t *dst = g_frame_rgb + (size_t)y * GP32_WASM_W;
            for (uint32_t x = 0; x < GP32_WASM_W; ++x) dst[x] = fb.pixels_rgba8888[(size_t)x * fb.stride_pixels + sx] & 0x00ffffffu;
        }
    } else if (fb.width == GP32_WASM_W && fb.height == GP32_WASM_H) {
        for (uint32_t y = 0; y < GP32_WASM_H; ++y) {
            const uint32_t *src = fb.pixels_rgba8888 + (size_t)y * fb.stride_pixels;
            uint32_t *dst = g_frame_rgb + (size_t)y * GP32_WASM_W;
            for (uint32_t x = 0; x < GP32_WASM_W; ++x) dst[x] = src[x] & 0x00ffffffu;
        }
    } else {
        for (uint32_t y = 0; y < GP32_WASM_H; ++y) {
            uint32_t sy = (uint32_t)(((uint64_t)y * fb.height) / GP32_WASM_H);
            if (sy >= fb.height) sy = fb.height ? fb.height - 1u : 0u;
            uint32_t *dst = g_frame_rgb + (size_t)y * GP32_WASM_W;
            for (uint32_t x = 0; x < GP32_WASM_W; ++x) {
                uint32_t sx = (uint32_t)(((uint64_t)x * fb.width) / GP32_WASM_W);
                if (sx >= fb.width) sx = fb.width ? fb.width - 1u : 0u;
                dst[x] = fb.pixels_rgba8888[(size_t)sy * fb.stride_pixels + sx] & 0x00ffffffu;
            }
        }
    }

    const uint32_t *present = g_frame_rgb;
    if (g_video_effects_ready && gp32_video_effects_active(&g_video_effects)) {
        if (gp32_video_effects_process_320x240(&g_video_effects, g_frame_rgb, g_frame_effect_rgb)) present = g_frame_effect_rgb;
    }
    flush_frame_rgba(present);
}

static void collect_audio_append(void) {
    if (!g_gp32 || g_audio_frames >= GP32_WASM_AUDIO_MAX_FRAMES) { if (g_gp32) gp32_clear_audio(g_gp32); return; }
    gp32_audio_desc_t ad;
    if (gp32_get_audio(g_gp32, &ad) != GP32_OK || !ad.samples_s16_interleaved || !ad.frame_count || !ad.sample_rate_hz) return;
    size_t remaining = GP32_WASM_AUDIO_MAX_FRAMES - (size_t)g_audio_frames;
    size_t max_out = gp32_audio_resampler_max_output_frames(&g_resampler, (size_t)ad.frame_count, ad.sample_rate_hz, g_audio_rate, 0);
    if (max_out > remaining) max_out = remaining;
    size_t out = gp32_audio_resampler_process(&g_resampler, ad.samples_s16_interleaved, (size_t)ad.frame_count, ad.sample_rate_hz, g_audio_rate, 0, g_audio + (size_t)g_audio_frames * 2u, max_out);
    if (out > remaining) out = remaining;
    g_audio_frames += (uint32_t)out;
    gp32_clear_audio(g_gp32);
}


static uint32_t run_one_frame(uint32_t button_mask, int do_video, int append_audio) {
    if (!g_gp32 || g_status != GP32_WASM_STATUS_RUNNING) return 0u;
    gp32_set_buttons(g_gp32, button_mask);
    uint32_t run_hz = gp32_get_run_clock_hz(g_gp32);
    if (!run_hz) run_hz = 66000000u;
    g_cycle_accum += (uint64_t)run_hz;
    uint32_t frame_cycles = (uint32_t)(g_cycle_accum / 60u);
    g_cycle_accum -= (uint64_t)frame_cycles * 60u;
    if (!frame_cycles) frame_cycles = 1u;
    gp32_status_t st = gp32_run_cycles(g_gp32, frame_cycles);
    if (st != GP32_OK) { set_error(GP32_WASM_ERR_CPU, gp32_get_error(g_gp32)); return 0u; }
    if (do_video) stage_frame();
    if (append_audio) collect_audio_append();
    ++g_frame_count;
    return 1u;
}

__attribute__((export_name("gp32_wasm_version")))
uint32_t gp32_wasm_version(void) { return 0x00010000u; }

__attribute__((export_name("gp32_wasm_malloc")))
uint32_t gp32_wasm_malloc(uint32_t size) { return (uint32_t)(uintptr_t)malloc(size ? size : 1u); }

__attribute__((export_name("gp32_wasm_heap_used")))
uint32_t gp32_wasm_heap_used(void) { return __pcfx_wasm_heap_used(); }

__attribute__((export_name("gp32_wasm_reset_heap")))
void gp32_wasm_reset_heap(void) {
    destroy_machine();
    __pcfx_wasm_heap_reset();
    memset(&g_video_effects, 0, sizeof(g_video_effects));
    g_video_effects_ready = 0;
    memset(g_frame_rgba, 0, sizeof(g_frame_rgba));
    memset(g_frame_rgb, 0, sizeof(g_frame_rgb));
    memset(g_frame_effect_rgb, 0, sizeof(g_frame_effect_rgb));
    g_save_state_buf = NULL;
    g_save_state_size = 0;
    g_save_state_capacity = 0;
    g_status = GP32_WASM_STATUS_EMPTY;
    g_error = GP32_WASM_ERR_NONE;
    g_error_text[0] = '\0';
    gp32_audio_resampler_init(&g_resampler);
}

__attribute__((export_name("gp32_wasm_init")))
uint32_t gp32_wasm_init(uint32_t sample_rate) {
    g_audio_rate = (sample_rate >= 8000u && sample_rate <= 192000u) ? sample_rate : 44100u;
    gp32_audio_resampler_init(&g_resampler);
    if (!g_video_effects_ready) g_video_effects_ready = gp32_video_effects_init(&g_video_effects);
    else gp32_video_effects_reset(&g_video_effects);
    destroy_machine();
    return ensure_machine() ? 1u : 0u;
}

__attribute__((export_name("gp32_wasm_load_bios")))
uint32_t gp32_wasm_load_bios(uint32_t data_ptr, uint32_t size) {
    if (!ensure_machine() || !data_ptr || !size) return 0u;
    gp32_status_t st = gp32_load_bios_data(g_gp32, (const void *)(uintptr_t)data_ptr, size);
    free((void *)(uintptr_t)data_ptr);
    if (st != GP32_OK) { set_error(GP32_WASM_ERR_BAD_BIOS, gp32_get_error(g_gp32)); return 0u; }
    g_have_bios = 1;
    clear_error();
    return 1u;
}

__attribute__((export_name("gp32_wasm_load_media")))
uint32_t gp32_wasm_load_media(uint32_t data_ptr, uint32_t size, uint32_t name_ptr, uint32_t name_len, uint32_t force_hle) {
    if (!ensure_machine() || !data_ptr || !size) return 0u;
    char name[260];
    label_from_ptr(name_ptr, name_len, name, sizeof(name));
    const void *data = (const void *)(uintptr_t)data_ptr;
    gp32_status_t st = GP32_ERR_BAD_IMAGE;
    int use_hle = force_hle || !g_have_bios;
    if (!use_hle) {
        st = gp32_load_smartmedia_data(g_gp32, data, size);
        if (st == GP32_OK) g_have_media = 1;
    } else if (ends_ci(name, ".fpk")) {
        st = gp32_load_fpk_data(g_gp32, data, size, name[0] ? name : "game.fpk");
    } else if (ends_ci(name, ".fxe") || ends_ci(name, ".gxb") || ends_ci(name, ".gxe")) {
        st = gp32_load_fxe_data(g_gp32, data, size, name[0] ? name : "game.fxe");
    } else {
        st = gp32_load_smartmedia_direct_data(g_gp32, data, size, name[0] ? name : "game.smc");
    }
    free((void *)(uintptr_t)data_ptr);
    if (name_ptr) free((void *)(uintptr_t)name_ptr);
    if (st != GP32_OK) { set_error(GP32_WASM_ERR_BAD_MEDIA, gp32_get_error(g_gp32)); return 0u; }
    g_have_media = 1;
    clear_error();
    return 1u;
}

__attribute__((export_name("gp32_wasm_start")))
uint32_t gp32_wasm_start(void) {
    if (!ensure_machine()) return 0u;
    gp32_set_jit(g_gp32, 0);
    if (g_have_bios) gp32_reset(g_gp32);
    g_cycle_accum = 0;
    g_frame_count = 0;
    g_audio_frames = 0;
    gp32_audio_resampler_reset(&g_resampler);
    if (g_video_effects_ready) gp32_video_effects_reset(&g_video_effects);
    g_status = GP32_WASM_STATUS_RUNNING;
    g_error = GP32_WASM_ERR_NONE;
    g_error_text[0] = '\0';
    return 1u;
}

__attribute__((export_name("gp32_wasm_soft_reset")))
uint32_t gp32_wasm_soft_reset(void) { return gp32_wasm_start(); }

__attribute__((export_name("gp32_wasm_frame")))
uint32_t gp32_wasm_frame(uint32_t button_mask) {
    g_audio_frames = 0;
    return run_one_frame(button_mask, 1, 1);
}

__attribute__((export_name("gp32_wasm_run_frames")))
uint32_t gp32_wasm_run_frames(uint32_t button_mask, uint32_t frame_count, uint32_t stage_last) {
    if (!g_gp32 || g_status != GP32_WASM_STATUS_RUNNING) return 0u;
    if (frame_count == 0u) return 0u;
    if (frame_count > 8u) frame_count = 8u;
    g_audio_frames = 0;
    uint32_t done = 0;
    for (uint32_t i = 0; i < frame_count; ++i) {
        int video = stage_last ? (i + 1u == frame_count) : 0;
        if (!run_one_frame(button_mask, video, 1)) break;
        ++done;
        if (g_audio_frames >= GP32_WASM_AUDIO_MAX_FRAMES - 1024u) break;
    }
    return done;
}

__attribute__((export_name("gp32_wasm_set_input")))
void gp32_wasm_set_input(uint32_t button_mask) {
    if (g_gp32) gp32_set_buttons(g_gp32, button_mask);
}

__attribute__((export_name("gp32_wasm_set_video_effects")))
void gp32_wasm_set_video_effects(uint32_t lcd_persistence, uint32_t frame_interpolation) {
    if (!g_video_effects_ready) g_video_effects_ready = gp32_video_effects_init(&g_video_effects);
    if (g_video_effects_ready) gp32_video_effects_set(&g_video_effects, lcd_persistence != 0u, frame_interpolation != 0u);
}

static int read_file_to_save_buffer(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long end = ftell(f);
    if (end <= 0 || end > 0x7fffffffl) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    if (!g_save_state_buf || g_save_state_capacity < (uint32_t)end) {
        uint8_t *buf = (uint8_t *)realloc(g_save_state_buf, (size_t)end);
        if (!buf) { fclose(f); return 0; }
        g_save_state_buf = buf;
        g_save_state_capacity = (uint32_t)end;
    }
    size_t got = fread(g_save_state_buf, 1, (size_t)end, f);
    int close_ok = (fclose(f) == 0);
    if (got != (size_t)end || !close_ok) return 0;
    g_save_state_size = (uint32_t)got;
    return 1;
}

__attribute__((export_name("gp32_wasm_save_state")))
uint32_t gp32_wasm_save_state(void) {
    static const char path[] = "/gp32_wasm_state.gp32st";
    if (!g_gp32 || g_status != GP32_WASM_STATUS_RUNNING) { set_error(GP32_WASM_ERR_BAD_STATE, "start the loaded BIOS/game before saving a state"); return 0u; }
    g_save_state_size = 0;
    gp32_status_t st = gp32_save_state(g_gp32, path);
    if (st != GP32_OK) { set_error(GP32_WASM_ERR_BAD_STATE, gp32_get_error(g_gp32)); return 0u; }
    if (!read_file_to_save_buffer(path)) { remove(path); set_error(GP32_WASM_ERR_BAD_STATE, "saved state could not be staged for browser storage"); return 0u; }
    remove(path);
    g_error = GP32_WASM_ERR_NONE;
    g_error_text[0] = '\0';
    return 1u;
}

__attribute__((export_name("gp32_wasm_get_save_ptr"))) uint32_t gp32_wasm_get_save_ptr(void) { return (uint32_t)(uintptr_t)g_save_state_buf; }
__attribute__((export_name("gp32_wasm_get_save_size"))) uint32_t gp32_wasm_get_save_size(void) { return g_save_state_size; }

__attribute__((export_name("gp32_wasm_load_state")))
uint32_t gp32_wasm_load_state(uint32_t data_ptr, uint32_t size) {
    static const char path[] = "/gp32_wasm_state_import.gp32st";
    if (!g_gp32 || g_status != GP32_WASM_STATUS_RUNNING || !data_ptr || !size) { set_error(GP32_WASM_ERR_BAD_STATE, "start the matching BIOS/game before loading a state"); return 0u; }
    if (!pcfx_wasm_vfs_add_file((uint32_t)(uintptr_t)path, (uint32_t)(sizeof(path) - 1u), data_ptr, size)) {
        free((void *)(uintptr_t)data_ptr);
        set_error(GP32_WASM_ERR_BAD_STATE, "savestate staging failed");
        return 0u;
    }
    gp32_status_t st = gp32_load_state(g_gp32, path);
    remove(path);
    free((void *)(uintptr_t)data_ptr);
    if (st != GP32_OK) { set_error(GP32_WASM_ERR_BAD_STATE, gp32_get_error(g_gp32)); return 0u; }
    g_audio_frames = 0;
    gp32_audio_resampler_reset(&g_resampler);
    if (g_video_effects_ready) gp32_video_effects_reset(&g_video_effects);
    stage_frame();
    g_status = GP32_WASM_STATUS_RUNNING;
    g_error = GP32_WASM_ERR_NONE;
    g_error_text[0] = '\0';
    return 1u;
}

__attribute__((export_name("gp32_wasm_get_status"))) uint32_t gp32_wasm_get_status(void) { return g_status; }
__attribute__((export_name("gp32_wasm_get_error"))) uint32_t gp32_wasm_get_error(void) { return g_error; }
__attribute__((export_name("gp32_wasm_get_error_ptr"))) uint32_t gp32_wasm_get_error_ptr(void) { return (uint32_t)(uintptr_t)g_error_text; }
__attribute__((export_name("gp32_wasm_get_frame_count"))) uint32_t gp32_wasm_get_frame_count(void) { return (uint32_t)g_frame_count; }
__attribute__((export_name("gp32_wasm_get_width"))) uint32_t gp32_wasm_get_width(void) { return GP32_WASM_W; }
__attribute__((export_name("gp32_wasm_get_height"))) uint32_t gp32_wasm_get_height(void) { return GP32_WASM_H; }
__attribute__((export_name("gp32_wasm_get_pitch_bytes"))) uint32_t gp32_wasm_get_pitch_bytes(void) { return GP32_WASM_W * 4u; }
__attribute__((export_name("gp32_wasm_get_framebuffer"))) uint32_t gp32_wasm_get_framebuffer(void) { return (uint32_t)(uintptr_t)g_frame_rgba; }
__attribute__((export_name("gp32_wasm_get_audio_rate"))) uint32_t gp32_wasm_get_audio_rate(void) { return g_audio_rate; }
__attribute__((export_name("gp32_wasm_get_audio_ptr"))) uint32_t gp32_wasm_get_audio_ptr(void) { return (uint32_t)(uintptr_t)g_audio; }
__attribute__((export_name("gp32_wasm_get_audio_frames"))) uint32_t gp32_wasm_get_audio_frames(void) { return g_audio_frames; }
__attribute__((export_name("gp32_wasm_audio_consume"))) void gp32_wasm_audio_consume(void) { g_audio_frames = 0; }
__attribute__((export_name("gp32_wasm_get_pc"))) uint32_t gp32_wasm_get_pc(void) { return g_gp32 ? gp32_get_pc(g_gp32) : 0u; }
__attribute__((export_name("gp32_wasm_get_cycles_lo"))) uint32_t gp32_wasm_get_cycles_lo(void) { return g_gp32 ? (uint32_t)gp32_get_cycles(g_gp32) : 0u; }
__attribute__((export_name("gp32_wasm_has_bios"))) uint32_t gp32_wasm_has_bios(void) { return (uint32_t)g_have_bios; }
__attribute__((export_name("gp32_wasm_has_media"))) uint32_t gp32_wasm_has_media(void) { return (uint32_t)g_have_media; }
