#include "platform/platform_internal.h"
#include "platform/sdl12/common.h"

#include <stdlib.h>
#include <string.h>

typedef struct sdl12_input_backend {
    gp32_input_backend_t base;
    SDL_Joystick *joy;
    int enable_joystick;
    int enable_joystick_axis;
    int num_axes;
    int num_buttons;
    int num_hats;
    int axis_centered[2];
    int axis_state[2];
    int axis_pending[2];
    unsigned axis_pending_count[2];
    uint32_t last_buttons;
    uint32_t pending_actions;
    char error[160];
} sdl12_input_backend_t;

static void add_key_buttons(uint8_t *keys, uint32_t *mask) {
    if (keys[SDLK_LEFT])   *mask |= GP32_BUTTON_LEFT;
    if (keys[SDLK_RIGHT])  *mask |= GP32_BUTTON_RIGHT;
    if (keys[SDLK_UP])     *mask |= GP32_BUTTON_UP;
    if (keys[SDLK_DOWN])   *mask |= GP32_BUTTON_DOWN;

    if (keys[SDLK_z])      *mask |= GP32_BUTTON_A;
    if (keys[SDLK_x])      *mask |= GP32_BUTTON_B;
    if (keys[SDLK_a])      *mask |= GP32_BUTTON_L;
    if (keys[SDLK_s])      *mask |= GP32_BUTTON_R;
    if (keys[SDLK_RETURN]) *mask |= GP32_BUTTON_START;
    if (keys[SDLK_RSHIFT] || keys[SDLK_LSHIFT]) *mask |= GP32_BUTTON_SELECT;
}

static int joy_button(const sdl12_input_backend_t *in, int index) {
    return in && in->joy && index >= 0 && index < in->num_buttons && SDL_JoystickGetButton(in->joy, index) != 0;
}

static void add_joystick_hat_buttons(sdl12_input_backend_t *in, uint32_t *mask) {
    if (!in || !in->joy || in->num_hats <= 0) return;
    Uint8 hat = SDL_JoystickGetHat(in->joy, 0);
    if (hat & SDL_HAT_LEFT)  *mask |= GP32_BUTTON_LEFT;
    if (hat & SDL_HAT_RIGHT) *mask |= GP32_BUTTON_RIGHT;
    if (hat & SDL_HAT_UP)    *mask |= GP32_BUTTON_UP;
    if (hat & SDL_HAT_DOWN)  *mask |= GP32_BUTTON_DOWN;
}

static int filtered_axis_dir(sdl12_input_backend_t *in, int axis) {
    const int center = 8000;
    const int press = 24000;
    const int release = 18000;
    int raw = 0;
    int next = 0;
    if (!in || !in->joy || axis < 0 || axis >= 2 || axis >= in->num_axes) return 0;

    raw = (int)SDL_JoystickGetAxis(in->joy, axis);
    if (!in->axis_centered[axis]) {
        if (raw > -center && raw < center) in->axis_centered[axis] = 1;
        else {
            in->axis_state[axis] = 0;
            in->axis_pending[axis] = 0;
            in->axis_pending_count[axis] = 0;
            return 0;
        }
    }

    if (in->axis_state[axis] < 0) next = (raw < -release) ? -1 : 0;
    else if (in->axis_state[axis] > 0) next = (raw > release) ? 1 : 0;
    else if (raw < -press) next = -1;
    else if (raw > press) next = 1;

    if (next != in->axis_state[axis]) {
        if (next == in->axis_pending[axis]) ++in->axis_pending_count[axis];
        else {
            in->axis_pending[axis] = next;
            in->axis_pending_count[axis] = 1;
        }
        if (in->axis_pending_count[axis] >= 2u) {
            in->axis_state[axis] = next;
            in->axis_pending_count[axis] = 0;
        }
    } else {
        in->axis_pending[axis] = next;
        in->axis_pending_count[axis] = 0;
    }
    return in->axis_state[axis];
}

static void add_joystick_axis_buttons(sdl12_input_backend_t *in, uint32_t *mask) {
    if (!in || !in->enable_joystick_axis) return;
    int ax0 = filtered_axis_dir(in, 0);
    int ax1 = filtered_axis_dir(in, 1);
    if (ax0 < 0) *mask |= GP32_BUTTON_LEFT;
    if (ax0 > 0) *mask |= GP32_BUTTON_RIGHT;
    if (ax1 < 0) *mask |= GP32_BUTTON_UP;
    if (ax1 > 0) *mask |= GP32_BUTTON_DOWN;
}

