#ifndef GP32EMU_SMC_DIRECT_H
#define GP32EMU_SMC_DIRECT_H

#include "common.h"
#include "fxe.h"
#include "fpk.h"

typedef struct smc_direct_package {
    fxe_image_t image;
    fpk_asset_t *assets;
    size_t asset_count;
    char title[64];
    char executable_path[260];
} smc_direct_package_t;

int smc_direct_load_file(const char *path, smc_direct_package_t *pkg, char *err, size_t err_len);
int smc_direct_load_buffer(const uint8_t *data, size_t size, const char *label, smc_direct_package_t *pkg, char *err, size_t err_len);
void smc_direct_package_free(smc_direct_package_t *pkg);

#endif /* GP32EMU_SMC_DIRECT_H */
