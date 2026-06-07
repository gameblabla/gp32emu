#include "gp32_media.h"
#include "audio/gp32_audio_resampler.h"

typedef unsigned long mz_ulong;
int mz_compress2(unsigned char *pDest, mz_ulong *pDest_len, const unsigned char *pSource, mz_ulong source_len, int level);
#define MZ_OK 0

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GP32EMU_ENABLE_THREADS
#if defined(_WIN32) || defined(__unix__) || defined(__APPLE__)
#define GP32EMU_ENABLE_THREADS 1
#else
#define GP32EMU_ENABLE_THREADS 0
#endif
#endif

#if GP32EMU_ENABLE_THREADS
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif
#endif

#define GP32_RAW_LCD_W 240u
#define GP32_RAW_LCD_H 320u
#define ZMBV_BLOCK 16u
#define ZMBV_FMT_32BPP 8u
#define ZMBV_KEYFRAME 1u

static void seterr(char *err, size_t err_len, const char *msg) {
    if (err && err_len) {
        snprintf(err, err_len, "%s", msg ? msg : "unknown error");
    }
}

static void put_u16le(FILE *f, uint16_t v) { fputc((int)(v & 255u), f); fputc((int)((v >> 8) & 255u), f); }
static void put_u32le(FILE *f, uint32_t v) { put_u16le(f, (uint16_t)v); put_u16le(f, (uint16_t)(v >> 16)); }
static void put_u16be(FILE *f, uint16_t v) { fputc((int)((v >> 8) & 255u), f); fputc((int)(v & 255u), f); }
static void put_u32be(FILE *f, uint32_t v) { put_u16be(f, (uint16_t)(v >> 16)); put_u16be(f, (uint16_t)v); }
static void put_u64be(FILE *f, uint64_t v) { put_u32be(f, (uint32_t)(v >> 32)); put_u32be(f, (uint32_t)v); }

static uint32_t rgb_from_pixel(uint32_t p) { return p & 0x00ffffffu; }

int gp32_media_frame_to_bgra_320x240(const gp32_framebuffer_desc_t *fb, uint32_t *out_bgra) {
    if (!fb || !out_bgra || !fb->pixels_rgba8888 || !fb->width || !fb->height || fb->stride_pixels < fb->width) return 0;
    const uint32_t *src = fb->pixels_rgba8888;
    uint32_t stride = fb->stride_pixels;
    if (fb->width == GP32_MEDIA_LCD_W && fb->height == GP32_MEDIA_LCD_H) {
        for (uint32_t y = 0; y < GP32_MEDIA_LCD_H; ++y) {
            const uint32_t *row = src + (size_t)y * stride;
            uint32_t *dst = out_bgra + (size_t)y * GP32_MEDIA_LCD_W;
            for (uint32_t x = 0; x < GP32_MEDIA_LCD_W; ++x) dst[x] = rgb_from_pixel(row[x]);
        }
        return 1;
    }
    if (fb->width == GP32_RAW_LCD_W && fb->height == GP32_RAW_LCD_H) {
        for (uint32_t y = 0; y < GP32_MEDIA_LCD_H; ++y) {
            uint32_t sx = GP32_RAW_LCD_W - 1u - y;
            uint32_t *dst = out_bgra + (size_t)y * GP32_MEDIA_LCD_W;
            for (uint32_t x = 0; x < GP32_MEDIA_LCD_W; ++x) dst[x] = rgb_from_pixel(src[(size_t)x * stride + sx]);
        }
        return 1;
    }
    for (uint32_t y = 0; y < GP32_MEDIA_LCD_H; ++y) {
        uint32_t sy = (uint32_t)(((uint64_t)y * fb->height) / GP32_MEDIA_LCD_H);
        if (sy >= fb->height) sy = fb->height - 1u;
        const uint32_t *row = src + (size_t)sy * stride;
        uint32_t *dst = out_bgra + (size_t)y * GP32_MEDIA_LCD_W;
        for (uint32_t x = 0; x < GP32_MEDIA_LCD_W; ++x) {
            uint32_t sx = (uint32_t)(((uint64_t)x * fb->width) / GP32_MEDIA_LCD_W);
            if (sx >= fb->width) sx = fb->width - 1u;
            dst[x] = rgb_from_pixel(row[sx]);
        }
    }
    return 1;
}

