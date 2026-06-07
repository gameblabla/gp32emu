#ifndef GP32EMU_PLATFORM_INTERNAL_H
#define GP32EMU_PLATFORM_INTERNAL_H

#include "gp32emu/platform.h"
#include <stdio.h>

struct gp32_video_backend {
    void (*destroy)(gp32_video_backend_t *backend);
    gp32_status_t (*present)(gp32_video_backend_t *backend, const gp32_framebuffer_desc_t *framebuffer);
    gp32_status_t (*set_title)(gp32_video_backend_t *backend, const char *title);
    const char *(*error)(const gp32_video_backend_t *backend);
};

struct gp32_audio_backend {
    void (*destroy)(gp32_audio_backend_t *backend);
    gp32_status_t (*submit)(gp32_audio_backend_t *backend, const gp32_audio_desc_t *audio);
    uint32_t (*buffered_frames)(gp32_audio_backend_t *backend);
    const char *(*error)(const gp32_audio_backend_t *backend);
};

struct gp32_input_backend {
    void (*destroy)(gp32_input_backend_t *backend);
    gp32_status_t (*poll)(gp32_input_backend_t *backend, uint32_t *out_buttons, int *out_quit);
    uint32_t (*take_actions)(gp32_input_backend_t *backend);
    const char *(*error)(const gp32_input_backend_t *backend);
};

static inline void gp32_platform_set_error(char *dst, size_t dst_size, const char *msg) {
    if (!dst || dst_size == 0) return;
    if (!msg) msg = "platform backend error";
    snprintf(dst, dst_size, "%s", msg);
}

#endif /* GP32EMU_PLATFORM_INTERNAL_H */
