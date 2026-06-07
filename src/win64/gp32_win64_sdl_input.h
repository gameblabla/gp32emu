#ifndef GP32EMU_WIN64_SDL_INPUT_H
#define GP32EMU_WIN64_SDL_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gp32_win64_sdl_input gp32_win64_sdl_input_t;

gp32_win64_sdl_input_t *gp32_win64_sdl_input_create(int enable_joystick_axis);
void gp32_win64_sdl_input_destroy(gp32_win64_sdl_input_t *s);
int gp32_win64_sdl_input_poll(gp32_win64_sdl_input_t *s, uint32_t keyboard_buttons, uint32_t *out_buttons, uint32_t *out_actions, int *quit_requested);
const char *gp32_win64_sdl_input_status(const gp32_win64_sdl_input_t *s);

#ifdef __cplusplus
}
#endif

#endif /* GP32EMU_WIN64_SDL_INPUT_H */