int gp32_media_write_bmp_320x240(const char *path, const gp32_framebuffer_desc_t *fb, char *err, size_t err_len) {
    if (!path || !path[0] || !fb) { seterr(err, err_len, "invalid screenshot request"); return 0; }
    uint32_t *buf = (uint32_t *)malloc((size_t)GP32_MEDIA_LCD_W * GP32_MEDIA_LCD_H * sizeof(uint32_t));
    if (!buf) { seterr(err, err_len, "out of memory writing screenshot"); return 0; }
    if (!gp32_media_frame_to_bgra_320x240(fb, buf)) { free(buf); seterr(err, err_len, "invalid framebuffer for screenshot"); return 0; }
    FILE *f = fopen(path, "wb");
    if (!f) { char tmp[256]; snprintf(tmp, sizeof(tmp), "screenshot open failed: %s", strerror(errno)); free(buf); seterr(err, err_len, tmp); return 0; }
    const uint32_t data_bytes = GP32_MEDIA_LCD_W * GP32_MEDIA_LCD_H * 4u;
    const uint32_t header_bytes = 14u + 40u;
    fputc('B', f); fputc('M', f);
    put_u32le(f, header_bytes + data_bytes);
    put_u16le(f, 0); put_u16le(f, 0);
    put_u32le(f, header_bytes);
    put_u32le(f, 40u);
    put_u32le(f, GP32_MEDIA_LCD_W);
    put_u32le(f, (uint32_t)(-(int32_t)GP32_MEDIA_LCD_H));
    put_u16le(f, 1u);
    put_u16le(f, 32u);
    put_u32le(f, 0u);
    put_u32le(f, data_bytes);
    put_u32le(f, 2835u); put_u32le(f, 2835u);
    put_u32le(f, 0u); put_u32le(f, 0u);
    for (uint32_t y = 0; y < GP32_MEDIA_LCD_H; ++y) {
        const uint32_t *row = buf + (size_t)y * GP32_MEDIA_LCD_W;
        for (uint32_t x = 0; x < GP32_MEDIA_LCD_W; ++x) {
            uint32_t p = row[x];
            fputc((int)(p & 255u), f);
            fputc((int)((p >> 8) & 255u), f);
            fputc((int)((p >> 16) & 255u), f);
            fputc(0xff, f);
        }
    }
    int ok = ferror(f) == 0;
    fclose(f);
    free(buf);
    if (!ok) seterr(err, err_len, "screenshot write failed");
    return ok;
}

typedef struct membuf {
    uint8_t *p;
    size_t n;
    size_t cap;
} membuf_t;

static int mb_reserve(membuf_t *b, size_t add) {
    if (add > (size_t)-1 - b->n) return 0;
    size_t need = b->n + add;
    if (need <= b->cap) return 1;
    size_t cap = b->cap ? b->cap * 2u : 256u;
    while (cap < need) {
        if (cap > (size_t)-1 / 2u) { cap = need; break; }
        cap *= 2u;
    }
    uint8_t *p = (uint8_t *)realloc(b->p, cap);
    if (!p) return 0;
    b->p = p; b->cap = cap;
    return 1;
}
static int mb_put(membuf_t *b, const void *p, size_t n) { if (!mb_reserve(b, n)) return 0; memcpy(b->p + b->n, p, n); b->n += n; return 1; }
static int mb_u8(membuf_t *b, uint8_t v) { return mb_put(b, &v, 1); }
static int mb_u16le(membuf_t *b, uint16_t v) { uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; return mb_put(b, x, 2); }
static int mb_u32le(membuf_t *b, uint32_t v) { return mb_u16le(b, (uint16_t)v) && mb_u16le(b, (uint16_t)(v >> 16)); }
static int mb_u32be(membuf_t *b, uint32_t v) { uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; return mb_put(b, x, 4); }
static int mb_u64be(membuf_t *b, uint64_t v) { return mb_u32be(b, (uint32_t)(v >> 32)) && mb_u32be(b, (uint32_t)v); }

#if GP32EMU_ENABLE_THREADS
typedef enum gp32_rec_job_type { GP32_REC_JOB_FRAME = 1, GP32_REC_JOB_AUDIO = 2 } gp32_rec_job_type_t;
typedef struct gp32_rec_job {
    struct gp32_rec_job *next;
    gp32_rec_job_type_t type;
    size_t bytes;
    uint64_t frame_index;
    uint32_t sample_rate;
    uint32_t frame_count;
    union {
        uint32_t *frame_bgra;
        int16_t *audio_s16;
        void *ptr;
    } data;
} gp32_rec_job_t;
#endif

