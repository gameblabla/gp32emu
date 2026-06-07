#ifndef GP32EMU_WIN64_AUDIO_H
#define GP32EMU_WIN64_AUDIO_H

#include "gp32emu/gp32.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gp32_win64_audio gp32_win64_audio_t;

typedef enum gp32_win64_audio_mode {
    GP32_WIN64_AUDIO_WAVEOUT = 0,
    GP32_WIN64_AUDIO_WASAPI_SHARED = 1,
    GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE = 2
} gp32_win64_audio_mode_t;

gp32_win64_audio_t *gp32_win64_audio_create(gp32_win64_audio_mode_t mode, uint32_t sample_rate_hz);
void gp32_win64_audio_destroy(gp32_win64_audio_t *a);
int gp32_win64_audio_submit(gp32_win64_audio_t *a, const gp32_audio_desc_t *audio);
int gp32_win64_audio_pump(gp32_win64_audio_t *a);
void gp32_win64_audio_reset(gp32_win64_audio_t *a);
uint32_t gp32_win64_audio_buffered_frames(const gp32_win64_audio_t *a);
const char *gp32_win64_audio_backend(const gp32_win64_audio_t *a);
const char *gp32_win64_audio_error(const gp32_win64_audio_t *a);

#ifdef __cplusplus
}
#endif

#endif /* GP32EMU_WIN64_AUDIO_H */
