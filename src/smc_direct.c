#include "smc_direct.h"
#include "zip.h"

typedef struct smc_file_buf {
    char path[260];
    uint8_t *data;
    size_t size;
    uint8_t attr;
    uint16_t first_cluster;
} smc_file_buf_t;

typedef struct smc_file_list {
    smc_file_buf_t *items;
    size_t count;
    size_t cap;
} smc_file_list_t;

typedef struct fat_view {
    const uint8_t *img;
    size_t img_size;
    uint32_t part_sec;
    uint32_t bytes_per_sec;
    uint32_t sec_per_clus;
    uint32_t reserved_sec;
    uint32_t fats;
    uint32_t root_entries;
    uint32_t total_sec;
    uint32_t sec_per_fat;
    uint32_t root_sec;
    uint32_t root_sec_count;
    uint32_t data_sec;
    uint32_t cluster_count;
    int fat_bits;
} fat_view_t;

static void serr(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || !err_len) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static int ends_ci(const char *s, const char *ext) {
    size_t ns = s ? strlen(s) : 0u;
    size_t ne = ext ? strlen(ext) : 0u;
    if (ns < ne) return 0;
    s += ns - ne;
    for (size_t i = 0; i < ne; ++i) if (tolower((unsigned char)s[i]) != tolower((unsigned char)ext[i])) return 0;
    return 1;
}

