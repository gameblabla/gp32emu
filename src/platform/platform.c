#include "platform_internal.h"

void gp32_video_destroy(gp32_video_backend_t *backend) {
    if (backend && backend->destroy) backend->destroy(backend);
}

gp32_status_t gp32_video_present(gp32_video_backend_t *backend, const gp32_framebuffer_desc_t *framebuffer) {
    if (!backend || !backend->present || !framebuffer) return GP32_ERR_INVALID_ARGUMENT;
    return backend->present(backend, framebuffer);
}

gp32_status_t gp32_video_set_title(gp32_video_backend_t *backend, const char *title) {
    if (!backend || !backend->set_title) return GP32_ERR_INVALID_ARGUMENT;
    return backend->set_title(backend, title);
}

const char *gp32_video_error(const gp32_video_backend_t *backend) {
    if (!backend || !backend->error) return "video backend unavailable";
    return backend->error(backend);
}

void gp32_audio_destroy(gp32_audio_backend_t *backend) {
    if (backend && backend->destroy) backend->destroy(backend);
}

gp32_status_t gp32_audio_submit(gp32_audio_backend_t *backend, const gp32_audio_desc_t *audio) {
    if (!backend || !backend->submit || !audio) return GP32_ERR_INVALID_ARGUMENT;
    return backend->submit(backend, audio);
}

uint32_t gp32_audio_buffered_frames(gp32_audio_backend_t *backend) {
    if (!backend || !backend->buffered_frames) return 0;
    return backend->buffered_frames(backend);
}

const char *gp32_audio_error(const gp32_audio_backend_t *backend) {
    if (!backend || !backend->error) return "audio backend unavailable";
    return backend->error(backend);
}

void gp32_input_destroy(gp32_input_backend_t *backend) {
    if (backend && backend->destroy) backend->destroy(backend);
}

gp32_status_t gp32_input_poll(gp32_input_backend_t *backend, uint32_t *out_buttons, int *out_quit) {
    if (!backend || !backend->poll || !out_buttons || !out_quit) return GP32_ERR_INVALID_ARGUMENT;
    return backend->poll(backend, out_buttons, out_quit);
}

uint32_t gp32_input_take_actions(gp32_input_backend_t *backend) {
    if (!backend || !backend->take_actions) return 0u;
    return backend->take_actions(backend);
}

const char *gp32_input_error(const gp32_input_backend_t *backend) {
    if (!backend || !backend->error) return "input backend unavailable";
    return backend->error(backend);
}
