#include "platform/sdl12/common.h"

static int sdl_initialized;
static unsigned video_refs;
static unsigned audio_refs;
static unsigned joystick_refs;

static int ensure_sdl_base(void) {
    if (!sdl_initialized) {
        if (SDL_Init(0) != 0) return -1;
        sdl_initialized = 1;
    }
    return 0;
}

static int init_one(Uint32 flag, unsigned *refs) {
    if ((*refs)++ == 0u) {
        if (SDL_InitSubSystem(flag) != 0) {
            --(*refs);
            return -1;
        }
    }
    return 0;
}

int gp32_sdl12_init_subsystem(Uint32 flags) {
    if (ensure_sdl_base() != 0) return -1;
    if ((flags & SDL_INIT_VIDEO) && init_one(SDL_INIT_VIDEO, &video_refs) != 0) return -1;
    if ((flags & SDL_INIT_AUDIO) && init_one(SDL_INIT_AUDIO, &audio_refs) != 0) return -1;
    if ((flags & SDL_INIT_JOYSTICK) && init_one(SDL_INIT_JOYSTICK, &joystick_refs) != 0) return -1;
    return 0;
}

static void quit_one(Uint32 flag, unsigned *refs) {
    if (*refs == 0u) return;
    --(*refs);
    if (*refs == 0u) SDL_QuitSubSystem(flag);
}

void gp32_sdl12_quit_subsystem(Uint32 flags) {
    if (flags & SDL_INIT_JOYSTICK) quit_one(SDL_INIT_JOYSTICK, &joystick_refs);
    if (flags & SDL_INIT_AUDIO) quit_one(SDL_INIT_AUDIO, &audio_refs);
    if (flags & SDL_INIT_VIDEO) quit_one(SDL_INIT_VIDEO, &video_refs);
    if (sdl_initialized && video_refs == 0u && audio_refs == 0u && joystick_refs == 0u) {
        SDL_Quit();
        sdl_initialized = 0;
    }
}
