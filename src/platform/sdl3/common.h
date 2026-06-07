#ifndef GP32EMU_SDL3_COMMON_H
#define GP32EMU_SDL3_COMMON_H

#include <SDL3/SDL.h>

int gp32_sdl3_init_subsystem(SDL_InitFlags flags);
void gp32_sdl3_quit_subsystem(SDL_InitFlags flags);

#endif /* GP32EMU_SDL3_COMMON_H */
