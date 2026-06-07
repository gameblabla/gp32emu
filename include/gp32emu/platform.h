/*
 * gp32emu platform backend API.
 *
 * These interfaces are intentionally separate from gp32_t so that the core
 * emulator stays headless.  Each backend is an opaque handle owned by the
 * caller; SDL 1.2 and SDL3 are optional implementations.
 */
#ifndef GP32EMU_PLATFORM_H
#define GP32EMU_PLATFORM_H

#include "gp32emu/gp32.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gp32_video_backend gp32_video_backend_t;
typedef struct gp32_audio_backend gp32_audio_backend_t;
typedef struct gp32_input_backend gp32_input_backend_t;

typedef struct gp32_video_options {
    const char *title;      /* NULL => default title */
    uint32_t scale;         /* 0 => 2 */
    int fullscreen;         /* non-zero => request fullscreen */
    int rotate;             /* 0 none, 1 CCW, 2 180, 3 CW */
    int lcd_persistence;    /* non-zero => temporal LCD persistence effect; off by default */
    int frame_interpolation;/* non-zero => blend adjacent frames for smoother pixel motion; off by default */
} gp32_video_options_t;

typedef struct gp32_audio_options {
    uint32_t sample_rate_hz; /* 0 => 44100 */
    uint32_t buffer_frames;  /* 0 => 2048 */
} gp32_audio_options_t;

typedef enum gp32_frontend_action {
    GP32_FRONTEND_ACTION_NONE = 0u,
    GP32_FRONTEND_ACTION_SAVE_STATE = 1u << 0,
    GP32_FRONTEND_ACTION_LOAD_STATE = 1u << 1
} gp32_frontend_action_t;

typedef struct gp32_input_options {
    int enable_joystick;       /* non-zero => open first joystick when available */
    int enable_joystick_axis;  /* non-zero => map analog axes 0/1 to GP32 d-pad; off by default to avoid stick drift */
} gp32_input_options_t;

void gp32_video_destroy(gp32_video_backend_t *backend);
gp32_status_t gp32_video_present(gp32_video_backend_t *backend, const gp32_framebuffer_desc_t *framebuffer);
gp32_status_t gp32_video_set_title(gp32_video_backend_t *backend, const char *title);
const char *gp32_video_error(const gp32_video_backend_t *backend);

void gp32_audio_destroy(gp32_audio_backend_t *backend);
gp32_status_t gp32_audio_submit(gp32_audio_backend_t *backend, const gp32_audio_desc_t *audio);
uint32_t gp32_audio_buffered_frames(gp32_audio_backend_t *backend);
const char *gp32_audio_error(const gp32_audio_backend_t *backend);

void gp32_input_destroy(gp32_input_backend_t *backend);
gp32_status_t gp32_input_poll(gp32_input_backend_t *backend, uint32_t *out_buttons, int *out_quit);
uint32_t gp32_input_take_actions(gp32_input_backend_t *backend);
const char *gp32_input_error(const gp32_input_backend_t *backend);

/* SDL 1.2 backend constructors. Link with libgp32emu_sdl12.a / gp32_sdl12. */
gp32_video_backend_t *gp32_video_sdl12_create(const gp32_video_options_t *options);
gp32_audio_backend_t *gp32_audio_sdl12_create(const gp32_audio_options_t *options);
gp32_input_backend_t *gp32_input_sdl12_create(const gp32_input_options_t *options);

/* SDL3 backend constructors. Link with libgp32emu_sdl3.a / gp32_sdl3. */
gp32_video_backend_t *gp32_video_sdl3_create(const gp32_video_options_t *options);
gp32_audio_backend_t *gp32_audio_sdl3_create(const gp32_audio_options_t *options);
gp32_input_backend_t *gp32_input_sdl3_create(const gp32_input_options_t *options);

#ifdef __cplusplus
}
#endif

#endif /* GP32EMU_PLATFORM_H */
