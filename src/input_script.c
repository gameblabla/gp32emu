#include "input_script.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum script_action {
    SCRIPT_TAP,
    SCRIPT_PRESS,
    SCRIPT_RELEASE,
    SCRIPT_SET
} script_action_t;

typedef struct script_event {
    uint64_t frame;
    uint32_t mask;
    script_action_t action;
} script_event_t;

struct gp32_input_script {
    script_event_t *events;
    size_t count;
    size_t cap;
    size_t next;
    uint32_t held;
};

struct gp32_input_recorder {
    FILE *file;
    uint32_t last;
};

static void seterr(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) --e;
    *e = '\0';
    return s;
}

static int eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

static uint32_t name_mask(const char *tok) {
    if (!tok || !*tok) return 0;
    if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) return (uint32_t)strtoul(tok, NULL, 0);
    if (isdigit((unsigned char)tok[0])) return (uint32_t)strtoul(tok, NULL, 0);
    if (eq_ci(tok, "A")) return GP32_BUTTON_A;
    if (eq_ci(tok, "B")) return GP32_BUTTON_B;
    if (eq_ci(tok, "L")) return GP32_BUTTON_L;
    if (eq_ci(tok, "R")) return GP32_BUTTON_R;
    if (eq_ci(tok, "START") || eq_ci(tok, "P") || eq_ci(tok, "PLAY") || eq_ci(tok, "ENTER")) return GP32_BUTTON_START;
    if (eq_ci(tok, "SELECT") || eq_ci(tok, "SEL") || eq_ci(tok, "S")) return GP32_BUTTON_SELECT;
    if (eq_ci(tok, "UP") || eq_ci(tok, "U")) return GP32_BUTTON_UP;
    if (eq_ci(tok, "DOWN") || eq_ci(tok, "D")) return GP32_BUTTON_DOWN;
    if (eq_ci(tok, "LEFT")) return GP32_BUTTON_LEFT;
    if (eq_ci(tok, "RIGHT")) return GP32_BUTTON_RIGHT;
    if (eq_ci(tok, "NONE") || eq_ci(tok, "0")) return 0;
    return UINT32_MAX;
}

static const char *single_name(uint32_t bit) {
    switch (bit) {
    case GP32_BUTTON_A: return "A";
    case GP32_BUTTON_B: return "B";
    case GP32_BUTTON_L: return "L";
    case GP32_BUTTON_R: return "R";
    case GP32_BUTTON_START: return "P";
    case GP32_BUTTON_SELECT: return "SELECT";
    case GP32_BUTTON_UP: return "UP";
    case GP32_BUTTON_DOWN: return "DOWN";
    case GP32_BUTTON_LEFT: return "LEFT";
    case GP32_BUTTON_RIGHT: return "RIGHT";
    default: return NULL;
    }
}

const char *gp32_input_button_names(uint32_t mask, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) return "";
    buf[0] = '\0';
    if (mask == 0) {
        snprintf(buf, buf_len, "NONE");
        return buf;
    }
    int first = 1;
    const uint32_t bits[] = { GP32_BUTTON_A, GP32_BUTTON_B, GP32_BUTTON_L, GP32_BUTTON_R, GP32_BUTTON_START, GP32_BUTTON_SELECT, GP32_BUTTON_UP, GP32_BUTTON_DOWN, GP32_BUTTON_LEFT, GP32_BUTTON_RIGHT };
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
        if (mask & bits[i]) {
            const char *n = single_name(bits[i]);
            size_t used = strlen(buf);
            snprintf(buf + used, used < buf_len ? buf_len - used : 0, "%s%s", first ? "" : "+", n ? n : "?");
            first = 0;
        }
    }
    uint32_t known = GP32_BUTTON_A | GP32_BUTTON_B | GP32_BUTTON_L | GP32_BUTTON_R | GP32_BUTTON_START | GP32_BUTTON_SELECT | GP32_BUTTON_UP | GP32_BUTTON_DOWN | GP32_BUTTON_LEFT | GP32_BUTTON_RIGHT;
    if (mask & ~known) {
        size_t used = strlen(buf);
        snprintf(buf + used, used < buf_len ? buf_len - used : 0, "%s0x%08" PRIx32, first ? "" : "+", mask & ~known);
    }
    return buf;
}

static int parse_buttons(char *text, uint32_t *out_mask) {
    uint32_t mask = 0;
    char *p = text;
    while (*p) {
        while (*p == '+' || *p == ',' || *p == '|' || isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '+' && *p != ',' && *p != '|' && !isspace((unsigned char)*p)) ++p;
        char save = *p;
        *p = '\0';
        uint32_t b = name_mask(start);
        *p = save;
        if (b == UINT32_MAX) return 0;
        mask |= b;
    }
    *out_mask = mask;
    return 1;
}

static int add_event(gp32_input_script_t *s, uint64_t frame, uint32_t mask, script_action_t action) {
    if (s->count == s->cap) {
        size_t nc = s->cap ? s->cap * 2u : 64u;
        if (nc < s->cap) return 0;
        script_event_t *n = (script_event_t *)realloc(s->events, nc * sizeof(*n));
        if (!n) return 0;
        s->events = n;
        s->cap = nc;
    }
    s->events[s->count].frame = frame;
    s->events[s->count].mask = mask;
    s->events[s->count].action = action;
    s->count++;
    return 1;
}

static int event_cmp(const void *a, const void *b) {
    const script_event_t *ea = (const script_event_t *)a;
    const script_event_t *eb = (const script_event_t *)b;
    if (ea->frame < eb->frame) return -1;
    if (ea->frame > eb->frame) return 1;
    return (int)ea->action - (int)eb->action;
}