struct gp32_media_recorder {
    FILE *f;
    uint32_t audio_rate;
    uint64_t audio_frames_out;
    uint64_t current_cluster_ms;
    int cluster_open;
    char err[256];
    uint8_t *zwork;
    size_t zwork_cap;
    uint8_t *zcomp;
    size_t zcomp_cap;
    uint32_t *frame;
    int16_t *audio_tmp;
    size_t audio_tmp_cap;
    gp32_audio_resampler_t audio_resampler;
#if GP32EMU_ENABLE_THREADS
    int threaded;
    int worker_started;
    int stop_worker;
    int async_failed;
    size_t async_bytes;
    uint64_t dropped_video_frames;
    gp32_rec_job_t *job_head;
    gp32_rec_job_t *job_tail;
#if defined(_WIN32)
    HANDLE worker;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cond;
#else
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t cond;
#endif
#endif
};

static void rec_seterr(gp32_media_recorder_t *r, const char *msg) { if (r) snprintf(r->err, sizeof(r->err), "%s", msg ? msg : "unknown recorder error"); }
const char *gp32_media_recorder_error(const gp32_media_recorder_t *r) { return r && r->err[0] ? r->err : "recorder error"; }

static void ebml_id(FILE *f, uint32_t id) {
    if (id > 0x00ffffffu) { fputc((int)(id >> 24), f); fputc((int)(id >> 16), f); fputc((int)(id >> 8), f); fputc((int)id, f); }
    else if (id > 0x0000ffffu) { fputc((int)(id >> 16), f); fputc((int)(id >> 8), f); fputc((int)id, f); }
    else if (id > 0x000000ffu) { fputc((int)(id >> 8), f); fputc((int)id, f); }
    else fputc((int)id, f);
}
static void ebml_size(FILE *f, uint64_t n) {
    if (n < 0x7full) fputc((int)(0x80u | (uint8_t)n), f);
    else if (n < 0x3fffull) { uint16_t v = (uint16_t)(0x4000u | n); put_u16be(f, v); }
    else if (n < 0x1fffffull) { fputc((int)(0x20u | (uint8_t)(n >> 16)), f); put_u16be(f, (uint16_t)n); }
    else if (n < 0x0fffffffull) { put_u32be(f, (uint32_t)(0x10000000u | n)); }
    else if (n < 0x07ffffffffull) { fputc((int)(0x08u | (uint8_t)(n >> 32)), f); put_u32be(f, (uint32_t)n); }
    else if (n < 0x03ffffffffffull) { fputc((int)(0x04u | (uint8_t)(n >> 40)), f); fputc((int)(n >> 32), f); put_u32be(f, (uint32_t)n); }
    else if (n < 0x01ffffffffffffull) { fputc((int)(0x02u | (uint8_t)(n >> 48)), f); fputc((int)(n >> 40), f); fputc((int)(n >> 32), f); put_u32be(f, (uint32_t)n); }
    else { put_u64be(f, 0x0100000000000000ull | n); }
}
static void ebml_unknown_size8(FILE *f) { static const uint8_t u[8] = {0x01,0xff,0xff,0xff,0xff,0xff,0xff,0xff}; fwrite(u, 1, sizeof(u), f); }
static void ebml_elem(FILE *f, uint32_t id, const void *p, size_t n) { ebml_id(f, id); ebml_size(f, (uint64_t)n); fwrite(p, 1, n, f); }
static void ebml_uint(FILE *f, uint32_t id, uint64_t v) {
    uint8_t b[8]; size_t n = 1;
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> ((7 - i) * 8));
    while (n < 8 && b[8 - n - 1] != 0) n++;
    ebml_elem(f, id, b + 8 - n, n);
}


