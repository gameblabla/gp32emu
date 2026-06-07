/*
 * gp32emu - standalone headless GP32 emulator front-end API
 * C11, no external runtime dependencies.
 *
 * The GP32/S3C2400 hardware model is derived from MAME's Game Park GP32
 * driver by Tim Schuerewegen (BSD-3-Clause). See licenses/.
 */
#ifndef GP32EMU_GP32_H
#define GP32EMU_GP32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gp32 gp32_t;
typedef struct gp32_framebuffer gp32_framebuffer_t;


typedef enum gp32_status {
    GP32_OK = 0,
    GP32_ERR_INVALID_ARGUMENT = -1,
    GP32_ERR_NO_MEMORY = -2,
    GP32_ERR_IO = -3,
    GP32_ERR_BAD_IMAGE = -4,
    GP32_ERR_CPU_FAULT = -5
} gp32_status_t;

typedef enum gp32_button {
    GP32_BUTTON_A      = 1u << 0,
    GP32_BUTTON_B      = 1u << 1,
    GP32_BUTTON_L      = 1u << 2,
    GP32_BUTTON_R      = 1u << 3,
    GP32_BUTTON_START  = 1u << 4,
    GP32_BUTTON_SELECT = 1u << 5,
    GP32_BUTTON_UP     = 1u << 6,
    GP32_BUTTON_DOWN   = 1u << 7,
    GP32_BUTTON_LEFT   = 1u << 8,
    GP32_BUTTON_RIGHT  = 1u << 9
} gp32_button_t;

typedef void (*gp32_log_fn)(void *user, const char *message);

typedef struct gp32_options {
    const char *bios_path;         /* optional; can be loaded later */
    const char *smartmedia_path;   /* optional; can be loaded later */
    size_t ram_size;               /* 0 => GP32 default 8 MiB */
    int enable_trace;              /* non-zero => CPU trace to log callback */
    gp32_log_fn log;
    void *log_user;
} gp32_options_t;

typedef struct gp32_framebuffer_desc {
    const uint32_t *pixels_rgba8888;
    uint32_t width;
    uint32_t height;
    uint32_t stride_pixels;
    uint64_t frame_counter;
} gp32_framebuffer_desc_t;

typedef struct gp32_audio_desc {
    const int16_t *samples_s16_interleaved; /* stereo L,R,L,R at sample_rate_hz */
    uint64_t frame_count;                  /* stereo frames, not int16_t sample count */
    uint32_t sample_rate_hz;
} gp32_audio_desc_t;

gp32_t *gp32_create(const gp32_options_t *options);
void gp32_destroy(gp32_t *gp32);

gp32_status_t gp32_load_bios(gp32_t *gp32, const char *path);
gp32_status_t gp32_load_smartmedia(gp32_t *gp32, const char *path);
/* Buffer loaders are used by the WASM frontend and by hosts that do not expose a filesystem. */
gp32_status_t gp32_load_bios_data(gp32_t *gp32, const void *data, size_t size);
gp32_status_t gp32_load_smartmedia_data(gp32_t *gp32, const void *data, size_t size);
/* BIOSless direct loader for retail SmartMedia images: extracts the first commercial/homebrew executable from FAT and installs file HLE for sibling assets. */
gp32_status_t gp32_load_smartmedia_direct(gp32_t *gp32, const char *path);
gp32_status_t gp32_load_smartmedia_direct_data(gp32_t *gp32, const void *data, size_t size, const char *label);
gp32_status_t gp32_load_fxe(gp32_t *gp32, const char *path);
gp32_status_t gp32_load_fxe_data(gp32_t *gp32, const void *data, size_t size, const char *label);
gp32_status_t gp32_load_fpk(gp32_t *gp32, const char *path);
gp32_status_t gp32_load_fpk_data(gp32_t *gp32, const void *data, size_t size, const char *label);
gp32_status_t gp32_save_smartmedia(gp32_t *gp32, const char *path);
gp32_status_t gp32_save_state(gp32_t *gp32, const char *path);
gp32_status_t gp32_load_state(gp32_t *gp32, const char *path);

gp32_status_t gp32_reset(gp32_t *gp32);
gp32_status_t gp32_run_cycles(gp32_t *gp32, uint32_t cycles);
gp32_status_t gp32_set_jit(gp32_t *gp32, int enabled);
/* Overrides HLE playback rate for raw SEF PCM. 0 restores SDK-derived auto rate. */
gp32_status_t gp32_set_hle_sef_rate(gp32_t *gp32, uint32_t sample_rate_hz);

gp32_status_t gp32_set_buttons(gp32_t *gp32, uint32_t gp32_button_mask);
gp32_status_t gp32_get_framebuffer(gp32_t *gp32, gp32_framebuffer_desc_t *out_desc);
gp32_status_t gp32_get_audio(gp32_t *gp32, gp32_audio_desc_t *out_desc);
gp32_status_t gp32_clear_audio(gp32_t *gp32);

uint32_t gp32_get_pc(const gp32_t *gp32);
uint32_t gp32_get_cpu_reg(const gp32_t *gp32, unsigned reg);
uint32_t gp32_get_cpsr(const gp32_t *gp32);
uint32_t gp32_get_cp15(const gp32_t *gp32, unsigned reg);
uint32_t gp32_debug_read32(gp32_t *gp32, uint32_t addr);
uint64_t gp32_get_cycles(const gp32_t *gp32);
uint32_t gp32_get_fclk_hz(const gp32_t *gp32);
/* Effective instruction budget clock used by frontends for real-time pacing. */
uint32_t gp32_get_run_clock_hz(const gp32_t *gp32);
uint64_t gp32_get_jit_hits(const gp32_t *gp32);
uint64_t gp32_get_jit_misses(const gp32_t *gp32);
uint64_t gp32_get_jit_fallbacks(const gp32_t *gp32);
const char *gp32_get_error(const gp32_t *gp32);

#ifdef __cplusplus
}
#endif

#endif /* GP32EMU_GP32_H */