static void add_joystick_buttons(sdl12_input_backend_t *in, uint32_t *mask) {
    if (!in || !in->joy) return;

    add_joystick_hat_buttons(in, mask);
    add_joystick_axis_buttons(in, mask);

    if (joy_button(in, 0)) *mask |= GP32_BUTTON_A;
    if (joy_button(in, 1)) *mask |= GP32_BUTTON_B;
    if (joy_button(in, 4)) *mask |= GP32_BUTTON_L;
    if (joy_button(in, 5)) *mask |= GP32_BUTTON_R;
    if (joy_button(in, 7)) *mask |= GP32_BUTTON_START;
    if (joy_button(in, 6)) *mask |= GP32_BUTTON_SELECT;
}

static uint32_t sdl12_input_take_actions(gp32_input_backend_t *backend) {
    sdl12_input_backend_t *in = (sdl12_input_backend_t *)backend;
    if (!in) return 0u;
    uint32_t actions = in->pending_actions;
    in->pending_actions = 0u;
    return actions;
}

static gp32_status_t sdl12_input_poll(gp32_input_backend_t *backend, uint32_t *out_buttons, int *out_quit) {
    sdl12_input_backend_t *in = (sdl12_input_backend_t *)backend;
    if (!in || !out_buttons || !out_quit) return GP32_ERR_INVALID_ARGUMENT;
    SDL_Event ev;
    int quit = 0;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) quit = 1;
        if (ev.type == SDL_KEYDOWN) {
            if (ev.key.keysym.sym == SDLK_ESCAPE) quit = 1;
            else if (ev.key.keysym.sym == SDLK_F5) in->pending_actions |= GP32_FRONTEND_ACTION_SAVE_STATE;
            else if (ev.key.keysym.sym == SDLK_F8) in->pending_actions |= GP32_FRONTEND_ACTION_LOAD_STATE;
        }
    }
    SDL_PumpEvents();
    if (in->joy) SDL_JoystickUpdate();
    uint32_t mask = 0;
    uint8_t *keys = SDL_GetKeyState(NULL);
    if (keys) add_key_buttons(keys, &mask);
    add_joystick_buttons(in, &mask);
    in->last_buttons = mask;
    *out_buttons = mask;
    *out_quit = quit;
    return GP32_OK;
}

static const char *sdl12_input_error(const gp32_input_backend_t *backend) {
    const sdl12_input_backend_t *in = (const sdl12_input_backend_t *)backend;
    return (in && in->error[0]) ? in->error : "";
}

static void sdl12_input_destroy(gp32_input_backend_t *backend) {
    sdl12_input_backend_t *in = (sdl12_input_backend_t *)backend;
    if (!in) return;
    if (in->joy) SDL_JoystickClose(in->joy);
    if (in->enable_joystick) gp32_sdl12_quit_subsystem(SDL_INIT_JOYSTICK);
    gp32_sdl12_quit_subsystem(SDL_INIT_VIDEO);
    free(in);
}

gp32_input_backend_t *gp32_input_sdl12_create(const gp32_input_options_t *options) {
    Uint32 flags = SDL_INIT_VIDEO;
    int use_joy = options ? options->enable_joystick : 0;
    int use_joy_axis = options ? options->enable_joystick_axis : 0;
    if (use_joy) flags |= SDL_INIT_JOYSTICK;
    if (gp32_sdl12_init_subsystem(flags) != 0) return NULL;
    sdl12_input_backend_t *in = (sdl12_input_backend_t *)calloc(1, sizeof(*in));
    if (!in) {
        if (use_joy) gp32_sdl12_quit_subsystem(SDL_INIT_JOYSTICK);
        gp32_sdl12_quit_subsystem(SDL_INIT_VIDEO);
        return NULL;
    }
    in->base.destroy = sdl12_input_destroy;
    in->base.poll = sdl12_input_poll;
    in->base.take_actions = sdl12_input_take_actions;
    in->base.error = sdl12_input_error;
    in->enable_joystick = use_joy;
    in->enable_joystick_axis = use_joy_axis;
    if (use_joy) {
        SDL_JoystickEventState(SDL_ENABLE);
        if (SDL_NumJoysticks() > 0) {
            in->joy = SDL_JoystickOpen(0);
            if (in->joy) {
                SDL_JoystickUpdate();
                in->num_axes = SDL_JoystickNumAxes(in->joy);
                in->num_buttons = SDL_JoystickNumButtons(in->joy);
                in->num_hats = SDL_JoystickNumHats(in->joy);
                for (int i = 0; i < 2; ++i) {
                    if (i < in->num_axes) {
                        int raw = (int)SDL_JoystickGetAxis(in->joy, i);
                        if (raw > -8000 && raw < 8000) in->axis_centered[i] = 1;
                    }
                }
            } else {
                gp32_platform_set_error(in->error, sizeof(in->error), SDL_GetError());
            }
        } else {
            gp32_platform_set_error(in->error, sizeof(in->error), "no SDL joystick available");
        }
    }
    return &in->base;
}
