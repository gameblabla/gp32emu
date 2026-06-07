#include "input/gp32_sdl3_input_shared.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void set_error(gp32_sdl3_input_state_t *s, const char *msg) {
    if (!s) return;
    if (!msg) msg = "SDL3 input error";
    snprintf(s->error, sizeof(s->error), "%s", msg);
}

void gp32_sdl3_input_state_init(gp32_sdl3_input_state_t *s, int enable_joystick, int enable_joystick_axis) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->enable_joystick = enable_joystick != 0;
    s->enable_joystick_axis = enable_joystick_axis != 0;
}

void gp32_sdl3_input_state_shutdown(gp32_sdl3_input_state_t *s) {
    if (!s) return;
    if (s->joy) SDL_CloseJoystick(s->joy);
    s->joy = NULL;
}

void gp32_sdl3_input_open_first_joystick(gp32_sdl3_input_state_t *s) {
    if (!s || !s->enable_joystick || s->joy) return;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids && count > 0) {
        s->joy = SDL_OpenJoystick(ids[0]);
        if (s->joy) {
            SDL_UpdateJoysticks();
            s->num_axes = SDL_GetNumJoystickAxes(s->joy);
            s->num_buttons = SDL_GetNumJoystickButtons(s->joy);
            s->num_hats = SDL_GetNumJoystickHats(s->joy);
            for (int i = 0; i < 2; ++i) {
                if (i < s->num_axes) {
                    int raw = (int)SDL_GetJoystickAxis(s->joy, i);
                    if (raw > -8000 && raw < 8000) s->axis_centered[i] = 1;
                }
            }
            s->error[0] = 0;
        } else set_error(s, SDL_GetError());
    } else set_error(s, "no SDL joystick available");
    if (ids) SDL_free(ids);
}

static void close_joystick_id(gp32_sdl3_input_state_t *s, SDL_JoystickID id) {
    if (!s || !s->joy) return;
    if (SDL_GetJoystickID(s->joy) == id) {
        SDL_CloseJoystick(s->joy);
        s->joy = NULL;
        s->num_axes = s->num_buttons = s->num_hats = 0;
        memset(s->axis_centered, 0, sizeof(s->axis_centered));
        memset(s->axis_state, 0, sizeof(s->axis_state));
        memset(s->axis_pending, 0, sizeof(s->axis_pending));
        memset(s->axis_pending_count, 0, sizeof(s->axis_pending_count));
    }
}

void gp32_sdl3_input_handle_event(gp32_sdl3_input_state_t *s, const SDL_Event *ev, int *quit_requested) {
    if (!ev) return;
    switch (ev->type) {
    case SDL_EVENT_QUIT:
        if (quit_requested) *quit_requested = 1;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (!ev->key.repeat) {
            if (ev->key.key == SDLK_ESCAPE) { if (quit_requested) *quit_requested = 1; }
            else if (ev->key.key == SDLK_F5 && s) s->pending_actions |= GP32_FRONTEND_ACTION_SAVE_STATE;
            else if (ev->key.key == SDLK_F8 && s) s->pending_actions |= GP32_FRONTEND_ACTION_LOAD_STATE;
        }
        break;
    case SDL_EVENT_JOYSTICK_ADDED:
        if (s && s->enable_joystick && !s->joy) {
            s->joy = SDL_OpenJoystick(ev->jdevice.which);
            if (s->joy) {
                s->num_axes = SDL_GetNumJoystickAxes(s->joy);
                s->num_buttons = SDL_GetNumJoystickButtons(s->joy);
                s->num_hats = SDL_GetNumJoystickHats(s->joy);
                s->error[0] = 0;
            } else set_error(s, SDL_GetError());
        }
        break;
    case SDL_EVENT_JOYSTICK_REMOVED:
        close_joystick_id(s, ev->jdevice.which);
        break;
    default:
        break;
    }
}

uint32_t gp32_sdl3_input_keyboard_buttons(void) {
    uint32_t mask = 0;
    const bool *keys = SDL_GetKeyboardState(NULL);
    if (!keys) return 0;
    if (keys[SDL_SCANCODE_LEFT])   mask |= GP32_BUTTON_LEFT;
    if (keys[SDL_SCANCODE_RIGHT])  mask |= GP32_BUTTON_RIGHT;
    if (keys[SDL_SCANCODE_UP])     mask |= GP32_BUTTON_UP;
    if (keys[SDL_SCANCODE_DOWN])   mask |= GP32_BUTTON_DOWN;
    if (keys[SDL_SCANCODE_Z])      mask |= GP32_BUTTON_A;
    if (keys[SDL_SCANCODE_X])      mask |= GP32_BUTTON_B;
    if (keys[SDL_SCANCODE_A])      mask |= GP32_BUTTON_L;
    if (keys[SDL_SCANCODE_S])      mask |= GP32_BUTTON_R;
    if (keys[SDL_SCANCODE_RETURN]) mask |= GP32_BUTTON_START;
    if (keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_LSHIFT]) mask |= GP32_BUTTON_SELECT;
    return mask;
}

