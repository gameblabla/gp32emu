#ifndef GP32EMU_ZIP_H
#define GP32EMU_ZIP_H

#include "common.h"

int gp32_zip_path_maybe(const char *path);
int gp32_zip_read_first_matching(const char *zip_path,
                                 const char * const *exts,
                                 size_t ext_count,
                                 uint8_t **out_data,
                                 size_t *out_size,
                                 char *out_name,
                                 size_t out_name_len,
                                 char *err,
                                 size_t err_len);

#endif /* GP32EMU_ZIP_H */
