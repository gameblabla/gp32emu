#include "fpk.h"
#include "zip.h"

static void ferr(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static uint8_t *read_file(const char *path, size_t *out_size, char *err, size_t err_len) {
    if (gp32_zip_path_maybe(path)) {
        uint8_t *zip_payload = NULL;
        char entry_name[260] = {0};
        static const char * const exts[] = { ".fpk" };
        if (!gp32_zip_read_first_matching(path, exts, GP32_ARRAY_COUNT(exts), &zip_payload, out_size, entry_name, sizeof(entry_name), err, err_len)) return NULL;
        GP32_UNUSED(entry_name);
        return zip_payload;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { ferr(err, err_len, "open %s: %s", path, strerror(errno)); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { ferr(err, err_len, "seek %s failed", path); fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { ferr(err, err_len, "empty or unreadable %s", path); fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { ferr(err, err_len, "out of memory reading %s", path); fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { ferr(err, err_len, "read %s failed", path); free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

static int ends_ci(const char *s, const char *suffix) {
    size_t ns = strlen(s), nf = strlen(suffix);
    if (ns < nf) return 0;
    s += ns - nf;
    for (size_t i = 0; i < nf; ++i) {
        int a = toupper((unsigned char)s[i]);
        int b = toupper((unsigned char)suffix[i]);
        if (a != b) return 0;
    }
    return 1;
}

static void copy_field(char *dst, size_t dst_len, const uint8_t *src, size_t src_len) {
    if (!dst || dst_len == 0) return;
    size_t n = 0;
    while (n + 1u < dst_len && n < src_len && src[n]) { dst[n] = (char)src[n]; ++n; }
    dst[n] = '\0';
}

static void make_joined_path(char *dst, size_t dst_len, const char *dir, const char *name) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    const char *d = dir ? dir : "";
    const char *n = name ? name : "";
    size_t j = 0;
    for (size_t i = 0; d[i] && j + 1u < dst_len; ++i) dst[j++] = d[i];
    if (j && dst[j - 1u] != '\\' && dst[j - 1u] != '/' && j + 1u < dst_len) dst[j++] = '\\';
    for (size_t i = 0; n[i] && j + 1u < dst_len; ++i) dst[j++] = n[i];
    dst[j] = '\0';
}


void fpk_package_free(fpk_package_t *pkg) {
    if (!pkg) return;
    free(pkg->fxe_data);
    for (size_t i = 0; i < pkg->asset_count; ++i) free(pkg->assets[i].data);
    free(pkg->assets);
    memset(pkg, 0, sizeof(*pkg));
}

static int fpk_parse_owned_buffer(uint8_t *buf, size_t size, const char *label, fpk_package_t *pkg, char *err, size_t err_len) {
    if (!buf || !pkg) { ferr(err, err_len, "invalid FPK arguments"); free(buf); return 0; }
    memset(pkg, 0, sizeof(*pkg));
    if (size < 0x90u || memcmp(buf, "GPKG", 4u) != 0) { ferr(err, err_len, "%s is not a GP32 GPKG/FPK package", label ? label : "buffer"); free(buf); return 0; }
    copy_field(pkg->title, sizeof(pkg->title), buf + 8u, size > 8u ? (size_t)8u : 0u);

    /*
     * GPKG/FPK packages have a variable-length install/root path in the
     * header. Older test packages used "gp:" and therefore placed the entry
     * count at 0x83 and the first entry at 0x87. Full GP Manager packages can
     * use longer roots such as "gp:\\gpmm", which shifts the count/table
     * accordingly. The two-byte length at 0x79 includes the terminating NUL.
     */
    size_t off = 0;
    if (size > 0x7bu) {
        uint32_t root_len = gp32_ld16le(buf + 0x79u);
        size_t count_off = 0x7bu + (size_t)root_len + 4u;
        size_t table_off = count_off + 4u;
        if (root_len < 0x1000u && table_off < size && (buf[table_off] == '0' || buf[table_off] == '1' || buf[table_off] == '2')) {
            off = table_off;
        }
    }
    if (!off) {
        off = 0x87u;
        if (off >= size || (buf[off] != '0' && buf[off] != '1')) {
            uint32_t rel = (size >= 0x87u) ? gp32_ld32le(buf + 0x83u) : 0u;
            if (rel && rel < size && (buf[rel] == '0' || buf[rel] == '1')) off = rel;
            else { ferr(err, err_len, "cannot find FPK entry table"); free(buf); return 0; }
        }
    }

    while (off < size) {
        uint8_t type = buf[off++];
        if (type == '2') break;
        if (type != '0' && type != '1') { ferr(err, err_len, "bad FPK entry tag at 0x%zx", off - 1u); fpk_package_free(pkg); free(buf); return 0; }
        if (off >= size) { ferr(err, err_len, "truncated FPK entry"); fpk_package_free(pkg); free(buf); return 0; }
        uint8_t plen = buf[off++];
        if (off + plen > size) { ferr(err, err_len, "truncated FPK path"); fpk_package_free(pkg); free(buf); return 0; }
        char dir[260];
        copy_field(dir, sizeof(dir), buf + off, plen);
        off += plen;
        if (off >= size) { ferr(err, err_len, "truncated FPK entry name"); fpk_package_free(pkg); free(buf); return 0; }
        uint8_t nlen = buf[off++];
        if (off + nlen > size) { ferr(err, err_len, "truncated FPK entry name"); fpk_package_free(pkg); free(buf); return 0; }
        char name[260];
        copy_field(name, sizeof(name), buf + off, nlen);
        off += nlen;
        if (type == '0') continue;
        if (off + 4u > size) { ferr(err, err_len, "truncated FPK file size"); fpk_package_free(pkg); free(buf); return 0; }
        uint32_t file_size = gp32_ld32le(buf + off);
        off += 4u;
        if ((size_t)file_size > size - off) { ferr(err, err_len, "truncated FPK file payload"); fpk_package_free(pkg); free(buf); return 0; }
        char joined[260];
        make_joined_path(joined, sizeof(joined), dir, name);
        uint8_t *copy = (uint8_t *)malloc((size_t)file_size ? (size_t)file_size : 1u);
        if (!copy) { ferr(err, err_len, "out of memory extracting %s", name); fpk_package_free(pkg); free(buf); return 0; }
        if (file_size) memcpy(copy, buf + off, (size_t)file_size);
        if (ends_ci(name, ".fxe") && (!pkg->fxe_data || strstr(dir, "\\gpmm") || strstr(dir, "\\GPMM") || strstr(dir, "/gpmm") || strstr(dir, "/GPMM"))) {
            free(pkg->fxe_data);
            pkg->fxe_data = copy;
            pkg->fxe_size = (size_t)file_size;
            if (!pkg->title[0]) copy_field(pkg->title, sizeof(pkg->title), (const uint8_t *)(joined[0] ? joined : name), strlen(joined[0] ? joined : name));
        } else {
            fpk_asset_t *na = (fpk_asset_t *)realloc(pkg->assets, (pkg->asset_count + 1u) * sizeof(pkg->assets[0]));
            if (!na) { free(copy); ferr(err, err_len, "out of memory indexing FPK assets"); fpk_package_free(pkg); free(buf); return 0; }
            pkg->assets = na;
            fpk_asset_t *a = &pkg->assets[pkg->asset_count++];
            memset(a, 0, sizeof(*a));
            snprintf(a->path, sizeof(a->path), "%s", joined[0] ? joined : name);
            a->data = copy;
            a->size = (size_t)file_size;
        }
        off += file_size;
    }
    free(buf);
    if (!pkg->fxe_data) { ferr(err, err_len, "FPK contains no .fxe executable"); fpk_package_free(pkg); return 0; }
    return 1;
}


int fpk_load_package_file(const char *path, fpk_package_t *pkg, char *err, size_t err_len) {
    if (!path || !pkg) { ferr(err, err_len, "invalid FPK arguments"); return 0; }
    size_t size = 0;
    uint8_t *buf = read_file(path, &size, err, err_len);
    if (!buf) return 0;
    return fpk_parse_owned_buffer(buf, size, path, pkg, err, err_len);
}

int fpk_load_package_buffer(const uint8_t *data, size_t size, const char *label, fpk_package_t *pkg, char *err, size_t err_len) {
    if (!data || !size || !pkg) { ferr(err, err_len, "invalid FPK buffer arguments"); return 0; }
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) { ferr(err, err_len, "out of memory reading %s", label ? label : "buffer"); return 0; }
    memcpy(buf, data, size);
    return fpk_parse_owned_buffer(buf, size, label ? label : "buffer", pkg, err, err_len);
}

int fpk_load_main_fxe_file(const char *path, uint8_t **out_data, size_t *out_size, char *title, size_t title_len, char *err, size_t err_len) {
    if (!path || !out_data || !out_size) { ferr(err, err_len, "invalid FPK arguments"); return 0; }
    *out_data = NULL;
    *out_size = 0;
    fpk_package_t pkg;
    if (!fpk_load_package_file(path, &pkg, err, err_len)) return 0;
    if (title && title_len) snprintf(title, title_len, "%s", pkg.title[0] ? pkg.title : "FPK");
    *out_data = pkg.fxe_data;
    *out_size = pkg.fxe_size;
    pkg.fxe_data = NULL;
    pkg.fxe_size = 0;
    fpk_package_free(&pkg);
    return 1;
}
