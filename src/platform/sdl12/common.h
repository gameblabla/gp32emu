#ifndef GP32EMU_SDL12_COMMON_H
#define GP32EMU_SDL12_COMMON_H

#if defined(__APPLE__)
#include <SDL/SDL.h>
#else
#include <SDL.h>
#endif

int gp32_sdl12_init_subsystem(Uint32 flags);
void gp32_sdl12_quit_subsystem(Uint32 flags);

#endif /* GP32EMU_SDL12_COMMON_H */
