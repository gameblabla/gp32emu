#ifndef GP32EMU_SDL3_INPUT_SHARED_H
#define GP32EMU_SDL3_INPUT_SHARED_H

#include "gp32emu/gp32.h"
#include "gp32emu/platform.h"

#define SDL_MAIN_HANDLED 1
#include <SDL3/SDL.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gp32_sdl3_input_state {
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
    uint32_t pending_actions;
    char error[160];
} gp32_sdl3_input_state_t;

void gp32_sdl3_input_state_init(gp32_sdl3_input_state_t *s, int enable_joystick, int enable_joystick_axis);
void gp32_sdl3_input_state_shutdown(gp32_sdl3_input_state_t *s);
void gp32_sdl3_input_open_first_joystick(gp32_sdl3_input_state_t *s);
void gp32_sdl3_input_handle_event(gp32_sdl3_input_state_t *s, const SDL_Event *ev, int *quit_requested);
uint32_t gp32_sdl3_input_keyboard_buttons(void);
uint32_t gp32_sdl3_input_joystick_buttons(gp32_sdl3_input_state_t *s);
uint32_t gp32_sdl3_input_take_actions(gp32_sdl3_input_state_t *s);
gp32_status_t gp32_sdl3_input_poll(gp32_sdl3_input_state_t *s, int include_keyboard, uint32_t extra_buttons, uint32_t *out_buttons, int *out_quit);
const char *gp32_sdl3_input_error(const gp32_sdl3_input_state_t *s);

#ifdef __cplusplus
}
#endif

#endif /* GP32EMU_SDL3_INPUT_SHARED_H */
