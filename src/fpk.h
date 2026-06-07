#ifndef GP32EMU_FPK_H
#define GP32EMU_FPK_H

#include "common.h"

typedef struct fpk_asset {
    char path[260];
    uint8_t *data;
    size_t size;
    uint16_t first_cluster;
    uint8_t attr;
} fpk_asset_t;

typedef struct fpk_package {
    uint8_t *fxe_data;
    size_t fxe_size;
    char title[64];
    fpk_asset_t *assets;
    size_t asset_count;
} fpk_package_t;

int fpk_load_package_file(const char *path, fpk_package_t *pkg, char *err, size_t err_len);
int fpk_load_package_buffer(const uint8_t *data, size_t size, const char *label, fpk_package_t *pkg, char *err, size_t err_len);
void fpk_package_free(fpk_package_t *pkg);
int fpk_load_main_fxe_file(const char *path, uint8_t **out_data, size_t *out_size, char *title, size_t title_len, char *err, size_t err_len);

#endif /* GP32EMU_FPK_H */
