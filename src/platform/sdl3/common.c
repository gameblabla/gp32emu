#include "platform/sdl3/common.h"

static unsigned video_refs;
static unsigned audio_refs;
static unsigned joystick_refs;

static int init_one(SDL_InitFlags flag, unsigned *refs) {
    if ((*refs)++ == 0u) {
        if (!SDL_InitSubSystem(flag)) {
            --(*refs);
            return -1;
        }
    }
    return 0;
}

int gp32_sdl3_init_subsystem(SDL_InitFlags flags) {
    if ((flags & SDL_INIT_VIDEO) && init_one(SDL_INIT_VIDEO, &video_refs) != 0) return -1;
    if ((flags & SDL_INIT_AUDIO) && init_one(SDL_INIT_AUDIO, &audio_refs) != 0) return -1;
    if ((flags & SDL_INIT_JOYSTICK) && init_one(SDL_INIT_JOYSTICK, &joystick_refs) != 0) return -1;
    return 0;
}

static void quit_one(SDL_InitFlags flag, unsigned *refs) {
    if (*refs == 0u) return;
    --(*refs);
    if (*refs == 0u) SDL_QuitSubSystem(flag);
}

void gp32_sdl3_quit_subsystem(SDL_InitFlags flags) {
    if (flags & SDL_INIT_JOYSTICK) quit_one(SDL_INIT_JOYSTICK, &joystick_refs);
    if (flags & SDL_INIT_AUDIO) quit_one(SDL_INIT_AUDIO, &audio_refs);
    if (flags & SDL_INIT_VIDEO) quit_one(SDL_INIT_VIDEO, &video_refs);
    if (video_refs == 0u && audio_refs == 0u && joystick_refs == 0u) SDL_Quit();
}
