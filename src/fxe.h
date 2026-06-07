#ifndef GP32EMU_FXE_H
#define GP32EMU_FXE_H

#include "common.h"

typedef struct fxe_image {
    uint8_t *payload;
    size_t payload_size;
    uint32_t load_addr;
    uint32_t entry_addr;
    char title[33];
    char author[33];
    int was_fxe;
    int was_b2fxec;
    int was_host_decrunched;
} fxe_image_t;

void fxe_image_free(fxe_image_t *img);
int fxe_load_file(const char *path, fxe_image_t *out, char *err, size_t err_len);
int fxe_load_buffer(const uint8_t *data, size_t size, const char *label, fxe_image_t *out, char *err, size_t err_len);

#endif
