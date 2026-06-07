#ifndef GP32EMU_INPUT_SCRIPT_H
#define GP32EMU_INPUT_SCRIPT_H

#include "gp32emu/gp32.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gp32_input_script gp32_input_script_t;
typedef struct gp32_input_recorder gp32_input_recorder_t;

int gp32_input_script_load(const char *path, gp32_input_script_t **out_script, char *err, size_t err_len);
void gp32_input_script_destroy(gp32_input_script_t *script);
uint32_t gp32_input_script_frame(gp32_input_script_t *script, uint64_t frame_index);
const char *gp32_input_button_names(uint32_t mask, char *buf, size_t buf_len);

gp32_input_recorder_t *gp32_input_recorder_open(const char *path, char *err, size_t err_len);
void gp32_input_recorder_sample(gp32_input_recorder_t *rec, uint64_t frame_index, uint32_t mask);
void gp32_input_recorder_close(gp32_input_recorder_t *rec);

#ifdef __cplusplus
}
#endif

#endif /* GP32EMU_INPUT_SCRIPT_H */
