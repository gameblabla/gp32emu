#ifndef GP32EMU_S3C2400_H
#define GP32EMU_S3C2400_H

#include "common.h"
#include "smartmedia.h"
#include "arm920t.h"

typedef struct s3c2400 s3c2400_t;

typedef void (*s3c2400_log_fn)(void *user, const char *line);

s3c2400_t *s3c2400_create(size_t ram_size);
void s3c2400_destroy(s3c2400_t *soc);
void s3c2400_reset(s3c2400_t *soc);
arm_bus_t s3c2400_get_bus(s3c2400_t *soc);

int s3c2400_load_bios(s3c2400_t *soc, const char *path, char *err, size_t err_len);
int s3c2400_load_bios_buffer(s3c2400_t *soc, const uint8_t *data, size_t size, char *err, size_t err_len);
int s3c2400_load_smartmedia(s3c2400_t *soc, const char *path, char *err, size_t err_len);
int s3c2400_load_smartmedia_buffer(s3c2400_t *soc, const uint8_t *data, size_t size, char *err, size_t err_len);
int s3c2400_save_smartmedia(s3c2400_t *soc, const char *path, char *err, size_t err_len);
int s3c2400_load_ram_image(s3c2400_t *soc, uint32_t addr, const uint8_t *data, size_t size, char *err, size_t err_len);
size_t s3c2400_ram_size(const s3c2400_t *soc);
void s3c2400_set_buttons(s3c2400_t *soc, uint32_t button_mask);
void s3c2400_set_irq_sink(s3c2400_t *soc, arm920t_t *cpu);
void s3c2400_set_log(s3c2400_t *soc, s3c2400_log_fn fn, void *user);

const uint32_t *s3c2400_framebuffer(s3c2400_t *soc, uint32_t *w, uint32_t *h, uint32_t *stride, uint64_t *frames);
const int16_t *s3c2400_audio_samples(s3c2400_t *soc, uint64_t *frames, uint32_t *sample_rate_hz);
void s3c2400_audio_append_u8_mono(s3c2400_t *soc, const uint8_t *samples, uint32_t sample_count, uint32_t sample_rate_hz);
void s3c2400_audio_append_s16_stereo(s3c2400_t *soc, int16_t left, int16_t right, uint32_t sample_rate_hz);
void s3c2400_audio_clear(s3c2400_t *soc);
void s3c2400_render_lcd(s3c2400_t *soc);
void s3c2400_tick(s3c2400_t *soc, uint32_t cpu_cycles);
uint32_t s3c2400_debug_read32(s3c2400_t *soc, uint32_t addr);
uint32_t s3c2400_fclk_hz(const s3c2400_t *soc);
uint32_t s3c2400_hclk_hz(const s3c2400_t *soc);
uint32_t s3c2400_run_clock_hz(const s3c2400_t *soc);
int s3c2400_state_save(const s3c2400_t *soc, FILE *f);
int s3c2400_state_load(s3c2400_t *soc, FILE *f);

uint8_t s3c2400_read8(void *user, uint32_t addr);
uint16_t s3c2400_read16(void *user, uint32_t addr);
uint32_t s3c2400_read32(void *user, uint32_t addr);
void s3c2400_write8(void *user, uint32_t addr, uint8_t value);
void s3c2400_write16(void *user, uint32_t addr, uint16_t value);
void s3c2400_write32(void *user, uint32_t addr, uint32_t value);

#endif