static int mb_ebml_id(membuf_t *b, uint32_t id) {
    if (id > 0x00ffffffu) return mb_u32be(b, id);
    if (id > 0x0000ffffu) { uint8_t x[3] = {(uint8_t)(id >> 16),(uint8_t)(id >> 8),(uint8_t)id}; return mb_put(b, x, 3); }
    if (id > 0x000000ffu) { uint8_t x[2] = {(uint8_t)(id >> 8),(uint8_t)id}; return mb_put(b, x, 2); }
    return mb_u8(b, (uint8_t)id);
}
static int mb_ebml_size(membuf_t *b, uint64_t n) {
    if (n < 0x7full) return mb_u8(b, (uint8_t)(0x80u | n));
    if (n < 0x3fffull) { uint16_t v = (uint16_t)(0x4000u | n); uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v}; return mb_put(b, x, 2); }
    if (n < 0x1fffffull) { uint8_t x[3] = {(uint8_t)(0x20u | (n >> 16)), (uint8_t)(n >> 8), (uint8_t)n}; return mb_put(b, x, 3); }
    if (n < 0x0fffffffull) return mb_u32be(b, (uint32_t)(0x10000000u | n));
    uint64_t v = 0x0100000000000000ull | n;
    return mb_u64be(b, v);
}
static int mb_ebml_elem(membuf_t *b, uint32_t id, const void *p, size_t n) { return mb_ebml_id(b, id) && mb_ebml_size(b, n) && mb_put(b, p, n); }
static int mb_ebml_uint(membuf_t *b, uint32_t id, uint64_t v) {
    uint8_t x[8]; size_t n = 1;
    for (int i = 0; i < 8; ++i) x[i] = (uint8_t)(v >> ((7 - i) * 8));
    while (n < 8 && x[8 - n - 1] != 0) n++;
    return mb_ebml_elem(b, id, x + 8 - n, n);
}
static int mb_ebml_float64(membuf_t *b, uint32_t id, double d) {
    union { double d; uint64_t u; } u; u.d = d;
    uint8_t x[8] = {(uint8_t)(u.u >> 56),(uint8_t)(u.u >> 48),(uint8_t)(u.u >> 40),(uint8_t)(u.u >> 32),(uint8_t)(u.u >> 24),(uint8_t)(u.u >> 16),(uint8_t)(u.u >> 8),(uint8_t)u.u};
    return mb_ebml_elem(b, id, x, sizeof(x));
}
static int mb_ebml_str(membuf_t *b, uint32_t id, const char *s) { return mb_ebml_elem(b, id, s, strlen(s)); }
static int mb_ebml_master(membuf_t *b, uint32_t id, const membuf_t *child) { return mb_ebml_id(b, id) && mb_ebml_size(b, child->n) && mb_put(b, child->p, child->n); }

static int write_header(gp32_media_recorder_t *r) {
    FILE *f = r->f;
    membuf_t ebml = {0}, info = {0}, tracks = {0}, te = {0}, video = {0}, audio = {0}, priv = {0};
    int ok = 0;
    if (!mb_ebml_uint(&ebml, 0x4286, 1) || !mb_ebml_uint(&ebml, 0x42F7, 1) || !mb_ebml_uint(&ebml, 0x42F2, 4) || !mb_ebml_uint(&ebml, 0x42F3, 8) || !mb_ebml_str(&ebml, 0x4282, "matroska") || !mb_ebml_uint(&ebml, 0x4287, 4) || !mb_ebml_uint(&ebml, 0x4285, 2)) goto done;
    ebml_id(f, 0x1A45DFA3); ebml_size(f, ebml.n); fwrite(ebml.p, 1, ebml.n, f);
    ebml_id(f, 0x18538067); ebml_unknown_size8(f);
    if (!mb_ebml_uint(&info, 0x2AD7B1, 1000000) || !mb_ebml_str(&info, 0x4D80, "GP32emu") || !mb_ebml_str(&info, 0x5741, "GP32emu ZMBV recorder")) goto done;
    ebml_id(f, 0x1549A966); ebml_size(f, info.n); fwrite(info.p, 1, info.n, f);

    /* BITMAPINFOHEADER codec private for V_MS/VFW/FOURCC ZMBV. */
    if (!mb_u32le(&priv, 40u) || !mb_u32le(&priv, GP32_MEDIA_LCD_W) || !mb_u32le(&priv, GP32_MEDIA_LCD_H) || !mb_u16le(&priv, 1u) || !mb_u16le(&priv, 32u) || !mb_put(&priv, "ZMBV", 4) || !mb_u32le(&priv, GP32_MEDIA_LCD_W * GP32_MEDIA_LCD_H * 4u) || !mb_u32le(&priv, 2835u) || !mb_u32le(&priv, 2835u) || !mb_u32le(&priv, 0u) || !mb_u32le(&priv, 0u)) goto done;
    if (!mb_ebml_uint(&video, 0xB0, GP32_MEDIA_LCD_W) || !mb_ebml_uint(&video, 0xBA, GP32_MEDIA_LCD_H)) goto done;
    if (!mb_ebml_uint(&te, 0xD7, 1) || !mb_ebml_uint(&te, 0x73C5, 1) || !mb_ebml_uint(&te, 0x83, 1) || !mb_ebml_str(&te, 0x86, "V_MS/VFW/FOURCC") || !mb_ebml_elem(&te, 0x63A2, priv.p, priv.n) || !mb_ebml_master(&te, 0xE0, &video)) goto done;
    if (!mb_ebml_master(&tracks, 0xAE, &te)) goto done;
    free(te.p); te.p = NULL; te.n = te.cap = 0;
    if (!mb_ebml_float64(&audio, 0xB5, (double)r->audio_rate) || !mb_ebml_uint(&audio, 0x9F, 2) || !mb_ebml_uint(&audio, 0x6264, 16)) goto done;
    if (!mb_ebml_uint(&te, 0xD7, 2) || !mb_ebml_uint(&te, 0x73C5, 2) || !mb_ebml_uint(&te, 0x83, 2) || !mb_ebml_str(&te, 0x86, "A_PCM/INT/LIT") || !mb_ebml_master(&te, 0xE1, &audio)) goto done;
    if (!mb_ebml_master(&tracks, 0xAE, &te)) goto done;
    ebml_id(f, 0x1654AE6B); ebml_size(f, tracks.n); fwrite(tracks.p, 1, tracks.n, f);
    ok = ferror(f) == 0;

done:
    free(ebml.p); free(info.p); free(tracks.p); free(te.p); free(video.p); free(audio.p); free(priv.p);
    return ok;
}