static int joy_button(const gp32_sdl3_input_state_t *s, int index) {
    return s && s->joy && index >= 0 && index < s->num_buttons && SDL_GetJoystickButton(s->joy, index);
}

static int filtered_axis_dir(gp32_sdl3_input_state_t *s, int axis) {
    const int center = 8000;
    const int press = 24000;
    const int release = 18000;
    int raw = 0;
    int next = 0;
    if (!s || !s->joy || axis < 0 || axis >= 2 || axis >= s->num_axes) return 0;
    raw = (int)SDL_GetJoystickAxis(s->joy, axis);
    if (!s->axis_centered[axis]) {
        if (raw > -center && raw < center) s->axis_centered[axis] = 1;
        else { s->axis_state[axis] = 0; s->axis_pending[axis] = 0; s->axis_pending_count[axis] = 0; return 0; }
    }
    if (s->axis_state[axis] < 0) next = (raw < -release) ? -1 : 0;
    else if (s->axis_state[axis] > 0) next = (raw > release) ? 1 : 0;
    else if (raw < -press) next = -1;
    else if (raw > press) next = 1;
    if (next != s->axis_state[axis]) {
        if (next == s->axis_pending[axis]) ++s->axis_pending_count[axis];
        else { s->axis_pending[axis] = next; s->axis_pending_count[axis] = 1; }
        if (s->axis_pending_count[axis] >= 2u) { s->axis_state[axis] = next; s->axis_pending_count[axis] = 0; }
    } else { s->axis_pending[axis] = next; s->axis_pending_count[axis] = 0; }
    return s->axis_state[axis];
}

uint32_t gp32_sdl3_input_joystick_buttons(gp32_sdl3_input_state_t *s) {
    uint32_t mask = 0;
    if (!s || !s->joy) return 0;
    if (s->num_hats > 0) {
        Uint8 hat = SDL_GetJoystickHat(s->joy, 0);
        if (hat & SDL_HAT_LEFT)  mask |= GP32_BUTTON_LEFT;
        if (hat & SDL_HAT_RIGHT) mask |= GP32_BUTTON_RIGHT;
        if (hat & SDL_HAT_UP)    mask |= GP32_BUTTON_UP;
        if (hat & SDL_HAT_DOWN)  mask |= GP32_BUTTON_DOWN;
    }
    if (s->enable_joystick_axis) {
        int ax0 = filtered_axis_dir(s, 0);
        int ax1 = filtered_axis_dir(s, 1);
        if (ax0 < 0) mask |= GP32_BUTTON_LEFT;
        if (ax0 > 0) mask |= GP32_BUTTON_RIGHT;
        if (ax1 < 0) mask |= GP32_BUTTON_UP;
        if (ax1 > 0) mask |= GP32_BUTTON_DOWN;
    }
    if (joy_button(s, 0)) mask |= GP32_BUTTON_A;
    if (joy_button(s, 1)) mask |= GP32_BUTTON_B;
    if (joy_button(s, 4)) mask |= GP32_BUTTON_L;
    if (joy_button(s, 5)) mask |= GP32_BUTTON_R;
    if (joy_button(s, 7)) mask |= GP32_BUTTON_START;
    if (joy_button(s, 6)) mask |= GP32_BUTTON_SELECT;
    return mask;
}

uint32_t gp32_sdl3_input_take_actions(gp32_sdl3_input_state_t *s) {
    if (!s) return 0u;
    uint32_t a = s->pending_actions;
    s->pending_actions = 0u;
    return a;
}

gp32_status_t gp32_sdl3_input_poll(gp32_sdl3_input_state_t *s, int include_keyboard, uint32_t extra_buttons, uint32_t *out_buttons, int *out_quit) {
    if (!s || !out_buttons || !out_quit) return GP32_ERR_INVALID_ARGUMENT;
    int quit = 0;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) gp32_sdl3_input_handle_event(s, &ev, &quit);
    SDL_PumpEvents();
    if (s->joy) SDL_UpdateJoysticks();
    uint32_t mask = extra_buttons;
    if (include_keyboard) mask |= gp32_sdl3_input_keyboard_buttons();
    mask |= gp32_sdl3_input_joystick_buttons(s);
    *out_buttons = mask;
    *out_quit = quit;
    return GP32_OK;
}

const char *gp32_sdl3_input_error(const gp32_sdl3_input_state_t *s) {
    return (s && s->error[0]) ? s->error : "";
}
