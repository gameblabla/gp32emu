#ifndef GP32EMU_SMARTMEDIA_H
#define GP32EMU_SMARTMEDIA_H

#include "common.h"

typedef struct smc smc_t;

smc_t *smc_create(void);
void smc_destroy(smc_t *smc);
int smc_load_file(smc_t *smc, const char *path, char *err, size_t err_len);
int smc_load_buffer(smc_t *smc, const uint8_t *data, size_t size, char *err, size_t err_len);
int smc_save_file(smc_t *smc, const char *path, char *err, size_t err_len);
void smc_reset(smc_t *smc);
int smc_is_present(const smc_t *smc);
int smc_is_protected(const smc_t *smc);
int smc_is_busy(const smc_t *smc);
uint8_t smc_data_r(smc_t *smc);
void smc_command_w(smc_t *smc, uint8_t data);
void smc_address_w(smc_t *smc, uint8_t data);
void smc_data_w(smc_t *smc, uint8_t data);
size_t smc_image_size(const smc_t *smc);
int smc_is_dirty(const smc_t *smc);
int smc_state_save(const smc_t *smc, FILE *f);
int smc_state_load(smc_t *smc, FILE *f);

#endif