static int ensure_cluster(gp32_media_recorder_t *r, uint64_t timestamp_ms) {
    if (!r->cluster_open || timestamp_ms < r->current_cluster_ms || timestamp_ms - r->current_cluster_ms > 30000ull) {
        ebml_id(r->f, 0x1F43B675); ebml_unknown_size8(r->f);
        r->current_cluster_ms = timestamp_ms;
        ebml_uint(r->f, 0xE7, timestamp_ms);
        r->cluster_open = 1;
    }
    return ferror(r->f) == 0;
}

static int write_simple_block(gp32_media_recorder_t *r, uint8_t track, uint64_t timestamp_ms, uint8_t flags, const void *data, size_t bytes) {
    if (!ensure_cluster(r, timestamp_ms)) return 0;
    int64_t rel = (int64_t)timestamp_ms - (int64_t)r->current_cluster_ms;
    if (rel < -32768 || rel > 32767) { rec_seterr(r, "Matroska relative timestamp overflow"); return 0; }
    ebml_id(r->f, 0xA3);
    ebml_size(r->f, bytes + 4u);
    fputc((int)(0x80u | track), r->f);
    put_u16be(r->f, (uint16_t)(int16_t)rel);
    fputc((int)flags, r->f);
    fwrite(data, 1, bytes, r->f);
    return ferror(r->f) == 0;
}

#if GP32EMU_ENABLE_THREADS
static int gp32_media_recorder_start_worker(gp32_media_recorder_t *r);
#endif

gp32_media_recorder_t *gp32_media_recorder_open(const char *path, uint32_t audio_rate_hz, char *err, size_t err_len) {
    if (!path || !path[0]) { seterr(err, err_len, "invalid recorder path"); return NULL; }
    gp32_media_recorder_t *r = (gp32_media_recorder_t *)calloc(1, sizeof(*r));
    if (!r) { seterr(err, err_len, "out of memory opening recorder"); return NULL; }
    r->audio_rate = audio_rate_hz ? audio_rate_hz : 44100u;
    gp32_audio_resampler_init(&r->audio_resampler);
    r->f = fopen(path, "wb");
    if (!r->f) { char tmp[256]; snprintf(tmp, sizeof(tmp), "recording open failed: %s", strerror(errno)); seterr(err, err_len, tmp); free(r); return NULL; }
    r->frame = (uint32_t *)malloc((size_t)GP32_MEDIA_LCD_W * GP32_MEDIA_LCD_H * sizeof(uint32_t));
    if (!r->frame) { fclose(r->f); free(r); seterr(err, err_len, "out of memory opening recorder"); return NULL; }
    if (!write_header(r)) {
        fclose(r->f); free(r->frame); free(r); seterr(err, err_len, "failed to write Matroska header"); return NULL;
    }
#if GP32EMU_ENABLE_THREADS
    if (!gp32_media_recorder_start_worker(r)) {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s", r->err[0] ? r->err : "failed to start recorder worker");
        fclose(r->f); free(r->frame); free(r); seterr(err, err_len, tmp); return NULL;
    }
#endif
    return r;
}

static int ensure_zbuf(gp32_media_recorder_t *r, size_t raw) {
    size_t comp = raw + (raw >> 3) + (raw >> 6) + 1024u;
    if (r->zwork_cap < raw) { uint8_t *p = (uint8_t *)realloc(r->zwork, raw); if (!p) return 0; r->zwork = p; r->zwork_cap = raw; }
    if (r->zcomp_cap < comp) { uint8_t *p = (uint8_t *)realloc(r->zcomp, comp); if (!p) return 0; r->zcomp = p; r->zcomp_cap = comp; }
    return 1;
}