static int eq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void copy_str_trunc(char *dst, size_t dst_len, const char *src) {
    if (!dst || !dst_len) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= dst_len) n = dst_len - 1u;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint8_t *read_file_or_zip(const char *path, size_t *out_size, char *err, size_t err_len) {
    uint8_t *buf = NULL;
    size_t len = 0;
    if (!path || !out_size) return NULL;
    *out_size = 0;
    if (gp32_zip_path_maybe(path)) {
        static const char * const exts[] = { ".smc" };
        char entry[260] = {0};
        if (!gp32_zip_read_first_matching(path, exts, GP32_ARRAY_COUNT(exts), &buf, &len, entry, sizeof(entry), err, err_len)) return NULL;
        GP32_UNUSED(entry);
        *out_size = len;
        return buf;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { serr(err, err_len, "open %s: %s", path, strerror(errno)); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { serr(err, err_len, "seek %s failed", path); fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { serr(err, err_len, "empty SMC image %s", path); fclose(f); return NULL; }
    rewind(f);
    buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { serr(err, err_len, "out of memory reading %s", path); fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        serr(err, err_len, "read %s failed", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

static uint16_t smc_spare_lba(const uint8_t *spare) {
    uint16_t v = (uint16_t)(((uint16_t)spare[6] << 8) | spare[7]);
    return (uint16_t)((v & 0x0ffeu) >> 1);
}

static int smc_reconstruct_512(const uint8_t *src, size_t len, uint8_t **out_img, size_t *out_size, char *err, size_t err_len) {
    if (!src || !out_img || !out_size) return 0;
    *out_img = NULL;
    *out_size = 0;

    size_t off = 0;
    if ((len > 1024u) && ((len - 1024u) % 528u) == 0) off = 1024u;
    if (((len - off) % 528u) == 0) {
        const size_t page_total = 528u;
        const size_t page_data = 512u;
        const size_t pages_per_block = 32u;
        size_t pages = (len - off) / page_total;
        size_t blocks = pages / pages_per_block;
        if (!blocks || pages != blocks * pages_per_block) { serr(err, err_len, "unsupported SMC page/block count"); return 0; }
        size_t img_size = pages * page_data;
        uint8_t *img = (uint8_t *)malloc(img_size ? img_size : 1u);
        uint8_t *seen = (uint8_t *)calloc(blocks ? blocks : 1u, 1u);
        if (!img || !seen) {
            free(img);
            free(seen);
            serr(err, err_len, "out of memory reconstructing SMC FAT image");
            return 0;
        }
        memset(img, 0xff, img_size);
        for (size_t pb = 0; pb < blocks; ++pb) {
            const uint8_t *first = src + off + pb * pages_per_block * page_total;
            const uint8_t *sp = first + page_data;
            if (sp[6] == 0xffu && sp[7] == 0xffu) continue;
            uint16_t lb = smc_spare_lba(sp);
            if ((size_t)lb >= blocks) continue;

            /*
             * SmartMedia logical block numbers are local to 1024-physical-block
             * zones.  32 MiB cards have two zones, so local LBA 0..999 in
             * physical zone 1 maps to logical block 1000..1999.  Without this,
             * BIOSless FAT extraction sees only the first half of larger retail
             * SMC images and reads late .GXE/.GXC files as erased 0xff pages.
             *
             * Some dumps also contain stale duplicate physical blocks inside a
             * zone.  Preserve the first valid translation entry for each
             * zone-adjusted logical block, matching firmware scan order, instead
             * of overwriting live FAT/root sectors with later stale copies.
             */
            size_t logical = (pb / 1024u) * 1000u + (size_t)lb;
            if (logical >= blocks) continue;
            if (seen[logical]) continue;
            seen[logical] = 1u;
            for (size_t p = 0; p < pages_per_block; ++p) {
                memcpy(img + (logical * pages_per_block + p) * page_data,
                       src + off + (pb * pages_per_block + p) * page_total,
                       page_data);
            }
        }
        free(seen);
        *out_img = img;
        *out_size = img_size;
        return 1;
    }

    if ((len % 512u) == 0) {
        uint8_t *img = (uint8_t *)malloc(len ? len : 1u);
        if (!img) { serr(err, err_len, "out of memory copying SMC FAT image"); return 0; }
        memcpy(img, src, len);
        *out_img = img;
        *out_size = len;
        return 1;
    }

    serr(err, err_len, "unsupported BIOSless SMC image size %zu", len);
    return 0;
}

static int fat_parse_at(const uint8_t *img, size_t size, uint32_t sec, fat_view_t *fv) {
    if (!img || !fv || (uint64_t)(sec + 1u) * 512u > size) return 0;
    const uint8_t *b = img + (size_t)sec * 512u;
    if (b[510] != 0x55u || b[511] != 0xaau) return 0;
    uint16_t bps = gp32_ld16le(b + 11u);
    uint8_t spc = b[13];
    uint16_t res = gp32_ld16le(b + 14u);
    uint8_t fats = b[16];
    uint16_t root_entries = gp32_ld16le(b + 17u);
    uint16_t total16 = gp32_ld16le(b + 19u);
    uint16_t spf16 = gp32_ld16le(b + 22u);
    uint32_t hidden = gp32_ld32le(b + 28u);
    uint32_t total32 = gp32_ld32le(b + 32u);
    if (bps != 512u || spc == 0u || res == 0u || fats == 0u || spf16 == 0u) return 0;
    if (memcmp(b + 54u, "FAT12", 5u) && memcmp(b + 54u, "FAT16", 5u)) return 0;
    uint32_t total = total16 ? total16 : total32;
    if (!total) return 0;
    uint32_t part = hidden == sec ? hidden : sec;
    uint32_t root_secs = ((uint32_t)root_entries * 32u + bps - 1u) / bps;
    uint32_t root_sec = part + res + (uint32_t)fats * (uint32_t)spf16;
    uint32_t data_sec = root_sec + root_secs;
    if ((uint64_t)(part + total) * 512u > size || data_sec >= part + total) return 0;
    uint32_t clusters = (total - (data_sec - part)) / spc;
    memset(fv, 0, sizeof(*fv));
    fv->img = img;
    fv->img_size = size;
    fv->part_sec = part;
    fv->bytes_per_sec = bps;
    fv->sec_per_clus = spc;
    fv->reserved_sec = res;
    fv->fats = fats;
    fv->root_entries = root_entries;
    fv->total_sec = total;
    fv->sec_per_fat = spf16;
    fv->root_sec = root_sec;
    fv->root_sec_count = root_secs;
    fv->data_sec = data_sec;
    fv->cluster_count = clusters;
    fv->fat_bits = (clusters < 4085u) ? 12 : 16;
    return 1;
}

static int fat_find(const uint8_t *img, size_t size, fat_view_t *fv) {
    if (!img || !fv || size < 512u) return 0;
    size_t sectors = size / 512u;
    for (size_t s = 0; s < sectors; ++s) {
        if (fat_parse_at(img, size, (uint32_t)s, fv)) return 1;
    }
    return 0;
}

static uint16_t fat_next_cluster(const fat_view_t *fv, uint16_t cl) {
    const uint8_t *fat = fv->img + (size_t)(fv->part_sec + fv->reserved_sec) * 512u;
    size_t fat_size = (size_t)fv->sec_per_fat * 512u;
    if (fv->fat_bits == 12) {
        size_t off = (size_t)cl + ((size_t)cl >> 1);
        if (off + 1u >= fat_size) return 0xfffu;
        uint16_t v = (uint16_t)(fat[off] | ((uint16_t)fat[off + 1u] << 8));
        return (cl & 1u) ? (uint16_t)(v >> 4) : (uint16_t)(v & 0x0fffu);
    }
    size_t off = (size_t)cl * 2u;
    if (off + 1u >= fat_size) return 0xffffu;
    return gp32_ld16le(fat + off);
}

static int fat_cluster_is_eoc(const fat_view_t *fv, uint16_t cl) {
    return fv->fat_bits == 12 ? cl >= 0x0ff8u : cl >= 0xfff8u;
}

static const uint8_t *fat_cluster_ptr(const fat_view_t *fv, uint16_t cl) {
    if (cl < 2u) return NULL;
    uint32_t sec = fv->data_sec + ((uint32_t)cl - 2u) * fv->sec_per_clus;
    if ((uint64_t)(sec + fv->sec_per_clus) * 512u > fv->img_size) return NULL;
    return fv->img + (size_t)sec * 512u;
}

static uint8_t *fat_read_chain(const fat_view_t *fv, uint16_t cl, size_t want, size_t *out_size) {
    if (out_size) *out_size = 0;
    if (!fv || cl < 2u) return NULL;
    size_t cap = want ? want : 1u;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) return NULL;
    size_t len = 0;
    uint32_t guard = 0;
    size_t cluster_bytes = (size_t)fv->sec_per_clus * 512u;
    while (cl >= 2u && !fat_cluster_is_eoc(fv, cl) && guard++ <= fv->cluster_count + 2u && len < want) {
        const uint8_t *p = fat_cluster_ptr(fv, cl);
        if (!p) break;
        size_t n = cluster_bytes;
        if (n > want - len) n = want - len;
        memcpy(out + len, p, n);
        len += n;
        if (len >= want) break;
        cl = fat_next_cluster(fv, cl);
    }
    if (out_size) *out_size = len;
    return out;
}

static void fat_83_name(const uint8_t *e, char *out, size_t out_len) {
    char name[9];
    char ext[4];
    size_t n = 0, x = 0;
    while (n < 8u && e[n] != ' ') { name[n] = (char)e[n]; ++n; }
    name[n] = '\0';
    while (x < 3u && e[8u + x] != ' ') { ext[x] = (char)e[8u + x]; ++x; }
    ext[x] = '\0';
    if (x) snprintf(out, out_len, "%s.%s", name, ext);
    else snprintf(out, out_len, "%s", name);
}

static int file_list_add(smc_file_list_t *list, const char *path, const uint8_t *data, size_t size, uint8_t attr, uint16_t cl, char *err, size_t err_len) {
    if (!list || !path || !data) return 0;
    if (list->count == list->cap) {
        size_t nc = list->cap ? list->cap * 2u : 32u;
        smc_file_buf_t *ni = (smc_file_buf_t *)realloc(list->items, nc * sizeof(list->items[0]));
        if (!ni) { serr(err, err_len, "out of memory indexing SMC files"); return 0; }
        list->items = ni;
        list->cap = nc;
    }
    smc_file_buf_t *it = &list->items[list->count++];
    memset(it, 0, sizeof(*it));
    snprintf(it->path, sizeof(it->path), "%s", path);
    it->data = (uint8_t *)malloc(size ? size : 1u);
    if (!it->data) { serr(err, err_len, "out of memory extracting %s", path); return 0; }
    if (size) memcpy(it->data, data, size);
    it->size = size;
    it->attr = attr;
    it->first_cluster = cl;
    return 1;
}

static void file_list_free(smc_file_list_t *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) free(list->items[i].data);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int fat_walk_dir(const fat_view_t *fv, const uint8_t *dir, size_t dir_size, const char *prefix, smc_file_list_t *list, unsigned depth, char *err, size_t err_len) {
    if (!fv || !dir || !list || depth > 12u) return 1;
    for (size_t off = 0; off + 32u <= dir_size; off += 32u) {
        const uint8_t *e = dir + off;
        if (e[0] == 0x00u) break;
        if (e[0] == 0xe5u || e[0] == 0xffu) continue;
        uint8_t attr = e[11];
        if (attr == 0x0fu || (attr & 0x08u)) continue;
        char name[32];
        fat_83_name(e, name, sizeof(name));
        if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) continue;
        uint16_t cl = gp32_ld16le(e + 26u);
        uint32_t size = gp32_ld32le(e + 28u);
        char path[260];
        if (prefix && prefix[0]) snprintf(path, sizeof(path), "%s/%s", prefix, name);
        else snprintf(path, sizeof(path), "%s", name);
        if (attr & 0x10u) {
            size_t sub_size = 0;
            uint32_t max_dir = (fv->cluster_count + 2u) * fv->sec_per_clus * 512u;
            uint8_t *sub = fat_read_chain(fv, cl, max_dir, &sub_size);
            if (sub) {
                int ok = fat_walk_dir(fv, sub, sub_size, path, list, depth + 1u, err, err_len);
                free(sub);
                if (!ok) return 0;
            }
        } else {
            size_t got = 0;
            uint8_t *buf = fat_read_chain(fv, cl, size, &got);
            if (!buf || got < size) { free(buf); serr(err, err_len, "failed extracting %s from SMC FAT", path); return 0; }
            int ok = file_list_add(list, path, buf, size, attr, cl, err, err_len);
            free(buf);
            if (!ok) return 0;
        }
    }
    return 1;
}

static int fat_extract_all(const fat_view_t *fv, smc_file_list_t *list, char *err, size_t err_len) {
    size_t root_size = (size_t)fv->root_sec_count * 512u;
    if ((uint64_t)fv->root_sec * 512u + root_size > fv->img_size) return 0;
    return fat_walk_dir(fv, fv->img + (size_t)fv->root_sec * 512u, root_size, "", list, 0u, err, err_len);
}

static const smc_file_buf_t *find_first_ext(const smc_file_list_t *list, const char *ext) {
    if (!list || !ext) return NULL;
    for (size_t i = 0; i < list->count; ++i) if (ends_ci(list->items[i].path, ext)) return &list->items[i];
    return NULL;
}

static void basename_no_ext(const char *path, char *out, size_t out_len) {
    if (!out || !out_len) return;
    out[0] = '\0';
    if (!path) return;
    const char *s = strrchr(path, '/');
    s = s ? s + 1 : path;
    size_t n = 0;
    while (s[n] && s[n] != '.' && n + 1u < out_len) { out[n] = s[n]; ++n; }
    out[n] = '\0';
}

static const smc_file_buf_t *find_matching_gxc(const smc_file_list_t *list, const char *gxe_path) {
    char base[64];
    basename_no_ext(gxe_path, base, sizeof(base));
    const smc_file_buf_t *fallback = NULL;
    for (size_t i = 0; list && i < list->count; ++i) {
        const smc_file_buf_t *it = &list->items[i];
        if (!ends_ci(it->path, ".gxc")) continue;
        if (!fallback) fallback = it;
        char b[64];
        basename_no_ext(it->path, b, sizeof(b));
        if (eq_ci(base, b)) return it;
    }
    return fallback;
}

static int valid_gxb_header(const uint8_t *p, size_t size, uint32_t *out_size) {
    if (!p || size < 32u) return 0;
    uint32_t first = gp32_ld32le(p);
    uint32_t rom = gp32_ld32le(p + 4u);
    uint32_t ro_limit = gp32_ld32le(p + 8u);
    uint32_t rw_base = gp32_ld32le(p + 12u);
    uint32_t zi_limit = gp32_ld32le(p + 16u);
    uint32_t rw_limit = gp32_ld32le(p + 20u);
    if ((first & 0x0f000000u) != 0x0a000000u) return 0;
    if (rom < 0x0c000000u || rom >= 0x0e000000u || ro_limit < rom ||
        rw_base < rom || rw_limit < rw_base || zi_limit < rw_limit) return 0;
    uint32_t ro_size = ro_limit - rom;
    uint32_t rw_size = rw_limit - rw_base;
    if (ro_size < 32u || (size_t)ro_size > size) return 0;

    /*
     * Retail .GXC payloads are not always just a contiguous ROM/RO image.
     * They can append the initialised RW section immediately after the RO
     * bytes.  The BIOS launcher keeps that tail and the program's scatterload
     * copies it to rw_base before zeroing ZI.  If the BIOSless loader truncates
     * the decrypted payload at ro_limit-rom, Korean SDK titles such as Princess
     * Maker 2 lose their initialised Hangul/text tables at 0x0c128000 and
     * render distorted syllables compared with the real-BIOS path.
     */
    uint64_t image_size = (uint64_t)ro_size + (uint64_t)rw_size;
    if (rw_size && image_size <= size && image_size <= UINT32_MAX) {
        if (out_size) *out_size = (uint32_t)image_size;
    } else {
        if (out_size) *out_size = ro_size;
    }
    return 1;
}

typedef struct gxc_key_profile {
    const char *name;
    const uint8_t *key;
    size_t key_len;
} gxc_key_profile_t;

static void gxc_xor_region(uint8_t *buf, size_t size, size_t off, size_t n, const gxc_key_profile_t *kp, size_t key_start) {
    if (!buf || !kp || !kp->key_len || off >= size) return;
    if (n > size - off) n = size - off;
    for (size_t i = 0; i < n; ++i) buf[off + i] ^= kp->key[(key_start + i) % kp->key_len];
}

static void gxc_decrypt_prefix(uint8_t *buf, size_t size, const gxc_key_profile_t *kp, size_t key_start) {
    gxc_xor_region(buf, size, 0u, 0x100u, kp, key_start);
}

static void gxc_decrypt_periodic(uint8_t *buf, size_t size, const gxc_key_profile_t *kp, size_t chunk, size_t key_start) {
    const size_t enc = 0x100u;
    if (!chunk) return;
    for (size_t off = 0; off < size; off += chunk) gxc_xor_region(buf, size, off, enc, kp, key_start);
}

static void gxc_decrypt_periodic_from(uint8_t *buf, size_t size, const gxc_key_profile_t *kp, size_t chunk, size_t key_start, size_t first_off) {
    const size_t enc = 0x100u;
    if (!chunk) return;
    for (size_t off = first_off; off < size; off += chunk) gxc_xor_region(buf, size, off, enc, kp, key_start);
}

static unsigned gxe_encryption_profile(const smc_file_buf_t *gxe) {
    if (!gxe || !gxe->data || gxe->size < 0x81u) return 0u;
    return (unsigned)gxe->data[0x7cu];
}

static int gxc_candidate_score(const uint8_t *buf, size_t size) {
    int score = 0;
    size_t limit = size < 0x20000u ? size : 0x20000u;
    for (size_t off = 0; off + 4u <= limit; off += 0x400u) {
        size_t end = off + 0x40u;
        if (end > limit) end = limit;
        for (size_t p = off; p + 4u <= end; p += 4u) {
            uint32_t w = gp32_ld32le(buf + p);
            uint32_t cond = w >> 28;
            if (cond == 0x0eu) score += 3;
            else if (cond <= 0x0bu) score += 1;
            else if (cond == 0x0fu) score -= 1;
            if (w == 0xe1a00000u || w == 0xeafffffeu || ((w & 0x0f000000u) == 0x0a000000u)) score += 2;
            if (w == 0xffffffffu || w == 0x00000000u) score -= 1;
        }
    }
    return score;
}

static int gxc_candidate_arm_score_range(const uint8_t *buf, size_t size, size_t start, size_t step) {
    int score = 0;
    if (!buf || !step || start >= size) return 0;
    for (size_t off = start; off < size; off += step) {
        size_t end = off + 0x100u;
        if (end > size) end = size;
        for (size_t p = off; p + 4u <= end; p += 4u) {
            uint32_t w = gp32_ld32le(buf + p);
            uint32_t cond = w >> 28;
            if (cond == 0x0eu) score += 3;
            else if (cond <= 0x0bu) score += 1;
            else if (cond == 0x0fu) score -= 2;
            if (w == 0xe1a00000u || w == 0xe1a0f00eu || ((w & 0x0f000000u) == 0x0a000000u) || ((w & 0x0f000000u) == 0x0b000000u)) score += 2;
            if (w == 0xffffffffu || w == 0x00000000u) score -= 1;
        }
    }
    return score;
}

static int gxc_try_decrypt(const smc_file_buf_t *gxc, uint32_t payload_size, const gxc_key_profile_t *kp,
                           size_t periodic_chunk, size_t key_start, uint8_t **out, size_t *out_size, int *out_score) {
    uint8_t *buf = (uint8_t *)malloc((size_t)payload_size ? (size_t)payload_size : 1u);
    if (!buf) return -1;
    memcpy(buf, gxc->data + 20u, (size_t)payload_size);
    if (periodic_chunk) gxc_decrypt_periodic(buf, (size_t)payload_size, kp, periodic_chunk, key_start);
    else gxc_decrypt_prefix(buf, (size_t)payload_size, kp, key_start);

    uint32_t gxb_size = 0;
    if (!valid_gxb_header(buf, (size_t)payload_size, &gxb_size)) {
        free(buf);
        return 0;
    }
    *out = buf;
    *out_size = (size_t)gxb_size;
    if (out_score) *out_score = gxc_candidate_score(buf, (size_t)payload_size);
    return 1;
}

static uint32_t gxc_payload_size_bytes(const smc_file_buf_t *gxc) {
    if (!gxc || !gxc->data || gxc->size <= 20u) return 0u;
    uint32_t payload_size = gp32_ld32le(gxc->data);
    if (!payload_size || (size_t)payload_size > gxc->size - 20u) payload_size = (uint32_t)(gxc->size - 20u);
    return payload_size;
}


static int gxc_payload_has_gp32_crt_stub(const uint8_t *p, size_t size) {
    if (!p || size < 0x16cu) return 0;
    static const uint32_t sig[] = {
        0xe1811002u, 0xe0011003u, 0xe0211004u, 0xe1a05221u,
        0xe0811007u, 0xe0477001u, 0xe3a08000u, 0xe1a0f008u
    };
    for (size_t i = 0; i < GP32_ARRAY_COUNT(sig); ++i) {
        if (gp32_ld32le(p + i * 4u) != sig[i]) return 0;
    }
    return 1;
}

static int gxc_rebuild_stripped_gxb(const smc_file_buf_t *gxe, const smc_file_buf_t *gxc, uint32_t payload_size, uint8_t **out, size_t *out_size) {
    if (!gxc || !out || !out_size || payload_size < 0x200u || (size_t)payload_size > gxc->size - 20u) return 0;
    const uint8_t *src = gxc->data + 20u;
    if (!gxc_payload_has_gp32_crt_stub(src, (size_t)payload_size)) return 0;
    if ((size_t)payload_size <= 0x168u + 4u) return 0;

    const uint32_t rom = 0x0c000000u;
    const uint32_t ro_limit = gp32_ld32le(src + 0x15cu);
    const uint32_t rw_limit = gp32_ld32le(src + 0x160u);
    const uint32_t rw_base = gp32_ld32le(src + 0x164u);
    const uint32_t zi_limit = gp32_ld32le(src + 0x168u);
    if (ro_limit <= rom || rw_base < rom || rw_limit < rw_base || zi_limit < rw_limit) return 0;
    uint32_t ro_size = ro_limit - rom;
    uint32_t rw_size = rw_limit - rw_base;
    if (ro_size + rw_size != payload_size + 0x100u) return 0;

    size_t ret_off = SIZE_MAX;
    for (size_t off = 0x50u; off + 8u <= (size_t)payload_size; off += 4u) {
        if (gp32_ld32le(src + off) == 0xe1a0f00eu &&
            gp32_ld32le(src + off + 4u) == 0xe1a0f00eu &&
            gp32_ld32le(src + off - 0x50u) == 0x0a000006u &&
            gp32_ld32le(src + off - 0x4cu) == 0xe92d4007u &&
            gp32_ld32le(src + off - 0x48u) == 0xe1a0e00fu &&
            gp32_ld32le(src + off - 0x44u) == 0xe1a0f003u &&
            gp32_ld32le(src + off - 0x38u) == 0xe8bd4007u &&
            gp32_ld32le(src + off - 0x34u) == 0xeafffff5u &&
            gp32_ld32le(src + off - 0x30u) == 0xe10f5000u &&
            gp32_ld32le(src + off - 0x2cu) == 0xe38550c0u &&
            gp32_ld32le(src + off - 0x28u) == 0xe12ff005u &&
            gp32_ld32le(src + off - 0x24u) == 0xe92d1fffu &&
            gp32_ld32le(src + off - 0x1cu) == 0xeafffffeu) {
            ret_off = off;
            break;
        }
    }
    if (ret_off == SIZE_MAX) {
        for (size_t off = 0u; off + 8u <= (size_t)payload_size; off += 4u) {
            if (gp32_ld32le(src + off) == 0xe1a0f00eu && gp32_ld32le(src + off + 4u) == 0xe1a0f00eu) { ret_off = off; break; }
        }
    }
    if (ret_off == SIZE_MAX) return 0;

    size_t total = (size_t)payload_size + 0x100u;
    uint8_t *buf = (uint8_t *)calloc(1u, total ? total : 1u);
    if (!buf) return -1;

    gp32_st32le(buf + 0x00u, 0xea000015u);
    gp32_st32le(buf + 0x04u, rom);
    gp32_st32le(buf + 0x08u, ro_limit);
    gp32_st32le(buf + 0x0cu, rw_base);
    gp32_st32le(buf + 0x10u, zi_limit);
    gp32_st32le(buf + 0x14u, rw_limit);
    gp32_st32le(buf + 0x18u, zi_limit);
    static const uint32_t hdr_words_1c[] = {
        0x44450011u, 0x44450011u, 0x01234567u, 0x12345678u,
        0x23456789u, 0x34567890u, 0x45678901u, 0x56789012u,
        0x23456789u, 0x34567890u, 0x45678901u, 0x56789012u,
        0x23456789u, 0x34567890u, 0x45678901u, 0x56789012u,
        0xe10f0000u, 0xe38000c0u, 0xe12ff000u
    };
    for (size_t i = 0u; i < GP32_ARRAY_COUNT(hdr_words_1c); ++i) gp32_st32le(buf + 0x1cu + (uint32_t)i * 4u, hdr_words_1c[i]);
    uint32_t target = rom + 0x100u + (uint32_t)ret_off;
    int32_t rel = (int32_t)(target - (rom + 0x68u + 8u));
    if ((rel & 3) != 0) { free(buf); return 0; }
    int32_t imm = rel >> 2;
    if (imm < -0x800000 || imm > 0x7fffff) { free(buf); return 0; }
    gp32_st32le(buf + 0x68u, 0xeb000000u | ((uint32_t)imm & 0x00ffffffu));

    static const uint32_t hdr_words_6c[] = {
        0xe3a00000u, 0xef00000bu, 0xe59f11bcu, 0xe59f21bcu,
        0xe5810000u, 0xe5820000u, 0xe3a00006u, 0xef00000bu,
        0xe59f11acu, 0xe5810000u, 0xe3a00000u, 0xef000010u,
        0xe59f21a0u, 0xe59f31a0u, 0xe5820000u, 0xe5831000u,
        0xe59f0198u, 0xe59f1198u, 0xe5810000u, 0xe3a00005u,
        0xef00000bu, 0xe59f118cu, 0xe24000ffu, 0xe3c00003u,
        0xe5810000u, 0xef000015u, 0xe1a0a000u, 0xe1a0b001u,
        0xe10f0000u, 0xe3c000c0u, 0xe3800040u, 0xe12ff000u,
        0xe1a0000au, 0xe1a0100bu, 0xe59f315cu, 0xe1a0f003u,
        0xef000012u
    };
    for (size_t i = 0u; i < GP32_ARRAY_COUNT(hdr_words_6c); ++i) gp32_st32le(buf + 0x6cu + (uint32_t)i * 4u, hdr_words_6c[i]);
    memcpy(buf + 0x100u, src, (size_t)payload_size);

    {
        static const uint8_t key_0001_2500[] =
            "0001nado2500achi0001gp322500bing0001yang2500home0001sist"
            "2500know0001jsta2500oos!0001gun72500ehye";
        static const uint8_t key_0003_2500[] =
            "0003nado2500achi0003gp322500bing0003yang2500home0003sist"
            "2500know0003jsta2500oos!0003gun72500ehye";
        static const uint8_t key_0001_3000[] =
            "0001yang3000home0001sist3000know0001jsta3000oos!"
            "0001gun73000ehye0001nado3000achi0001gp323000bing";
        static const uint8_t key_0003_3000[] =
            "0003nado3000achi0003gp323000bing0003yang3000home0003sist"
            "3000know0003jsta3000oos!0003gun73000ehye";
        static const gxc_key_profile_t strip_keys[] = {
            { "0001/2500", key_0001_2500, sizeof(key_0001_2500) - 1u },
            { "0003/2500", key_0003_2500, sizeof(key_0003_2500) - 1u },
            { "0001/3000", key_0001_3000, sizeof(key_0001_3000) - 1u },
            { "0003/3000", key_0003_3000, sizeof(key_0003_3000) - 1u },
        };
        static const size_t strip_chunks[] = { 0x0c00u, 0x1400u, 0x1800u, 0x1c00u, 0x2c00u, 0x3000u };
        char family_id[5] = {0};
        char key_id[5] = {0};
        int base_score = gxc_candidate_score(buf, total);
        uint8_t *best = NULL;
        int best_score = base_score;
        int applied_descriptor_profile = 0;

        if (gxe && gxe->data && gxe->size >= 12u) {
            for (size_t i = 0u; i < 4u; ++i) {
                unsigned char c0 = gxe->data[4u + i];
                unsigned char c1 = gxe->data[8u + i];
                family_id[i] = (c0 >= '0' && c0 <= '9') ? (char)c0 : '\0';
                key_id[i] = (c1 >= '0' && c1 <= '9') ? (char)c1 : '\0';
            }
            if (key_id[0]) best_score = -0x7fffffff;
        }

        /*
         * Stripped commercial GXCs keep the first launcher chunk in clear text
         * and encrypt the first 0x100 bytes of each following stride.  BIOS
         * decrypts those slices before branching into the C runtime; direct HLE
         * must do the same or later SDK thunks fall through random data.  The
         * GXE descriptor carries the family/key ids in bytes 4..11; prefer those
         * descriptor-level profiles when known, and fall back to scoring only for
         * unrecognised variants.
         */
        if (family_id[0] && key_id[0]) {
            if (strncmp(family_id, "1001", 4u) == 0 && strncmp(key_id, "0001", 4u) == 0) {
                gxc_decrypt_periodic_from(buf, total, &strip_keys[0], 0x2c00u, 0u, 0x2c00u);
                applied_descriptor_profile = 1;
            } else if (strncmp(family_id, "1001", 4u) == 0 && strncmp(key_id, "0003", 4u) == 0) {
                gxc_decrypt_periodic_from(buf, total, &strip_keys[1], 0x0c00u, 8u, 0x0c00u);
                applied_descriptor_profile = 1;
            } else if (strncmp(family_id, "1003", 4u) == 0 && strncmp(key_id, "0001", 4u) == 0) {
                gxc_decrypt_periodic_from(buf, total, &strip_keys[2], 0x1400u, 40u, 0x1400u);
                applied_descriptor_profile = 1;
            }
        }

        if (!applied_descriptor_profile) {
            for (size_t ci = 0u; ci < GP32_ARRAY_COUNT(strip_chunks); ++ci) {
                size_t chunk = strip_chunks[ci];
                for (size_t ki = 0u; ki < GP32_ARRAY_COUNT(strip_keys); ++ki) {
                    if (key_id[0] && strncmp(strip_keys[ki].name, key_id, 4u) != 0) continue;
                    for (size_t key_start = 0u; key_start < strip_keys[ki].key_len; ++key_start) {
                        uint8_t *cand = (uint8_t *)malloc(total ? total : 1u);
                        if (!cand) { free(best); free(buf); return -1; }
                        memcpy(cand, buf, total);
                        gxc_decrypt_periodic_from(cand, total, &strip_keys[ki], chunk, key_start, chunk);
                        int score = gxc_candidate_score(cand, total) + gxc_candidate_arm_score_range(cand, total, chunk, chunk);
                        if (score > best_score) {
                            free(best);
                            best = cand;
                            best_score = score;
                        } else {
                            free(cand);
                        }
                    }
                }
            }
            if (best && (key_id[0] || best_score > base_score + 64)) {
                free(buf);
                buf = best;
            } else {
                free(best);
            }
        }
    }

    *out = buf;
    *out_size = total;
    return 1;
}


static int gxc_rebuild_xor256_selfloader(const smc_file_buf_t *gxc, uint32_t payload_size, uint8_t **out, size_t *out_size) {
    static const uint8_t loader_sig[256] = {
        0x00,0x00,0xc7,0xe7,0x01,0x00,0x50,0xe2,0xfc,0xff,0xff,0x5a,0x01,0x26,0xa0,0xe3,
        0x02,0x3c,0xa0,0xe3,0xd8,0x40,0x8d,0xe2,0x33,0x00,0x00,0xeb,0x37,0x00,0x00,0xeb,
        0x01,0x3c,0xa0,0xe3,0x97,0x4e,0x8d,0xe2,0x2f,0x00,0x00,0xeb,0x33,0x00,0x00,0xeb,
        0xe1,0x4e,0x8d,0xe2,0x2c,0x00,0x00,0xeb,0x30,0x00,0x00,0xeb,0xd8,0x20,0x8d,0xe2,
        0x18,0x00,0x00,0xeb,0x01,0x3c,0x52,0xe2,0x13,0x00,0x00,0x4a,0xd1,0xff,0xff,0x0a,
        0x01,0x30,0x53,0xe2,0x05,0x30,0xa0,0x01,0x06,0x40,0xa0,0x01,0x0c,0x00,0x00,0x0a,
        0x03,0x50,0xa0,0xe1,0x97,0x2e,0x8d,0xe2,0x0e,0x00,0x00,0xeb,0x00,0x00,0x52,0xe3,
        0x07,0x40,0xa0,0x01,0x05,0x00,0x00,0x0a,0x7f,0x40,0x02,0xe2,0x02,0x00,0x54,0xe1,
        0xe1,0x2e,0x8d,0x12,0x07,0x00,0x00,0x1b,0x04,0x44,0x82,0x11,0x04,0x70,0xa0,0x11,
        0x04,0x60,0xa0,0xe1,0x04,0x20,0x59,0xe7,0x01,0x30,0x53,0xe2,0x01,0x20,0xc9,0xe4,
        0xfb,0xff,0xff,0x5a,0xe4,0xff,0xff,0xea,0x84,0x80,0x82,0xe2,0x08,0x10,0x92,0xe4,
        0x01,0x00,0x5b,0xe1,0xfc,0xff,0xff,0x8a,0xb2,0x10,0x52,0xe1,0xb4,0x20,0x52,0xe1,
        0x20,0x00,0x61,0xe2,0x3b,0x20,0x62,0xe0,0x02,0x21,0x98,0xe7,0x01,0xa0,0x5a,0xe0,
        0x3b,0x00,0xa0,0xe1,0x1b,0xb1,0xa0,0xe1,0xb2,0x10,0xdc,0xd0,0x00,0xa0,0x6a,0xd2,
        0x11,0xba,0x8b,0xd1,0x10,0xa0,0x7a,0xd2,0x0e,0xf0,0xa0,0xe1,0x84,0x10,0x84,0xe2,
        0x03,0x21,0x81,0xe7,0x00,0x50,0xa0,0xe3,0x04,0x50,0x01,0xe5,0x0e,0xf0,0xa0,0xe1
    };
    if (!gxc || !out || !out_size || gxc->size <= 20u) return 0;
    size_t raw_payload_size = gxc->size - 20u;
    if (raw_payload_size < 0x400u || (payload_size && (size_t)payload_size > raw_payload_size)) return 0;
    const uint8_t *src = gxc->data + 20u;
    uint8_t key[256];
    for (size_t i = 0u; i < sizeof(key); ++i) key[i] = (uint8_t)(src[i] ^ loader_sig[i]);
    size_t loader_size = raw_payload_size;
    uint8_t *loader = (uint8_t *)malloc(loader_size ? loader_size : 1u);
    if (!loader) return -1;
    size_t xor_len = loader_size & ~(size_t)0xffu;
    for (size_t i = 0u; i < loader_size; ++i) {
        /* The commercial XOR256 self-loader encrypts complete 0x100-byte
         * blocks.  A final short block is stored as plaintext; XORing it
         * corrupts the FXP3 stream footer and makes the decompressor run past
         * the real end marker. */
        loader[i] = (i < xor_len) ? (uint8_t)(src[i] ^ key[i & 0xffu]) : src[i];
    }
    if (memcmp(loader, loader_sig, sizeof(loader_sig)) != 0 || gp32_ld32le(loader + 0x100u) != 0xe92d4004u || gp32_ld32le(loader + 0x104u) != 0xe3a05000u) {
        free(loader);
        return 0;
    }
    if (gxc_candidate_score(loader, loader_size) < 512) {
        free(loader);
        return 0;
    }

    /*
     * Some commercial GXCs are not encrypted GXB images.  The firmware places a
     * 256-byte-XOR-decoded FXP3 self-loader behind a BIOS-supplied 0x100-byte
     * launcher prefix.  v45 only wrapped the decoded body and jumped to +0x218,
     * which left the decompressor's bitstream/output registers uninitialised and
     * made it spin at the Huffman decode compare loop.  Rebuild the missing
     * generic launcher prefix instead: it copies the compressed tail to high RAM,
     * decrunches a real GXB to a safe work address, then SWI #5 tail-calls that
     * GXB through the normal HLE firmware path.
     */
    const uint32_t rom = 0x0c000000u;
    const uint32_t code_off = 0x100u;
    const uint32_t packed_stream_off = 0x348u;
    if (loader_size <= packed_stream_off + 8u || memcmp(loader + packed_stream_off - code_off, "FXP3", 4u) != 0) {
        free(loader);
        return 0;
    }
    uint32_t unpacked_size = gp32_ld32le(loader + packed_stream_off - code_off + 4u);
    uint32_t out_addr = (unpacked_size > 0x00100000u) ? 0x0c140000u : 0x0c040000u;
    uint32_t source_end = code_off + (uint32_t)loader_size;
    uint32_t source_end_plus8 = source_end + 8u;
    size_t total = code_off + loader_size;
    if (total < 0x1000u) total = 0x1000u;
    uint8_t *buf = (uint8_t *)calloc(1u, total ? total : 1u);
    if (!buf) { free(loader); return -1; }

    /* Firmware FXP3 launcher header.  Only game-size/address fields vary. */
    gp32_st32le(buf + 0x00u, 0xea00000eu); /* b +0x40 */
    gp32_st32le(buf + 0x04u, rom);
    gp32_st32le(buf + 0x08u, rom + 0x348u);
    gp32_st32le(buf + 0x0cu, rom + 0x348u);
    gp32_st32le(buf + 0x10u, rom + source_end_plus8);
    gp32_st32le(buf + 0x14u, rom + source_end);
    gp32_st32le(buf + 0x18u, rom + source_end_plus8);
    gp32_st32le(buf + 0x1cu, 0x44450011u);
    gp32_st32le(buf + 0x20u, 0x44450011u);
    gp32_st32le(buf + 0x34u, out_addr);

    static const uint32_t prefix_code[] = {
        0xe51f0010u, 0xe51f1010u, 0xe3a02003u, 0xe92d0007u,
        0xe1a0000du, 0xe3510000u, 0x1f00000du, 0xe3a00005u,
        0xef00000bu, 0xe3c0c003u, 0xe51fb064u, 0xe51f9060u,
        0xe049b00bu, 0xe539a004u, 0xe25bb004u, 0x152ca004u,
        0x1afffffbu, 0xe51f9058u, 0xe49c0008u, 0xe4890004u,
        0xe24ddc13u, 0xe3a0a010u, 0xe35a0010u, 0x024cc004u,
        0x124cc002u, 0xe0dc80b2u, 0xe3580000u, 0x0a000099u,
        0xe0dcb0b2u, 0xe3a0a010u, 0xe0dc10b2u, 0xe181b80bu,
        0xe3a02702u, 0xe3a03010u, 0xe1a0400du, 0xeb000046u,
        0xe28d6084u, 0xe3a01003u, 0xe2610020u, 0xeb00003au,
        0xe1851800u, 0xe4861004u, 0xe2855001u, 0xe1550003u,
        0x3afffff7u, 0xeb000055u, 0xe3a0000fu, 0xe28d70c8u
    };
    for (size_t i = 0u; i < GP32_ARRAY_COUNT(prefix_code); ++i) gp32_st32le(buf + 0x40u + (uint32_t)i * 4u, prefix_code[i]);
    memcpy(buf + code_off, loader, loader_size);
    free(loader);
    *out = buf;
    *out_size = total;
    return 1;
}

static int decrypt_commercial_gxc(const smc_file_buf_t *gxe, const smc_file_buf_t *gxc, uint8_t **out, size_t *out_size, char *err, size_t err_len) {
    static const uint8_t key_0001[] =
        "0001yang3000home0001sist3000know0001jsta3000oos!"
        "0001gun73000ehye0001nado3000achi0001gp323000bing";
    static const uint8_t key_1023[] =
        "1023mola1202joaa1023game1202park1023go321202kigo"
        "1023nopa1202babo102377211202prom1023jang12021cut";
    static const uint8_t key_1009_0604[] =
        "06041cut1009mola0604joaa1009game0604park1009go320604kigo"
        "1009nopa0604babo100977210604prom1009jang";
    static const uint8_t key_1001_1110[] =
        "1110prom1001jang11101cut1001mola1110joaa1001game1110park"
        "1001go321110kigo1001nopa1110babo10017721";
    static const uint8_t key_1002_1127[] =
        "1002jang11271cut1002mola1127joaa1002game1127park1002go32"
        "1127kigo1002nopa1127babo100277211127prom";
    static const uint8_t key_1010_0605[] =
        "0605kigo1010nopa0605babo101077210605prom1010jang06051cut"
        "1010mola0605joaa1010game0605park1010go32";
    static const uint8_t key_1008_0319[] =
        "1008game0319park1008go320319kigo1008nopa0319babo10087721"
        "0319prom1008jang03191cut1008mola0319joaa";
    static const uint8_t key_1008_0428[] =
        "0428park1008go320428kigo1008nopa0428babo100877210428prom"
        "1008jang04281cut1008mola0428joaa1008game";
    static const uint8_t key_1006_0125[] =
        "1006nopa0125babo100677210125prom1006jang01251cut1006mola"
        "0125joaa1006game0125park1006go320125kigo";
    static const uint8_t key_1018_0327[] =
        "1018game0327park1018go320327kigo1018nopa0327babo10187721"
        "0327prom1018jang03271cut1018mola0327joaa";
    static const uint8_t key_1111[] =
        "1111nopa1111babo111177211111prom1111jang11111cut1111mola"
        "1111joaa1111game1111park1111go321111kigo";
    static const uint8_t key_0003_3000[] =
        "0003nado3000achi0003gp323000bing0003yang3000home0003sist"
        "3000know0003jsta3000oos!0003gun73000ehye";
    static const uint8_t key_0002_3000[] =
        "3000achi0002gp323000bing0002yang3000home0002sist3000know"
        "0002jsta3000oos!0002gun73000ehye0002nado";
    static const uint8_t key_0004_2500[] =
        "0004gun72500ehye0004nado2500achi0004gp322500bing0004yang"
        "2500home0004sist2500know0004jsta2500oos!";
    static const uint8_t key_0001_2500[] =
        "0001nado2500achi0001gp322500bing0001yang2500home0001sist"
        "2500know0001jsta2500oos!0001gun72500ehye";
    static const uint8_t key_0003_2500[] =
        "0003nado2500achi0003gp322500bing0003yang2500home0003sist"
        "2500know0003jsta2500oos!0003gun72500ehye";
    static const gxc_key_profile_t keys[] = {
        { "0001/3000", key_0001, sizeof(key_0001) - 1u },
        { "1023/1202", key_1023, sizeof(key_1023) - 1u },
        { "1009/0604", key_1009_0604, sizeof(key_1009_0604) - 1u },
        { "1001/1110", key_1001_1110, sizeof(key_1001_1110) - 1u },
        { "1002/1127", key_1002_1127, sizeof(key_1002_1127) - 1u },
        { "1010/0605", key_1010_0605, sizeof(key_1010_0605) - 1u },
        { "1008/0319", key_1008_0319, sizeof(key_1008_0319) - 1u },
        { "1008/0428", key_1008_0428, sizeof(key_1008_0428) - 1u },
        { "1006/0125", key_1006_0125, sizeof(key_1006_0125) - 1u },
        { "1018/0327", key_1018_0327, sizeof(key_1018_0327) - 1u },
        { "1111/1111", key_1111, sizeof(key_1111) - 1u },
        { "0003/3000", key_0003_3000, sizeof(key_0003_3000) - 1u },
        { "0002/3000", key_0002_3000, sizeof(key_0002_3000) - 1u },
        { "0004/2500", key_0004_2500, sizeof(key_0004_2500) - 1u },
        { "0001/2500", key_0001_2500, sizeof(key_0001_2500) - 1u },
        { "0003/2500", key_0003_2500, sizeof(key_0003_2500) - 1u },
    };
    if (!gxc || !out || !out_size) return 0;
    *out = NULL;
    *out_size = 0;
    if (gxc->size < 20u) { serr(err, err_len, "%s is too small for a GXC header", gxc->path); return 0; }
    uint32_t payload_size = gxc_payload_size_bytes(gxc);

    /*
     * Commercial GXC is a 20-byte header plus a GXB-compatible payload whose
     * encrypted span is selected by the publisher's GXE encryption profile.
     * The firmware XORs the first 0x100 bytes of each encrypted span, but the
     * span stride differs by profile: the validated 0001/3000 Korean profile
     * uses 0x1c00-byte spans, while the validated 1023/1202 European profile
     * uses 0x1800-byte spans.  Try the BIOS key/profile set in GXE-profile order
     * and accept only candidates that reconstruct a valid GXB scatter header.
     */
    static const size_t mode_profile_1[] = { 0x1800u, 0x0c00u, 0x1400u, 0x1c00u, 0x2c00u, 0u, 0x0400u, 0x0800u, 0x1000u, 0x2000u, 0x2400u, 0x2800u, 0x3000u, 0x3800u };
    static const size_t mode_profile_4[] = { 0x1c00u, 0x1800u, 0x0c00u, 0x1400u, 0x2c00u, 0u, 0x0400u, 0x0800u, 0x1000u, 0x2000u, 0x2400u, 0x2800u, 0x3000u, 0x3800u };
    const unsigned profile = gxe_encryption_profile(gxe);
    const size_t *modes = (profile == 1u) ? mode_profile_1 : mode_profile_4;
    const size_t mode_count = (profile == 1u) ? (sizeof(mode_profile_1) / sizeof(mode_profile_1[0])) : (sizeof(mode_profile_4) / sizeof(mode_profile_4[0]));
    uint8_t *best = NULL;
    size_t best_size = 0u;
    int best_score = -0x7fffffff;
    for (size_t pass = 0; pass < mode_count; ++pass) {
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            for (size_t key_start = 0u; key_start < keys[i].key_len; ++key_start) {
                uint8_t *cand = NULL;
                size_t cand_size = 0u;
                int cand_score = 0;
                int r = gxc_try_decrypt(gxc, payload_size, &keys[i], modes[pass], key_start, &cand, &cand_size, &cand_score);
                if (r < 0) { free(best); serr(err, err_len, "out of memory decrypting %s", gxc->path); return 0; }
                if (r > 0) {
                    if (!best || cand_score > best_score) {
                        free(best);
                        best = cand;
                        best_size = cand_size;
                        best_score = cand_score;
                    } else {
                        free(cand);
                    }
                }
            }
        }
    }
    if (best) {
        *out = best;
        *out_size = best_size;
        return 1;
    }

    {
        uint8_t *rebuilt = NULL;
        size_t rebuilt_size = 0u;
        int rr = gxc_rebuild_stripped_gxb(gxe, gxc, payload_size, &rebuilt, &rebuilt_size);
        if (rr < 0) { serr(err, err_len, "out of memory rebuilding stripped GXC %s", gxc->path); return 0; }
        if (rr > 0) {
            *out = rebuilt;
            *out_size = rebuilt_size;
            return 1;
        }
    }

    {
        uint8_t *rebuilt = NULL;
        size_t rebuilt_size = 0u;
        int rr = gxc_rebuild_xor256_selfloader(gxc, payload_size, &rebuilt, &rebuilt_size);
        if (rr < 0) { serr(err, err_len, "out of memory rebuilding XOR256 self-loader GXC %s", gxc->path); return 0; }
        if (rr > 0) {
            *out = rebuilt;
            *out_size = rebuilt_size;
            return 1;
        }
    }

    serr(err, err_len, "GXC decrypt did not produce a valid GXB header for %s", gxc->path);
    return 0;
}

static int move_files_to_assets(smc_file_list_t *list, fpk_asset_t **out_assets, size_t *out_count, char *err, size_t err_len) {
    if (!list || !out_assets || !out_count) return 0;
    *out_assets = NULL;
    *out_count = 0;
    fpk_asset_t *assets = (fpk_asset_t *)calloc(list->count ? list->count : 1u, sizeof(*assets));
    if (!assets) { serr(err, err_len, "out of memory moving SMC assets"); return 0; }
    for (size_t i = 0; i < list->count; ++i) {
        snprintf(assets[i].path, sizeof(assets[i].path), "%s", list->items[i].path);
        assets[i].data = list->items[i].data;
        assets[i].size = list->items[i].size;
        assets[i].first_cluster = list->items[i].first_cluster;
        assets[i].attr = list->items[i].attr;
        list->items[i].data = NULL;
    }
    *out_assets = assets;
    *out_count = list->count;
    return 1;
}

void smc_direct_package_free(smc_direct_package_t *pkg) {
    if (!pkg) return;
    fxe_image_free(&pkg->image);
    for (size_t i = 0; i < pkg->asset_count; ++i) free(pkg->assets[i].data);
    free(pkg->assets);
    memset(pkg, 0, sizeof(*pkg));
}

static int smc_direct_load_image_owned(uint8_t *img, size_t img_size, const char *source_label, smc_direct_package_t *pkg, char *err, size_t err_len) {
    if (!img || !pkg) { free(img); serr(err, err_len, "invalid SMC direct-load image"); return 0; }
    memset(pkg, 0, sizeof(*pkg));

    fat_view_t fv;
    if (!fat_find(img, img_size, &fv)) { free(img); serr(err, err_len, "no FAT12/FAT16 filesystem found in SMC image"); return 0; }
    smc_file_list_t list;
    memset(&list, 0, sizeof(list));
    if (!fat_extract_all(&fv, &list, err, err_len)) { file_list_free(&list); free(img); return 0; }
    free(img);
    if (!list.count) { file_list_free(&list); serr(err, err_len, "SMC filesystem contains no files"); return 0; }

    const smc_file_buf_t *gxe = find_first_ext(&list, ".gxe");
    const smc_file_buf_t *gxb = find_first_ext(&list, ".gxb");
    const smc_file_buf_t *fxe = find_first_ext(&list, ".fxe");
    uint8_t *exec_data = NULL;
    size_t exec_size = 0;
    const char *exec_label = source_label;

    if (gxe) {
        const smc_file_buf_t *gxc = find_matching_gxc(&list, gxe->path);
        if (!gxc) { file_list_free(&list); serr(err, err_len, "found %s but no matching .GXC payload", gxe->path); return 0; }
        if (!decrypt_commercial_gxc(gxe, gxc, &exec_data, &exec_size, err, err_len)) {
            file_list_free(&list);
            return 0;
        }
        exec_label = gxc->path;
        snprintf(pkg->executable_path, sizeof(pkg->executable_path), "%s", gxe->path);
        copy_str_trunc(pkg->title, sizeof(pkg->title), gxe->path);
    } else if (gxb) {
        exec_data = (uint8_t *)malloc(gxb->size ? gxb->size : 1u);
        if (!exec_data) { file_list_free(&list); serr(err, err_len, "out of memory loading %s", gxb->path); return 0; }
        memcpy(exec_data, gxb->data, gxb->size);
        exec_size = gxb->size;
        exec_label = gxb->path;
        snprintf(pkg->executable_path, sizeof(pkg->executable_path), "%s", gxb->path);
    } else if (fxe) {
        exec_data = (uint8_t *)malloc(fxe->size ? fxe->size : 1u);
        if (!exec_data) { file_list_free(&list); serr(err, err_len, "out of memory loading %s", fxe->path); return 0; }
        memcpy(exec_data, fxe->data, fxe->size);
        exec_size = fxe->size;
        exec_label = fxe->path;
        snprintf(pkg->executable_path, sizeof(pkg->executable_path), "%s", fxe->path);
    } else {
        file_list_free(&list);
        serr(err, err_len, "SMC filesystem contains no .GXE/.GXB/.FXE executable");
        return 0;
    }

    if (!fxe_load_buffer(exec_data, exec_size, exec_label ? exec_label : "buffer", &pkg->image, err, err_len)) {
        free(exec_data);
        file_list_free(&list);
        return 0;
    }
    free(exec_data);

    if (!move_files_to_assets(&list, &pkg->assets, &pkg->asset_count, err, err_len)) {
        smc_direct_package_free(pkg);
        file_list_free(&list);
        return 0;
    }
    file_list_free(&list);
    return 1;
}

int smc_direct_load_file(const char *path, smc_direct_package_t *pkg, char *err, size_t err_len) {
    if (!path || !pkg) { serr(err, err_len, "invalid SMC direct-load arguments"); return 0; }
    size_t raw_size = 0, img_size = 0;
    uint8_t *raw = read_file_or_zip(path, &raw_size, err, err_len);
    if (!raw) return 0;
    uint8_t *img = NULL;
    if (!smc_reconstruct_512(raw, raw_size, &img, &img_size, err, err_len)) { free(raw); return 0; }
    free(raw);
    return smc_direct_load_image_owned(img, img_size, path, pkg, err, err_len);
}

int smc_direct_load_buffer(const uint8_t *data, size_t size, const char *label, smc_direct_package_t *pkg, char *err, size_t err_len) {
    if (!data || !size || !pkg) { serr(err, err_len, "invalid SMC direct-load buffer arguments"); return 0; }
    uint8_t *raw = (uint8_t *)malloc(size);
    if (!raw) { serr(err, err_len, "out of memory reading %s", label ? label : "buffer"); return 0; }
    memcpy(raw, data, size);
    uint8_t *img = NULL;
    size_t img_size = 0;
    if (!smc_reconstruct_512(raw, size, &img, &img_size, err, err_len)) { free(raw); return 0; }
    free(raw);
    return smc_direct_load_image_owned(img, img_size, label ? label : "buffer", pkg, err, err_len);
}

