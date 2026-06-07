#include "fxe.h"
#include "zip.h"

#define FXE_MAGIC0 'f'
#define FXE_MAGIC1 'x'
#define FXE_MAGIC2 'e'
#define FXE_MAGIC3 ' '
#define FXE_INFO_SIZE 0x454u
#define FXE_FIXED_PREFIX_SIZE (4u + 4u + 4u + 32u + 32u + 16u + 1024u)
#define GP32_RAM_BASE 0x0c000000u

static void ferr(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || !err_len) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static uint8_t *read_whole_file(const char *path, size_t *out_size, char *err, size_t err_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { ferr(err, err_len, "open %s: %s", path, strerror(errno)); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { ferr(err, err_len, "seek %s failed", path); fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { ferr(err, err_len, "size %s failed", path); fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)n ? (size_t)n : 1u);
    if (!buf) { ferr(err, err_len, "out of memory reading %s", path); fclose(f); return NULL; }
    if ((size_t)n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        ferr(err, err_len, "read %s failed", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

static void copy_zstr(char *dst, size_t dst_len, const uint8_t *src, size_t src_len) {
    size_t n = 0;
    while (n + 1u < dst_len && n < src_len && src[n]) {
        dst[n] = (char)src[n];
        ++n;
    }
    dst[n] = '\0';
}


static int parse_gxb_header(fxe_image_t *img, char *err, size_t err_len);

static int branch_opcode(uint32_t w) { return (w & 0x0f000000u) == 0x0a000000u; }

static uint8_t b2_rd8(const uint8_t *mem, uint32_t addr) { return mem[addr - GP32_RAM_BASE]; }
static void b2_wr8(uint8_t *mem, uint32_t addr, uint8_t v) { mem[addr - GP32_RAM_BASE] = v; }
static uint16_t b2_rd16(const uint8_t *mem, uint32_t addr) {
    size_t o = (size_t)(addr - GP32_RAM_BASE);
    return (uint16_t)mem[o] | (uint16_t)((uint16_t)mem[o + 1u] << 8);
}
static void b2_wr16(uint8_t *mem, uint32_t addr, uint16_t v) {
    size_t o = (size_t)(addr - GP32_RAM_BASE);
    mem[o] = (uint8_t)v;
    mem[o + 1u] = (uint8_t)(v >> 8);
}
static uint32_t b2_rd32(const uint8_t *mem, uint32_t addr) {
    size_t o = (size_t)(addr - GP32_RAM_BASE);
    return gp32_ld32le(mem + o);
}
static void b2_wr32(uint8_t *mem, uint32_t addr, uint32_t v) {
    size_t o = (size_t)(addr - GP32_RAM_BASE);
    gp32_st32le(mem + o, v);
}
static int b2_in_ram(uint32_t addr, size_t len) {
    return addr >= GP32_RAM_BASE && len <= (size_t)(GP32_RAM_BASE + 0x00800000u - addr);
}
static uint32_t b2_ror32(uint32_t v, unsigned n) {
    n &= 31u;
    return n ? ((v >> n) | (v << (32u - n))) : v;
}

typedef struct b2ctx {
    uint8_t *mem;
    uint32_t ip;
    int32_t bit_count;
    uint32_t bitbuf;
} b2ctx_t;

static int b2_read_bits(b2ctx_t *ctx, unsigned n, uint32_t *out) {
    uint32_t r = n >= 32u ? 0u : (ctx->bitbuf >> (32u - n));
    ctx->bit_count -= (int32_t)n;
    ctx->bitbuf = n >= 32u ? 0u : (ctx->bitbuf << n);
    if (ctx->bit_count > 0) { *out = r; return 1; }
    if (!b2_in_ram(ctx->ip, 2u)) return 0;
    uint32_t h = b2_rd16(ctx->mem, ctx->ip);
    ctx->ip += 2u;
    int32_t neg = -ctx->bit_count;
    if (neg >= 32) return 0;
    ctx->bitbuf |= h << (unsigned)neg;
    ctx->bit_count = 16 - neg;
    *out = r;
    return 1;
}

static int b2_build_table(uint8_t *mem, uint32_t table, unsigned index_bits, unsigned count) {
    uint32_t p = table + 4u + (8u << index_bits);
    if (!b2_in_ram(table, (size_t)(4u + (8u << index_bits) + 4u + count * 4u + 4u))) return 0;
    b2_wr32(mem, p, 0u);
    p += 4u;
    b2_wr32(mem, table, p);
    b2_wr32(mem, p + count * 4u, (1u + (1u << index_bits)) << 16);
    return 1;
}

static int b2_sort_table(uint8_t *mem, uint32_t table, unsigned count) {
    uint32_t data = b2_rd32(mem, table);
    uint32_t out = table + 4u;
    if (!b2_in_ram(data - 4u, (size_t)(count + 2u) * 4u)) return 0;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t v = b2_rd32(mem, data + i * 4u);
        uint32_t p = data + i * 4u;
        for (;;) {
            p -= 4u;
            uint32_t prev = b2_rd32(mem, p);
            if (prev > v) { b2_wr32(mem, p + 4u, prev); continue; }
            break;
        }
        b2_wr32(mem, p + 4u, v);
    }

    unsigned sym = 0;
    while (sym < count && (b2_rd32(mem, data + sym * 4u) >> 16) == 0u) ++sym;
    uint32_t entry = data + sym * 4u;
    uint32_t code = 0;
    const uint32_t all = UINT32_MAX;
    while (sym < count) {
        uint16_t len = b2_rd16(mem, entry + 2u);
        uint16_t next_len = b2_rd16(mem, entry + 6u);
        int32_t diff = (int32_t)next_len - (int32_t)len;
        if (diff > 0) {
            uint32_t threshold = (all >> len) | b2_ror32(code, len);
            uint32_t base = sym - code;
            if (!b2_in_ram(out, 8u)) return 0;
            b2_wr32(mem, out, threshold); out += 4u;
            b2_wr16(mem, out, (uint16_t)base); out += 2u;
            b2_wr16(mem, out, len); out += 2u;
        }
        ++code;
        if (diff > 0) code <<= (unsigned)diff;
        ++sym;
        uint32_t word = b2_rd32(mem, entry);
        word &= 0x0000ffffu;
        b2_wr32(mem, entry, word);
        entry += 4u;
    }
    return 1;
}

static int b2_decode_symbol(b2ctx_t *ctx, uint32_t table, uint32_t *sym_out) {
    if (!b2_in_ram(table, 12u)) return 0;
    uint32_t symbols = b2_rd32(ctx->mem, table);
    uint32_t p = table + 4u;
    uint32_t threshold = b2_rd32(ctx->mem, p);
    p += 8u;
    unsigned guard = 0;
    while (ctx->bitbuf > threshold) {
        if (!b2_in_ram(p, 4u) || ++guard > 128u) return 0;
        threshold = b2_rd32(ctx->mem, p);
        p += 8u;
    }
    uint16_t len = b2_rd16(ctx->mem, p - 2u);
    uint16_t base = b2_rd16(ctx->mem, p - 4u);
    if (len > 31u) return 0;
    uint32_t top = len ? (ctx->bitbuf >> (32u - len)) : 0u;
    uint32_t idx = (top + base) & 0xffffu;
    if (!b2_in_ram(symbols + idx * 4u, 4u)) return 0;
    uint32_t sym = b2_rd32(ctx->mem, symbols + idx * 4u);
    uint32_t dummy = 0;
    if (!b2_read_bits(ctx, len, &dummy)) return 0;
    *sym_out = sym;
    return 1;
}


/* b2fxec algorithm 3 (FXP3) host-side decruncher.  This variant is used by
   later StoneCracker/b2fxec FXEs.  Its ARM stub uses three canonical Huffman
   trees, a 16-symbol MTF transform for tree lengths, PMR match reuse, and an
   in-place high-memory compressed stream.  Host decrunching avoids requiring
   the direct FXE firmware shim to execute the packer runtime correctly before
   the real payload starts. */
static int b2fxp3_build_table(uint8_t *mem, uint32_t table, unsigned count, uint32_t sentinel) {
    uint32_t symbols = table + 0x84u;
    if (!b2_in_ram(symbols - 4u, (size_t)(count + 2u) * 4u)) return 0;
    b2_wr32(mem, symbols - 4u, 0u);
    b2_wr32(mem, symbols + count * 4u, sentinel);
    return 1;
}

static int b2fxp3_sort_table(uint8_t *mem, uint32_t table, unsigned count) {
    uint32_t symbols = table + 0x84u;
    if (!b2_in_ram(symbols - 4u, (size_t)(count + 2u) * 4u)) return 0;

    for (unsigned i = 0; i < count; ++i) {
        uint32_t v = b2_rd32(mem, symbols + i * 4u);
        uint32_t p = symbols + i * 4u;
        for (;;) {
            p -= 4u;
            uint32_t prev = b2_rd32(mem, p);
            if (prev > v) { b2_wr32(mem, p + 4u, prev); continue; }
            break;
        }
        b2_wr32(mem, p + 4u, v);
    }

    unsigned first = 0;
    while (first < count && (b2_rd32(mem, symbols + first * 4u) >> 16) == 0u) ++first;
    if (first >= count) return 1;

    unsigned used = count - first;
    for (unsigned i = 0; i <= used; ++i) b2_wr32(mem, symbols + i * 4u, b2_rd32(mem, symbols + (first + i) * 4u));

    uint32_t out = table;
    uint32_t code = 0u;
    uint32_t p = symbols;
    const uint32_t all = UINT32_MAX;
    for (unsigned sym = 0; sym < used; ++sym) {
        uint16_t len = b2_rd16(mem, p + 2u);
        uint16_t next_len = b2_rd16(mem, p + 6u);
        int32_t diff = (int32_t)next_len - (int32_t)len;
        if (diff > 0) {
            if (len > 31u || !b2_in_ram(out, 8u)) return 0;
            uint32_t threshold = (all >> len) | b2_ror32(code, len);
            b2_wr32(mem, out, threshold); out += 4u;
            b2_wr16(mem, out, (uint16_t)(code - sym)); out += 2u;
            b2_wr16(mem, out, len); out += 2u;
        }
        ++code;
        if (diff > 0) code <<= (unsigned)diff;
        b2_wr32(mem, p, b2_rd32(mem, p) & 0xffffu);
        p += 4u;
    }
    return 1;
}

static int b2fxp3_decode_symbol(b2ctx_t *ctx, uint32_t table, uint32_t *sym_out) {
    if (!b2_in_ram(table, 8u) || !b2_in_ram(table + 0x84u, 4u)) return 0;
    uint32_t symbols = table + 0x84u;
    uint32_t p = table;
    uint32_t threshold = b2_rd32(ctx->mem, p);
    p += 8u;
    unsigned guard = 0;
    while (ctx->bitbuf > threshold) {
        if (!b2_in_ram(p, 8u) || ++guard > 128u) return 0;
        threshold = b2_rd32(ctx->mem, p);
        p += 8u;
    }
    uint16_t base = b2_rd16(ctx->mem, p - 4u);
    uint16_t len = b2_rd16(ctx->mem, p - 2u);
    if (len > 31u) return 0;
    uint32_t top = len ? (ctx->bitbuf >> (32u - len)) : 0u;
    uint32_t idx = (top - (uint32_t)base) & 0xffffu;
    if (!b2_in_ram(symbols + idx * 4u, 4u)) return 0;
    *sym_out = b2_rd32(ctx->mem, symbols + idx * 4u) & 0xffffu;
    uint32_t dummy = 0;
    return b2_read_bits(ctx, len, &dummy);
}

static int b2fxp3_fill_table(b2ctx_t *ctx, uint32_t pre_table, uint32_t table, unsigned count, uint8_t mtf[16]) {
    uint32_t symbols = table + 0x84u;
    if (!b2_in_ram(symbols, (size_t)count * 4u)) return 0;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t mtf_index = 0;
        if (!b2fxp3_decode_symbol(ctx, pre_table, &mtf_index) || mtf_index >= 16u) return 0;
        uint8_t value = mtf[mtf_index];
        while (mtf_index > 0u) {
            mtf[mtf_index] = mtf[mtf_index - 1u];
            --mtf_index;
        }
        mtf[0] = value;
        b2_wr32(ctx->mem, symbols + i * 4u, i | ((uint32_t)value << 16));
    }
    return b2fxp3_sort_table(ctx->mem, table, count);
}

static int b2fxec_finish_decrunch(fxe_image_t *img, uint8_t *mem, uint32_t source, uint32_t outp, char *err, size_t err_len) {
    uint32_t image_size = (uint32_t)(outp - source);
    if (b2_in_ram(source + 4u, 32u) && !branch_opcode(b2_rd32(mem, source)) && branch_opcode(b2_rd32(mem, source + 4u))) {
        image_size = b2_rd32(mem, source);
        source += 4u;
    }
    if (!b2_in_ram(source, 32u) || !branch_opcode(b2_rd32(mem, source))) {
        ferr(err, err_len, "b2fxec did not produce a GXB header");
        return 0;
    }
    if (image_size < 32u || !b2_in_ram(source, image_size)) {
        ferr(err, err_len, "invalid b2fxec output size");
        return 0;
    }
    uint8_t *new_payload = (uint8_t *)calloc(1, (size_t)image_size);
    if (!new_payload) { ferr(err, err_len, "out of memory storing decrunched image"); return 0; }
    memcpy(new_payload, mem + (source - GP32_RAM_BASE), (size_t)image_size);
    free(img->payload);
    img->payload = new_payload;
    img->payload_size = (size_t)image_size;
    img->was_host_decrunched = 1;
    if (!parse_gxb_header(img, err, err_len)) return 0;
    return 1;
}

static int b2fxec_host_decrunch_fxp3(fxe_image_t *img, char *err, size_t err_len) {
    const size_t ram_size = 8u * 1024u * 1024u;
    if (!img || !img->payload || img->payload_size < 0x348u) return 0;
    uint32_t rom_start = gp32_ld32le(img->payload + 4u);
    uint32_t rom_end = gp32_ld32le(img->payload + 12u);
    uint32_t comp_end = gp32_ld32le(img->payload + 20u);
    if (rom_end <= rom_start || rom_end - rom_start != 0x348u) { ferr(err, err_len, "not an FXP3 b2fxec stub"); return 0; }
    if (!b2_in_ram(rom_end, 4u) || !b2_in_ram(comp_end, 0u) || comp_end <= rom_end || (size_t)(comp_end - GP32_RAM_BASE) > img->payload_size) {
        ferr(err, err_len, "unsupported FXP3 b2fxec layout");
        return 0;
    }

    uint8_t *mem = (uint8_t *)calloc(1, ram_size);
    if (!mem) { ferr(err, err_len, "out of memory decrunching FXP3 b2fxec image"); return 0; }
    memcpy(mem, img->payload, img->payload_size < ram_size ? img->payload_size : ram_size);

    uint32_t high = GP32_RAM_BASE + (uint32_t)ram_size;
    uint32_t src = comp_end;
    for (;;) {
        if (src < GP32_RAM_BASE + 4u) { free(mem); ferr(err, err_len, "FXP3 b2fxec copy underflow"); return 0; }
        src -= 4u;
        uint32_t word = b2_rd32(mem, src);
        if (src != rom_end) {
            high -= 4u;
            if (!b2_in_ram(high, 4u)) { free(mem); ferr(err, err_len, "FXP3 b2fxec high copy overflow"); return 0; }
            b2_wr32(mem, high, word);
            continue;
        }
        break;
    }

    uint32_t first = b2_rd32(mem, high);
    high += 8u;
    b2_wr32(mem, src, first);
    uint32_t outp = src + 4u;
    uint32_t ip = high;
    int32_t bit_count = 16;
    uint32_t bitbuf = 0u;

    if (high < GP32_RAM_BASE + 16u + 12u + 0x1300u) { free(mem); ferr(err, err_len, "FXP3 b2fxec stream leaves no stack room"); return 0; }
    uint32_t sp = high - 16u - 12u - 0x1300u;
    if (!b2_in_ram(sp, 0x1300u)) { free(mem); ferr(err, err_len, "FXP3 b2fxec stack outside RAM"); return 0; }

    const uint32_t pre_table = sp;
    const uint32_t lit_table = sp + 0x0d8u;
    const uint32_t off_hi_table = sp + 0x970u;
    const uint32_t off_lo_table = sp + 0x0e10u;
    unsigned blocks = 0;
    for (;;) {
        if (bit_count == 16) ip -= 4u; else ip -= 2u;
        if (!b2_in_ram(ip, 2u)) { free(mem); ferr(err, err_len, "FXP3 b2fxec block header overrun"); return 0; }
        uint32_t block_len = b2_rd16(mem, ip); ip += 2u;
        if (block_len == 0u) break;
        if (block_len > 0x7fffu || !b2_in_ram(ip, block_len)) { free(mem); ferr(err, err_len, "FXP3 b2fxec invalid block length"); return 0; }
        uint32_t lo = b2_rd16(mem, ip); ip += 2u;
        uint32_t hi = b2_rd16(mem, ip); ip += 2u;
        b2ctx_t ctx;
        ctx.mem = mem;
        ctx.ip = ip;
        ctx.bit_count = 16;
        ctx.bitbuf = (lo << 16) | hi;

        if (!b2fxp3_build_table(mem, pre_table, 16u, 0x00080000u)) { free(mem); ferr(err, err_len, "FXP3 pretable build failed"); return 0; }
        for (unsigned i = 0; i < 16u; ++i) {
            uint32_t bits = 0;
            if (!b2_read_bits(&ctx, 3u, &bits)) { free(mem); ferr(err, err_len, "FXP3 pretable stream failed"); return 0; }
            b2_wr32(mem, pre_table + 0x84u + i * 4u, i | (bits << 16));
        }
        if (!b2fxp3_sort_table(mem, pre_table, 16u)) { free(mem); ferr(err, err_len, "FXP3 pretable sort failed"); return 0; }

        uint8_t mtf[16];
        for (unsigned i = 0; i < 16u; ++i) mtf[i] = (uint8_t)i;
        if (!b2fxp3_build_table(mem, lit_table, 0x200u, 0x00100000u) ||
            !b2fxp3_fill_table(&ctx, pre_table, lit_table, 0x200u, mtf) ||
            !b2fxp3_build_table(mem, off_hi_table, 0x100u, 0x00100000u) ||
            !b2fxp3_fill_table(&ctx, pre_table, off_hi_table, 0x100u, mtf) ||
            !b2fxp3_build_table(mem, off_lo_table, 0x100u, 0x00100000u) ||
            !b2fxp3_fill_table(&ctx, pre_table, off_lo_table, 0x100u, mtf)) {
            free(mem); ferr(err, err_len, "FXP3 tree stream failed"); return 0;
        }

        uint32_t old_len_minus1 = 0u;
        uint32_t old_offset = 1u;
        uint32_t old_offset_long = 1u;
        uint32_t emitted = 0u;
        for (;;) {
            uint32_t sym = 0;
            if (!b2fxp3_decode_symbol(&ctx, lit_table, &sym)) { free(mem); ferr(err, err_len, "FXP3 literal stream failed"); return 0; }
            if (sym < 0x100u) {
                if (!b2_in_ram(outp, 1u)) { free(mem); ferr(err, err_len, "FXP3 output overflow"); return 0; }
                b2_wr8(mem, outp++, (uint8_t)sym);
            } else if (sym == 0x100u) {
                break;
            } else {
                uint32_t len_minus1;
                uint32_t dist;
                if (sym == 0x101u) {
                    len_minus1 = old_len_minus1;
                    dist = old_offset;
                } else {
                    len_minus1 = sym - 0x101u;
                    old_len_minus1 = len_minus1;
                    uint32_t off_hi = 0;
                    if (!b2fxp3_decode_symbol(&ctx, off_hi_table, &off_hi)) { free(mem); ferr(err, err_len, "FXP3 offset-high stream failed"); return 0; }
                    if (off_hi == 0u) {
                        dist = old_offset_long;
                    } else {
                        dist = off_hi & 0x7fu;
                        if (dist != off_hi) {
                            uint32_t off_lo = 0;
                            if (!b2fxp3_decode_symbol(&ctx, off_lo_table, &off_lo)) { free(mem); ferr(err, err_len, "FXP3 offset-low stream failed"); return 0; }
                            dist = (dist << 8) | (off_lo & 0xffu);
                            old_offset_long = dist;
                        }
                    }
                    old_offset = dist;
                }
                if (dist == 0u) { free(mem); ferr(err, err_len, "FXP3 zero back-reference"); return 0; }
                for (uint32_t i = 0; i <= len_minus1; ++i) {
                    if (!b2_in_ram(outp, 1u) || dist > outp - GP32_RAM_BASE) { free(mem); ferr(err, err_len, "FXP3 back-reference overflow"); return 0; }
                    uint8_t v = b2_rd8(mem, outp - dist);
                    b2_wr8(mem, outp++, v);
                }
            }
            if (++emitted > 0x01000000u) { free(mem); ferr(err, err_len, "FXP3 block did not terminate"); return 0; }
        }
        ip = ctx.ip;
        bit_count = ctx.bit_count;
        bitbuf = ctx.bitbuf;
        GP32_UNUSED(bitbuf);
        if (++blocks > 4096u) { free(mem); ferr(err, err_len, "FXP3 too many blocks"); return 0; }
    }

    int ok = b2fxec_finish_decrunch(img, mem, rom_end, outp, err, err_len);
    free(mem);
    return ok;
}

static int b2fxec_host_decrunch(fxe_image_t *img, char *err, size_t err_len) {
    char fxp3_err[256] = {0};
    if (b2fxec_host_decrunch_fxp3(img, fxp3_err, sizeof(fxp3_err))) return 1;
    GP32_UNUSED(fxp3_err);
    const size_t ram_size = 8u * 1024u * 1024u;
    if (!img || !img->payload || img->payload_size < 0x40u) return 0;
    uint32_t rom_end = gp32_ld32le(img->payload + 12u);
    uint32_t comp_end = gp32_ld32le(img->payload + 20u);
    if (!b2_in_ram(rom_end, 4u) || !b2_in_ram(comp_end, 0u) || comp_end <= rom_end || (size_t)(comp_end - GP32_RAM_BASE) > img->payload_size) {
        ferr(err, err_len, "unsupported b2fxec layout");
        return 0;
    }
    uint8_t *mem = (uint8_t *)calloc(1, ram_size);
    if (!mem) { ferr(err, err_len, "out of memory decrunching b2fxec image"); return 0; }
    memcpy(mem, img->payload, img->payload_size < ram_size ? img->payload_size : ram_size);

    uint32_t r8 = GP32_RAM_BASE + (uint32_t)ram_size;
    uint32_t r9 = comp_end;
    uint32_t fp = rom_end;
    for (;;) {
        if (r9 < GP32_RAM_BASE + 4u) { free(mem); ferr(err, err_len, "b2fxec copy underflow"); return 0; }
        r9 -= 4u;
        uint32_t sl = b2_rd32(mem, r9);
        if (fp != r9) {
            r8 -= 4u;
            if (!b2_in_ram(r8, 4u)) { free(mem); ferr(err, err_len, "b2fxec high copy overflow"); return 0; }
            b2_wr32(mem, r8, sl);
            continue;
        }
        break;
    }
    uint32_t first = b2_rd32(mem, r8);
    r8 += 8u;
    b2_wr32(mem, r9, first);
    r9 += 4u;

    uint32_t sp = GP32_RAM_BASE + (uint32_t)ram_size - 16u - 0x780u;
    uint32_t ip = r8 - 4u;
    uint32_t outp = r9;
    unsigned blocks = 0;
    for (;;) {
        if (!b2_in_ram(ip, 2u)) { free(mem); ferr(err, err_len, "b2fxec stream overrun"); return 0; }
        uint32_t block_len = b2_rd16(mem, ip); ip += 2u;
        if (((block_len + 1u) & 0xffffffffu) == 0x10000u) break;
        if (!b2_in_ram(ip, 4u)) { free(mem); ferr(err, err_len, "b2fxec stream overrun"); return 0; }
        uint32_t lo = b2_rd16(mem, ip); ip += 2u;
        uint32_t hi = b2_rd16(mem, ip); ip += 2u;
        b2ctx_t ctx;
        ctx.mem = mem;
        ctx.ip = ip;
        ctx.bit_count = 16;
        ctx.bitbuf = (lo << 16) | hi;
        uint32_t block_end = ip + block_len;
        if (block_end < GP32_RAM_BASE || block_end > GP32_RAM_BASE + 0x00800004u) { free(mem); ferr(err, err_len, "b2fxec block outside RAM"); return 0; }

        if (!b2_build_table(mem, sp, 3u, 16u)) { free(mem); ferr(err, err_len, "b2fxec table build failed"); return 0; }
        uint32_t table_data = b2_rd32(mem, sp);
        for (unsigned i = 0; i < 16u; ++i) {
            uint32_t bits = 0;
            if (!b2_read_bits(&ctx, 3u, &bits)) { free(mem); ferr(err, err_len, "b2fxec table stream failed"); return 0; }
            b2_wr32(mem, table_data + i * 4u, i | (bits << 16));
        }
        if (!b2_sort_table(mem, sp, 16u)) { free(mem); ferr(err, err_len, "b2fxec table sort failed"); return 0; }

        if (!b2_build_table(mem, sp + 0x90u, 4u, 0x179u)) { free(mem); ferr(err, err_len, "b2fxec literal table build failed"); return 0; }
        table_data = b2_rd32(mem, sp + 0x90u);
        for (unsigned i = 0; i < 0x179u; ++i) {
            uint32_t sym = 0;
            if (!b2_decode_symbol(&ctx, sp, &sym)) { free(mem); ferr(err, err_len, "b2fxec literal table stream failed"); return 0; }
            b2_wr32(mem, table_data + i * 4u, i | (sym << 16));
        }
        if (!b2_sort_table(mem, sp + 0x90u, 0x179u)) { free(mem); ferr(err, err_len, "b2fxec literal table sort failed"); return 0; }

        const uint32_t adjust = 0x179u - 0x78u;
        uint32_t emitted = 0;
        for (;;) {
            uint32_t sym = 0;
            if (!b2_decode_symbol(&ctx, sp + 0x90u, &sym)) { free(mem); ferr(err, err_len, "b2fxec data stream failed"); return 0; }
            if (sym == 0x100u) break;
            if (sym < 0x100u) {
                if (!b2_in_ram(outp, 1u)) { free(mem); ferr(err, err_len, "b2fxec output overflow"); return 0; }
                b2_wr8(mem, outp++, (uint8_t)sym);
            } else {
                uint32_t v = sym - adjust;
                unsigned low = v & 7u;
                uint32_t len = 1u + (1u << low);
                if (low) { uint32_t extra = 0; if (!b2_read_bits(&ctx, low, &extra)) { free(mem); ferr(err, err_len, "b2fxec length stream failed"); return 0; } len += extra; }
                unsigned high = v >> 3;
                uint32_t dist = 1u;
                if (high) { uint32_t extra = 0; dist = 1u << high; if (!b2_read_bits(&ctx, high, &extra)) { free(mem); ferr(err, err_len, "b2fxec distance stream failed"); return 0; } dist += extra; }
                for (uint32_t i = 0; i < len; ++i) {
                    if (!b2_in_ram(outp, 1u) || dist > outp - GP32_RAM_BASE) { free(mem); ferr(err, err_len, "b2fxec back-reference overflow"); return 0; }
                    uint8_t v8 = b2_rd8(mem, outp - dist);
                    b2_wr8(mem, outp++, v8);
                }
            }
            if (++emitted > 0x01000000u) { free(mem); ferr(err, err_len, "b2fxec block did not terminate"); return 0; }
        }
        ip = block_end - 4u;
        if (++blocks > 4096u) { free(mem); ferr(err, err_len, "b2fxec too many blocks"); return 0; }
    }

    uint32_t source = rom_end;
    uint32_t image_size = (uint32_t)(outp - source);
    if (b2_in_ram(source + 4u, 32u) && !branch_opcode(b2_rd32(mem, source)) && branch_opcode(b2_rd32(mem, source + 4u))) {
        image_size = b2_rd32(mem, source);
        source += 4u;
    }
    if (!b2_in_ram(source, 32u) || !branch_opcode(b2_rd32(mem, source))) {
        free(mem);
        ferr(err, err_len, "b2fxec did not produce a GXB header");
        return 0;
    }
    if (image_size < 32u || !b2_in_ram(source, image_size)) {
        free(mem);
        ferr(err, err_len, "invalid b2fxec output size");
        return 0;
    }
    uint8_t *new_payload = (uint8_t *)calloc(1, (size_t)image_size);
    if (!new_payload) { free(mem); ferr(err, err_len, "out of memory storing decrunched image"); return 0; }
    memcpy(new_payload, mem + (source - GP32_RAM_BASE), (size_t)image_size);
    free(mem);
    free(img->payload);
    img->payload = new_payload;
    img->payload_size = (size_t)image_size;
    img->was_host_decrunched = 1;
    if (!parse_gxb_header(img, err, err_len)) return 0;
    return 1;
}

void fxe_image_free(fxe_image_t *img) {
    if (!img) return;
    free(img->payload);
    memset(img, 0, sizeof(*img));
}

static int parse_gxb_header(fxe_image_t *img, char *err, size_t err_len) {
    if (!img || !img->payload || img->payload_size < 32u) {
        ferr(err, err_len, "GXB payload too small");
        return 0;
    }
    uint32_t first = gp32_ld32le(&img->payload[0]);
    uint32_t rom_start = gp32_ld32le(&img->payload[4]);
    uint32_t rom_end = gp32_ld32le(&img->payload[8]);
    uint32_t bss_start = gp32_ld32le(&img->payload[12]);
    uint32_t ram_end = gp32_ld32le(&img->payload[16]);
    if (!branch_opcode(first) || rom_start < GP32_RAM_BASE || rom_start >= GP32_RAM_BASE + 0x02000000u) {
        ferr(err, err_len, "not a GP32 GXB/AXF payload");
        return 0;
    }
    if (rom_end < rom_start || bss_start < rom_start || ram_end < bss_start) {
        ferr(err, err_len, "invalid GXB address fields");
        return 0;
    }
    int32_t branch_off = (int32_t)((first & 0x00ffffffu) << 8) >> 6; /* sign-extend imm24, then << 2 */
    uint32_t branch_pc = rom_start + 8u;
    img->load_addr = rom_start;
    img->entry_addr = branch_pc + (uint32_t)branch_off;
    return 1;
}

int fxe_load_buffer(const uint8_t *data, size_t size, const char *label, fxe_image_t *out, char *err, size_t err_len) {
    if (!data || !out) { ferr(err, err_len, "invalid FXE buffer arguments"); return 0; }
    memset(out, 0, sizeof(*out));
    uint8_t *file = (uint8_t *)malloc(size ? size : 1u);
    if (!file) { ferr(err, err_len, "out of memory copying %s", label ? label : "FXE buffer"); return 0; }
    if (size) memcpy(file, data, size);

    if (size >= FXE_FIXED_PREFIX_SIZE + 8u && file[0] == FXE_MAGIC0 && file[1] == FXE_MAGIC1 && file[2] == FXE_MAGIC2 && file[3] == FXE_MAGIC3) {
        uint32_t legacy_file_size = gp32_ld32le(&file[4]);
        uint32_t info_size = gp32_ld32le(&file[8]);
        uint32_t payload_size = gp32_ld32le(&file[FXE_FIXED_PREFIX_SIZE]);
        uint32_t key_size = gp32_ld32le(&file[FXE_FIXED_PREFIX_SIZE + 4u]);
        size_t key_off = FXE_FIXED_PREFIX_SIZE + 8u;
        size_t data_off = key_off + (size_t)key_size;
        if (info_size != FXE_INFO_SIZE) { ferr(err, err_len, "unsupported FXE info size 0x%08" PRIx32, info_size); free(file); return 0; }
        if (legacy_file_size != payload_size && legacy_file_size + 8u != size) { GP32_UNUSED(legacy_file_size); }
        if (key_size == 0u || data_off > size || (size_t)payload_size > size - data_off) {
            ferr(err, err_len, "invalid FXE key/payload sizes");
            free(file);
            return 0;
        }
        uint8_t *payload = (uint8_t *)malloc((size_t)payload_size + (size_t)key_size);
        uint8_t *key = (uint8_t *)malloc((size_t)key_size);
        if (!payload || !key) {
            free(payload);
            free(key);
            ferr(err, err_len, "out of memory decrypting FXE");
            free(file);
            return 0;
        }
        for (uint32_t i = 0; i < key_size; ++i) key[i] = (uint8_t)(file[key_off + i] ^ 0xffu);
        for (uint32_t i = 0; i < payload_size; ++i) payload[i] = (uint8_t)(file[data_off + i] ^ key[i % key_size]);
        memcpy(payload + payload_size, key, key_size);
        free(key);
        out->payload = payload;
        out->payload_size = (size_t)payload_size + (size_t)key_size;
        out->was_fxe = 1;
        out->was_b2fxec = !memcmp(file + 76u, "**StoneCracker**", 16u);
        copy_zstr(out->title, sizeof(out->title), file + 12u, 32u);
        copy_zstr(out->author, sizeof(out->author), file + 44u, 32u);
        free(file);
        if (out->was_b2fxec) {
            char decrunch_err[256] = {0};
            if (b2fxec_host_decrunch(out, decrunch_err, sizeof(decrunch_err))) return 1;
            GP32_UNUSED(decrunch_err);
        }
        if (!parse_gxb_header(out, err, err_len)) { fxe_image_free(out); return 0; }
        return 1;
    }

    out->payload = file;
    out->payload_size = size;
    out->was_fxe = 0;
    out->was_b2fxec = 0;
    strcpy(out->title, label && label[0] ? label : "raw GXB");
    {
        char decrunch_err[256] = {0};
        if (b2fxec_host_decrunch(out, decrunch_err, sizeof(decrunch_err))) return 1;
        GP32_UNUSED(decrunch_err);
    }
    if (!parse_gxb_header(out, err, err_len)) { fxe_image_free(out); return 0; }
    return 1;
}

int fxe_load_file(const char *path, fxe_image_t *out, char *err, size_t err_len) {
    if (!path || !out) { ferr(err, err_len, "invalid FXE arguments"); return 0; }
    size_t size = 0;
    uint8_t *file = NULL;
    char entry_name[260] = {0};
    if (gp32_zip_path_maybe(path)) {
        static const char * const exts[] = { ".fxe", ".gxb" };
        if (!gp32_zip_read_first_matching(path, exts, GP32_ARRAY_COUNT(exts), &file, &size, entry_name, sizeof(entry_name), err, err_len)) return 0;
    } else {
        file = read_whole_file(path, &size, err, err_len);
        if (!file) return 0;
    }
    int ok = fxe_load_buffer(file, size, entry_name[0] ? entry_name : path, out, err, err_len);
    free(file);
    return ok;
}
