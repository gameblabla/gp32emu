#ifndef GP32EMU_COMMON_H
#define GP32EMU_COMMON_H

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GP32_UNUSED(x) ((void)(x))
#define GP32_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define GP32_BIT(x,n) (((x) >> (n)) & 1u)
#define GP32_BITS(x,hi,lo) (((x) >> (lo)) & ((UINT32_C(1) << ((hi) - (lo) + 1u)) - 1u))

static inline uint16_t gp32_ld16le(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}
static inline uint32_t gp32_ld32le(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static inline void gp32_st16le(void *p, uint16_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}
static inline void gp32_st32le(void *p, uint32_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}
static inline uint32_t gp32_ror32(uint32_t v, unsigned s) {
    s &= 31u;
    return s ? ((v >> s) | (v << (32u - s))) : v;
}
static inline uint32_t gp32_rol32(uint32_t v, unsigned s) {
    s &= 31u;
    return s ? ((v << s) | (v >> (32u - s))) : v;
}
static inline int32_t gp32_sign_extend(uint32_t v, unsigned bits) {
    const uint32_t m = UINT32_C(1) << (bits - 1u);
    return (int32_t)((v ^ m) - m);
}

#endif /* GP32EMU_COMMON_H */