static int gp32_media_recorder_add_frame_sync(gp32_media_recorder_t *r, const gp32_framebuffer_desc_t *fb, uint64_t frame_index) {
    if (!r || !fb) return 0;
    const size_t pixels = (size_t)GP32_MEDIA_LCD_W * GP32_MEDIA_LCD_H;
    const size_t raw = pixels * 4u;
    if (!gp32_media_frame_to_bgra_320x240(fb, r->frame)) { rec_seterr(r, "invalid framebuffer for recording"); return 0; }
    if (!ensure_zbuf(r, raw)) { rec_seterr(r, "out of memory compressing ZMBV frame"); return 0; }
    for (size_t i = 0; i < pixels; ++i) {
        uint32_t p = r->frame[i];
        r->zwork[i * 4u + 0u] = (uint8_t)(p & 255u);
        r->zwork[i * 4u + 1u] = (uint8_t)((p >> 8) & 255u);
        r->zwork[i * 4u + 2u] = (uint8_t)((p >> 16) & 255u);
        r->zwork[i * 4u + 3u] = 0u;
    }
    mz_ulong comp_len = (mz_ulong)r->zcomp_cap;
    if (mz_compress2(r->zcomp, &comp_len, r->zwork, (mz_ulong)raw, 6) != MZ_OK) { rec_seterr(r, "ZMBV deflate failed"); return 0; }
    size_t packet_len = 7u + (size_t)comp_len;
    uint8_t *packet = (uint8_t *)malloc(packet_len);
    if (!packet) { rec_seterr(r, "out of memory writing ZMBV frame"); return 0; }
    packet[0] = ZMBV_KEYFRAME;
    packet[1] = 0; packet[2] = 1; packet[3] = 1; packet[4] = ZMBV_FMT_32BPP; packet[5] = ZMBV_BLOCK; packet[6] = ZMBV_BLOCK;
    memcpy(packet + 7, r->zcomp, (size_t)comp_len);
    uint64_t ts = (frame_index * 1000ull) / 60ull;
    int ok = write_simple_block(r, 1u, ts, 0x80u, packet, packet_len);
    free(packet);
    if (!ok) rec_seterr(r, "failed to write video block");
    return ok;
}

static int ensure_audio_tmp(gp32_media_recorder_t *r, size_t frames) {
    size_t samples = frames * 2u;
    if (samples <= r->audio_tmp_cap) return 1;
    int16_t *p = (int16_t *)realloc(r->audio_tmp, samples * sizeof(int16_t));
    if (!p) return 0;
    r->audio_tmp = p; r->audio_tmp_cap = samples;
    return 1;
}

static int gp32_media_recorder_add_audio_sync(gp32_media_recorder_t *r, const gp32_audio_desc_t *audio) {
    if (!r || !audio || !audio->samples_s16_interleaved || audio->frame_count == 0) return 1;
    uint32_t src_rate = audio->sample_rate_hz ? audio->sample_rate_hz : r->audio_rate;
    if (!src_rate || !r->audio_rate) return 1;
    size_t max_out = gp32_audio_resampler_max_output_frames(&r->audio_resampler,
                                                            (size_t)audio->frame_count,
                                                            src_rate,
                                                            r->audio_rate,
                                                            0);
    if (max_out == 0) return 1;
    if (max_out > (SIZE_MAX / (2u * sizeof(int16_t)))) { rec_seterr(r, "audio block too large"); return 0; }
    if (!ensure_audio_tmp(r, max_out)) { rec_seterr(r, "out of memory resampling recorder audio"); return 0; }
    size_t out_frames = gp32_audio_resampler_process(&r->audio_resampler,
                                                     audio->samples_s16_interleaved,
                                                     (size_t)audio->frame_count,
                                                     src_rate,
                                                     r->audio_rate,
                                                     0,
                                                     r->audio_tmp,
                                                     max_out);
    if (out_frames == 0) return 1;
    uint64_t ts = (r->audio_frames_out * 1000ull) / r->audio_rate;
    r->audio_frames_out += out_frames;
    if (!write_simple_block(r, 2u, ts, 0x80u, r->audio_tmp, out_frames * 2u * sizeof(int16_t))) { rec_seterr(r, "failed to write audio block"); return 0; }
    return 1;
}


#if GP32EMU_ENABLE_THREADS
#define GP32_RECORDER_MAX_ASYNC_BYTES (32u * 1024u * 1024u)
#define GP32_RECORDER_MAX_AUDIO_BYTES (48u * 1024u * 1024u)

static void rec_job_free(gp32_rec_job_t *j) {
    if (!j) return;
    free(j->data.ptr);
    free(j);
}

static void rec_lock(gp32_media_recorder_t *r) {
#if defined(_WIN32)
    EnterCriticalSection(&r->lock);
#else
    pthread_mutex_lock(&r->lock);
#endif
}

static void rec_unlock(gp32_media_recorder_t *r) {
#if defined(_WIN32)
    LeaveCriticalSection(&r->lock);
#else
    pthread_mutex_unlock(&r->lock);
#endif
}