int gp32_input_script_load(const char *path, gp32_input_script_t **out_script, char *err, size_t err_len) {
    if (!path || !out_script) { seterr(err, err_len, "invalid input script arguments"); return 0; }
    FILE *f = fopen(path, "rb");
    if (!f) { seterr(err, err_len, "open %s: %s", path, strerror(errno)); return 0; }
    gp32_input_script_t *s = (gp32_input_script_t *)calloc(1, sizeof(*s));
    if (!s) { fclose(f); seterr(err, err_len, "out of memory loading input script"); return 0; }
    char line[512];
    unsigned line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        ++line_no;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *semi = strchr(line, ';');
        if (semi) *semi = '\0';
        char *t = trim(line);
        if (!*t) continue;
        char *colon = strchr(t, ':');
        if (!colon) { seterr(err, err_len, "%s:%u: expected FRAMEf:BUTTONS", path, line_no); gp32_input_script_destroy(s); fclose(f); return 0; }
        *colon = '\0';
        char *frame_txt = trim(t);
        char *end = frame_txt;
        uint64_t frame = strtoull(frame_txt, &end, 0);
        if (end == frame_txt) { seterr(err, err_len, "%s:%u: bad frame", path, line_no); gp32_input_script_destroy(s); fclose(f); return 0; }
        if (*end == 'f' || *end == 'F') ++end;
        if (*trim(end)) { seterr(err, err_len, "%s:%u: bad frame suffix", path, line_no); gp32_input_script_destroy(s); fclose(f); return 0; }
        char *cmd = trim(colon + 1);
        script_action_t action = SCRIPT_TAP;
        if (*cmd == '+') { action = SCRIPT_PRESS; ++cmd; }
        else if (*cmd == '-') { action = SCRIPT_RELEASE; ++cmd; }
        else if (*cmd == '=') { action = SCRIPT_SET; ++cmd; }
        cmd = trim(cmd);
        uint32_t mask = 0;
        if (!parse_buttons(cmd, &mask)) { seterr(err, err_len, "%s:%u: unknown button in '%s'", path, line_no, cmd); gp32_input_script_destroy(s); fclose(f); return 0; }
        if (!add_event(s, frame, mask, action)) { seterr(err, err_len, "out of memory storing input event"); gp32_input_script_destroy(s); fclose(f); return 0; }
    }
    fclose(f);
    qsort(s->events, s->count, sizeof(s->events[0]), event_cmp);
    *out_script = s;
    return 1;
}

void gp32_input_script_destroy(gp32_input_script_t *s) {
    if (!s) return;
    free(s->events);
    free(s);
}

uint32_t gp32_input_script_frame(gp32_input_script_t *s, uint64_t frame_index) {
    if (!s) return 0;
    uint32_t tap = 0;
    while (s->next < s->count && s->events[s->next].frame <= frame_index) {
        const script_event_t *e = &s->events[s->next++];
        if (e->frame < frame_index && e->action == SCRIPT_TAP) continue;
        switch (e->action) {
        case SCRIPT_TAP: tap |= e->mask; break;
        case SCRIPT_PRESS: s->held |= e->mask; break;
        case SCRIPT_RELEASE: s->held &= ~e->mask; break;
        case SCRIPT_SET: s->held = e->mask; break;
        }
    }
    return s->held | tap;
}

gp32_input_recorder_t *gp32_input_recorder_open(const char *path, char *err, size_t err_len) {
    if (!path) { seterr(err, err_len, "invalid recorder path"); return NULL; }
    FILE *f = fopen(path, "wb");
    if (!f) { seterr(err, err_len, "open %s: %s", path, strerror(errno)); return NULL; }
    gp32_input_recorder_t *r = (gp32_input_recorder_t *)calloc(1, sizeof(*r));
    if (!r) { fclose(f); seterr(err, err_len, "out of memory opening input recorder"); return NULL; }
    r->file = f;
    fprintf(f, "# gp32emu input script\n");
    fprintf(f, "# framef:+BUTTON presses; framef:-BUTTON releases; framef:BUTTON taps for one frame.\n");
    return r;
}

void gp32_input_recorder_sample(gp32_input_recorder_t *r, uint64_t frame_index, uint32_t mask) {
    if (!r || !r->file) return;
    uint32_t changed = r->last ^ mask;
    if (!changed) return;
    const uint32_t bits[] = { GP32_BUTTON_A, GP32_BUTTON_B, GP32_BUTTON_L, GP32_BUTTON_R, GP32_BUTTON_START, GP32_BUTTON_SELECT, GP32_BUTTON_UP, GP32_BUTTON_DOWN, GP32_BUTTON_LEFT, GP32_BUTTON_RIGHT };
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
        if (changed & bits[i]) {
            const char *n = single_name(bits[i]);
            fprintf(r->file, "%" PRIu64 "f:%c%s\n", frame_index, (mask & bits[i]) ? '+' : '-', n ? n : "?");
        }
    }
    uint32_t known = GP32_BUTTON_A | GP32_BUTTON_B | GP32_BUTTON_L | GP32_BUTTON_R | GP32_BUTTON_START | GP32_BUTTON_SELECT | GP32_BUTTON_UP | GP32_BUTTON_DOWN | GP32_BUTTON_LEFT | GP32_BUTTON_RIGHT;
    if (changed & ~known) fprintf(r->file, "%" PRIu64 "f:=0x%08" PRIx32 "\n", frame_index, mask);
    r->last = mask;
}

void gp32_input_recorder_close(gp32_input_recorder_t *r) {
    if (!r) return;
    if (r->file) fclose(r->file);
    free(r);
}
