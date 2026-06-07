#include "platform/platform_internal.h"
#include "platform/sdl3/common.h"
#include "input/gp32_sdl3_input_shared.h"

#include <stdlib.h>
#include <string.h>

typedef struct sdl3_input_backend {
    gp32_input_backend_t base;
    gp32_sdl3_input_state_t shared;
    int enable_joystick;
} sdl3_input_backend_t;

static uint32_t sdl3_input_take_actions(gp32_input_backend_t *backend) {
    sdl3_input_backend_t *in = (sdl3_input_backend_t *)backend;
    return in ? gp32_sdl3_input_take_actions(&in->shared) : 0u;
}

static gp32_status_t sdl3_input_poll(gp32_input_backend_t *backend, uint32_t *out_buttons, int *out_quit) {
    sdl3_input_backend_t *in = (sdl3_input_backend_t *)backend;
    if (!in) return GP32_ERR_INVALID_ARGUMENT;
    return gp32_sdl3_input_poll(&in->shared, 1, 0u, out_buttons, out_quit);
}

static const char *sdl3_input_error(const gp32_input_backend_t *backend) {
    const sdl3_input_backend_t *in = (const sdl3_input_backend_t *)backend;
    return in ? gp32_sdl3_input_error(&in->shared) : "";
}

static void sdl3_input_destroy(gp32_input_backend_t *backend) {
    sdl3_input_backend_t *in = (sdl3_input_backend_t *)backend;
    if (!in) return;
    gp32_sdl3_input_state_shutdown(&in->shared);
    if (in->enable_joystick) gp32_sdl3_quit_subsystem(SDL_INIT_JOYSTICK);
    gp32_sdl3_quit_subsystem(SDL_INIT_VIDEO);
    free(in);
}

gp32_input_backend_t *gp32_input_sdl3_create(const gp32_input_options_t *options) {
    SDL_InitFlags flags = SDL_INIT_VIDEO;
    int use_joy = options ? options->enable_joystick : 0;
    int use_joy_axis = options ? options->enable_joystick_axis : 0;
    if (use_joy) flags |= SDL_INIT_JOYSTICK;
    if (gp32_sdl3_init_subsystem(flags) != 0) return NULL;
    sdl3_input_backend_t *in = (sdl3_input_backend_t *)calloc(1, sizeof(*in));
    if (!in) { if (use_joy) gp32_sdl3_quit_subsystem(SDL_INIT_JOYSTICK); gp32_sdl3_quit_subsystem(SDL_INIT_VIDEO); return NULL; }
    in->base.destroy = sdl3_input_destroy;
    in->base.poll = sdl3_input_poll;
    in->base.take_actions = sdl3_input_take_actions;
    in->base.error = sdl3_input_error;
    in->enable_joystick = use_joy;
    gp32_sdl3_input_state_init(&in->shared, use_joy, use_joy_axis);
    if (use_joy) gp32_sdl3_input_open_first_joystick(&in->shared);
    return &in->base;
}