static void rec_signal(gp32_media_recorder_t *r) {
#if defined(_WIN32)
    WakeConditionVariable(&r->cond);
#else
    pthread_cond_signal(&r->cond);
#endif
}

static void rec_wait(gp32_media_recorder_t *r) {
#if defined(_WIN32)
    SleepConditionVariableCS(&r->cond, &r->lock, INFINITE);
#else
    pthread_cond_wait(&r->cond, &r->lock);
#endif
}

static int rec_push_job(gp32_media_recorder_t *r, gp32_rec_job_t *j, int may_drop_video) {
    int ok = 1;
    rec_lock(r);
    if (r->async_failed) ok = 0;
    else if (may_drop_video && r->async_bytes > GP32_RECORDER_MAX_ASYNC_BYTES) {
        r->dropped_video_frames++;
        ok = 2;
    } else if (!may_drop_video && r->async_bytes > GP32_RECORDER_MAX_AUDIO_BYTES) {
        snprintf(r->err, sizeof(r->err), "recording worker fell behind");
        r->async_failed = 1;
        ok = 0;
    } else {
        j->next = NULL;
        if (r->job_tail) r->job_tail->next = j;
        else r->job_head = j;
        r->job_tail = j;
        r->async_bytes += j->bytes;
        rec_signal(r);
    }
    rec_unlock(r);
    if (ok != 1) rec_job_free(j);
    return ok != 0;
}

static gp32_rec_job_t *rec_pop_job(gp32_media_recorder_t *r) {
    gp32_rec_job_t *j = NULL;
    rec_lock(r);
    while (!r->job_head && !r->stop_worker) rec_wait(r);
    if (r->job_head) {
        j = r->job_head;
        r->job_head = j->next;
        if (!r->job_head) r->job_tail = NULL;
        r->async_bytes -= j->bytes;
        j->next = NULL;
    }
    rec_unlock(r);
    return j;
}

static void rec_worker_body(gp32_media_recorder_t *r) {
    for (;;) {
        gp32_rec_job_t *j = rec_pop_job(r);
        if (!j) {
            rec_lock(r);
            int done = r->stop_worker && !r->job_head;
            rec_unlock(r);
            if (done) break;
            continue;
        }
        int ok = 1;
        if (j->type == GP32_REC_JOB_FRAME) {
            gp32_framebuffer_desc_t fb;
            memset(&fb, 0, sizeof(fb));
            fb.pixels_rgba8888 = j->data.frame_bgra;
            fb.width = GP32_MEDIA_LCD_W;
            fb.height = GP32_MEDIA_LCD_H;
            fb.stride_pixels = GP32_MEDIA_LCD_W;
            ok = gp32_media_recorder_add_frame_sync(r, &fb, j->frame_index);
        } else if (j->type == GP32_REC_JOB_AUDIO) {
            gp32_audio_desc_t aud;
            memset(&aud, 0, sizeof(aud));
            aud.samples_s16_interleaved = j->data.audio_s16;
            aud.frame_count = j->frame_count;
            aud.sample_rate_hz = j->sample_rate;
            ok = gp32_media_recorder_add_audio_sync(r, &aud);
        }
        rec_job_free(j);
        if (!ok) {
            rec_lock(r);
            r->async_failed = 1;
            rec_unlock(r);
        }
    }
}

#if defined(_WIN32)
static DWORD WINAPI rec_worker_proc(LPVOID opaque) { rec_worker_body((gp32_media_recorder_t *)opaque); return 0; }
#else
static void *rec_worker_proc(void *opaque) { rec_worker_body((gp32_media_recorder_t *)opaque); return NULL; }
#endif

static int gp32_media_recorder_start_worker(gp32_media_recorder_t *r) {
    if (!r) return 0;
#if defined(_WIN32)
    InitializeCriticalSection(&r->lock);
    InitializeConditionVariable(&r->cond);
    r->worker = CreateThread(NULL, 0, rec_worker_proc, r, 0, NULL);
    if (!r->worker) { DeleteCriticalSection(&r->lock); rec_seterr(r, "CreateThread failed for recorder"); return 0; }
#else
    if (pthread_mutex_init(&r->lock, NULL) != 0) { rec_seterr(r, "pthread_mutex_init failed for recorder"); return 0; }
    if (pthread_cond_init(&r->cond, NULL) != 0) { pthread_mutex_destroy(&r->lock); rec_seterr(r, "pthread_cond_init failed for recorder"); return 0; }
    if (pthread_create(&r->worker, NULL, rec_worker_proc, r) != 0) { pthread_cond_destroy(&r->cond); pthread_mutex_destroy(&r->lock); rec_seterr(r, "pthread_create failed for recorder"); return 0; }
#endif
    r->threaded = 1;
    r->worker_started = 1;
    return 1;
}

