#include "zip.h"
#include "third_party/zip/zip.h"

static void zerr(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

int gp32_zip_path_maybe(const char *path) {
    if (!path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t sig[4] = {0, 0, 0, 0};
    size_t n = fread(sig, 1, sizeof(sig), f);
    fclose(f);
    return n == sizeof(sig) && gp32_ld32le(sig) == 0x04034b50u;
}

static int zip_name_is_root(const char *name) {
    if (!name || !*name) return 0;
    for (const char *p = name; *p; ++p) {
        if (*p == '/' || *p == '\\') return 0;
    }
    return 1;
}

static int zip_name_safe(const char *name) {
    if (!name || !*name) return 0;
    if (name[0] == '/' || name[0] == '\\') return 0;
    if (isalpha((unsigned char)name[0]) && name[1] == ':') return 0;
    const char *p = name;
    while (*p) {
        if ((p[0] == '.' && p[1] == '.' && (p[2] == '\0' || p[2] == '/' || p[2] == '\\')) ||
            ((p[0] == '/' || p[0] == '\\') && p[1] == '.' && p[2] == '.' &&
             (p[3] == '\0' || p[3] == '/' || p[3] == '\\'))) {
            return 0;
        }
        ++p;
    }
    return 1;
}

static const char *zip_basename(const char *name) {
    const char *base = name ? name : "";
    for (const char *p = base; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

static int zip_ends_ci(const char *s, const char *suffix) {
    size_t ns = strlen(s);
    size_t nf = strlen(suffix);
    if (ns < nf) return 0;
    s += ns - nf;
    for (size_t i = 0; i < nf; ++i) {
        int a = toupper((unsigned char)s[i]);
        int b = toupper((unsigned char)suffix[i]);
        if (a != b) return 0;
    }
    return 1;
}

static int zip_name_matches(const char *name, const char * const *exts, size_t ext_count) {
    const char *base = zip_basename(name);
    if (!base[0] || base[0] == '.') return 0;
    for (size_t i = 0; i < ext_count; ++i) {
        if (zip_ends_ci(base, exts[i])) return 1;
    }
    return 0;
}

static int zip_entry_matches_current(struct zip_t *zip,
                                     int root_pass,
                                     const char * const *exts,
                                     size_t ext_count,
                                     char *match_name,
                                     size_t match_name_len) {
    const char *name = zip_entry_name(zip);
    if (!name || zip_entry_isdir(zip) != 0) return 0;
    if (!zip_name_safe(name)) return 0;
    if (root_pass && !zip_name_is_root(name)) return 0;
    if (!zip_name_matches(name, exts, ext_count)) return 0;
    if (match_name && match_name_len) snprintf(match_name, match_name_len, "%s", name);
    return 1;
}

int gp32_zip_read_first_matching(const char *zip_path,
                                 const char * const *exts,
                                 size_t ext_count,
                                 uint8_t **out_data,
                                 size_t *out_size,
                                 char *out_name,
                                 size_t out_name_len,
                                 char *err,
                                 size_t err_len) {
    if (!zip_path || !exts || ext_count == 0 || !out_data || !out_size) {
        zerr(err, err_len, "invalid ZIP arguments");
        return 0;
    }
    *out_data = NULL;
    *out_size = 0;
    if (out_name && out_name_len) out_name[0] = '\0';

    int open_err = 0;
    struct zip_t *zip = zip_openwitherror(zip_path, 0, 'r', &open_err);
    if (!zip) {
        const char *msg = zip_strerror(open_err);
        zerr(err, err_len, "open ZIP %s: %s", zip_path, msg ? msg : "unknown error");
        return 0;
    }

    ssize_t total = zip_entries_total(zip);
    if (total < 0) {
        const char *msg = zip_strerror((int)total);
        zerr(err, err_len, "read ZIP directory %s: %s", zip_path, msg ? msg : "unknown error");
        zip_close(zip);
        return 0;
    }

    char match_name[1024];
    match_name[0] = '\0';
    int found_open_entry = 0;
    for (int pass = 0; pass < 2 && !found_open_entry; ++pass) {
        for (ssize_t i = 0; i < total; ++i) {
            int zr = zip_entry_openbyindex(zip, (size_t)i);
            if (zr < 0) continue;
            if (zip_entry_matches_current(zip, pass == 0, exts, ext_count, match_name, sizeof(match_name))) {
                found_open_entry = 1;
                break;
            }
            zip_entry_close(zip);
        }
    }

    if (!found_open_entry) {
        zerr(err, err_len, "ZIP contains no supported GP32 file at root or in subfolders");
        zip_close(zip);
        return 0;
    }

    void *buf = NULL;
    size_t buf_size = 0;
    ssize_t got = zip_entry_read(zip, &buf, &buf_size);
    if (got < 0) {
        const char *msg = zip_strerror((int)got);
        zerr(err, err_len, "extract ZIP entry %s: %s", match_name[0] ? match_name : "<unknown>", msg ? msg : "unknown error");
        zip_entry_close(zip);
        zip_close(zip);
        return 0;
    }
    if ((size_t)got != buf_size) buf_size = (size_t)got;
    zip_entry_close(zip);
    zip_close(zip);

    *out_data = (uint8_t *)buf;
    *out_size = buf_size;
    if (out_name && out_name_len) snprintf(out_name, out_name_len, "%s", match_name);
    return 1;
}