static void gp32_media_recorder_stop_worker(gp32_media_recorder_t *r, int flush) {
    if (!r || !r->worker_started) return;
    rec_lock(r);
    if (!flush) {
        gp32_rec_job_t *j = r->job_head;
        r->job_head = r->job_tail = NULL;
        r->async_bytes = 0;
        while (j) { gp32_rec_job_t *next = j->next; rec_job_free(j); j = next; }
    }
    r->stop_worker = 1;
    rec_signal(r);
    rec_unlock(r);
#if defined(_WIN32)
    WaitForSingleObject(r->worker, INFINITE);
    CloseHandle(r->worker);
    DeleteCriticalSection(&r->lock);
#else
    pthread_join(r->worker, NULL);
    pthread_cond_destroy(&r->cond);
    pthread_mutex_destroy(&r->lock);
#endif
    r->worker_started = 0;
}
#endif /* GP32EMU_ENABLE_THREADS */

int gp32_media_recorder_add_frame(gp32_media_recorder_t *r, const gp32_framebuffer_desc_t *fb, uint64_t frame_index) {
    if (!r || !fb) return 0;
#if GP32EMU_ENABLE_THREADS
    if (r->threaded) {
        gp32_rec_job_t *j = (gp32_rec_job_t *)calloc(1, sizeof(*j));
        if (!j) { rec_seterr(r, "out of memory queueing video frame"); return 0; }
        j->bytes = (size_t)GP32_MEDIA_LCD_W * GP32_MEDIA_LCD_H * sizeof(uint32_t);
        j->data.frame_bgra = (uint32_t *)malloc(j->bytes);
        if (!j->data.frame_bgra) { free(j); rec_seterr(r, "out of memory queueing video frame"); return 0; }
        if (!gp32_media_frame_to_bgra_320x240(fb, j->data.frame_bgra)) { rec_job_free(j); rec_seterr(r, "invalid framebuffer for recording"); return 0; }
        j->type = GP32_REC_JOB_FRAME;
        j->frame_index = frame_index;
        return rec_push_job(r, j, 1);
    }
#endif
    return gp32_media_recorder_add_frame_sync(r, fb, frame_index);
}

int gp32_media_recorder_add_audio(gp32_media_recorder_t *r, const gp32_audio_desc_t *audio) {
    if (!r || !audio || !audio->samples_s16_interleaved || audio->frame_count == 0) return 1;
#if GP32EMU_ENABLE_THREADS
    if (r->threaded) {
        if (audio->frame_count > SIZE_MAX / (2u * sizeof(int16_t))) { rec_seterr(r, "audio block too large"); return 0; }
        gp32_rec_job_t *j = (gp32_rec_job_t *)calloc(1, sizeof(*j));
        if (!j) { rec_seterr(r, "out of memory queueing audio"); return 0; }
        j->bytes = (size_t)audio->frame_count * 2u * sizeof(int16_t);
        j->data.audio_s16 = (int16_t *)malloc(j->bytes);
        if (!j->data.audio_s16) { free(j); rec_seterr(r, "out of memory queueing audio"); return 0; }
        memcpy(j->data.audio_s16, audio->samples_s16_interleaved, j->bytes);
        j->type = GP32_REC_JOB_AUDIO;
        j->sample_rate = audio->sample_rate_hz;
        j->frame_count = audio->frame_count;
        return rec_push_job(r, j, 0);
    }
#endif
    return gp32_media_recorder_add_audio_sync(r, audio);
}

int gp32_media_recorder_close(gp32_media_recorder_t *r) {
    if (!r) return 1;
#if GP32EMU_ENABLE_THREADS
    gp32_media_recorder_stop_worker(r, 1);
#endif
    int ok = 1;
    if (r->f) { ok = ferror(r->f) == 0; if (fclose(r->f) != 0) ok = 0; }
#if GP32EMU_ENABLE_THREADS
    if (r->async_failed) ok = 0;
#endif
    free(r->zwork); free(r->zcomp); free(r->frame); free(r->audio_tmp); free(r);
    return ok;
}

void gp32_media_recorder_abort(gp32_media_recorder_t *r) {
    if (!r) return;
#if GP32EMU_ENABLE_THREADS
    gp32_media_recorder_stop_worker(r, 0);
#endif
    if (r->f) fclose(r->f);
    free(r->zwork); free(r->zcomp); free(r->frame); free(r->audio_tmp); free(r);
}
