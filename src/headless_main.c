#include "gp32emu/gp32.h"
#include "input_script.h"
#include "media/gp32_media.h"
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void log_line(void *user, const char *msg) {
    (void)user;
    fputs(msg, stderr);
    fputc('\n', stderr);
}

static uint32_t fb_pixel_at(const gp32_framebuffer_desc_t *fb, uint32_t x, uint32_t y) {
    return fb->pixels_rgba8888[y * fb->stride_pixels + x];
}

static int write_ppm(const char *path, const gp32_framebuffer_desc_t *fb, int rotate) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t out_w = (rotate == 1 || rotate == 3) ? fb->height : fb->width;
    uint32_t out_h = (rotate == 1 || rotate == 3) ? fb->width : fb->height;
    fprintf(f, "P6\n%u %u\n255\n", out_w, out_h);
    for (uint32_t y = 0; y < out_h; ++y) {
        for (uint32_t x = 0; x < out_w; ++x) {
            uint32_t sx = x, sy = y;
            if (rotate == 1) {          /* 90 degrees counter-clockwise, matching GP32 landscape display orientation. */
                sx = fb->width - 1u - y;
                sy = x;
            } else if (rotate == 2) {   /* 180 degrees */
                sx = fb->width - 1u - x;
                sy = fb->height - 1u - y;
            } else if (rotate == 3) {   /* 90 degrees clockwise */
                sx = y;
                sy = fb->height - 1u - x;
            }
            uint32_t p = fb_pixel_at(fb, sx, sy);
            unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return 1;
}

static int dump_frame_now(gp32_t *g, const char *path, int rotate) {
    gp32_framebuffer_desc_t fb;
    if (!path) return 0;
    if (gp32_get_framebuffer(g, &fb) == GP32_OK && write_ppm(path, &fb, rotate)) {
        printf("wrote %s (%ux%u%s)\n", path, (rotate == 1 || rotate == 3) ? fb.height : fb.width, (rotate == 1 || rotate == 3) ? fb.width : fb.height, rotate ? " rotated" : "");
        return 1;
    }
    fprintf(stderr, "frame dump failed: %s\n", path);
    return 0;
}

static uint32_t frame_cycles_for(gp32_t *g, int cycles_per_frame_set, uint32_t cycles_per_frame, uint64_t *cycle_accum) {
    if (cycles_per_frame_set) return cycles_per_frame ? cycles_per_frame : 1u;
    uint32_t run_hz = gp32_get_run_clock_hz(g);
    if (!run_hz) run_hz = 66000000u;
    *cycle_accum += (uint64_t)run_hz;
    uint32_t frame_cycles = (uint32_t)(*cycle_accum / 60u);
    *cycle_accum -= (uint64_t)frame_cycles * 60u;
    return frame_cycles ? frame_cycles : 1u;
}

static void put_le16(FILE *f, uint16_t v) {
    fputc((int)(v & 0xffu), f);
    fputc((int)((v >> 8) & 0xffu), f);
}

static void put_le32(FILE *f, uint32_t v) {
    put_le16(f, (uint16_t)v);
    put_le16(f, (uint16_t)(v >> 16));
}

static int write_wav(const char *path, const gp32_audio_desc_t *audio) {
    if (!path || !audio || !audio->samples_s16_interleaved || audio->frame_count == 0) return 0;
    uint64_t data_bytes64 = audio->frame_count * 2u * sizeof(int16_t);
    if (data_bytes64 > UINT32_MAX - 36u) return 0;
    uint32_t data_bytes = (uint32_t)data_bytes64;
    uint32_t rate = audio->sample_rate_hz ? audio->sample_rate_hz : 44100u;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite("RIFF", 1, 4, f); put_le32(f, 36u + data_bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put_le32(f, 16u); put_le16(f, 1u); put_le16(f, 2u);
    put_le32(f, rate); put_le32(f, rate * 2u * 2u); put_le16(f, 4u); put_le16(f, 16u);
    fwrite("data", 1, 4, f); put_le32(f, data_bytes);
    for (uint64_t i = 0; i < audio->frame_count * 2u; ++i) put_le16(f, (uint16_t)audio->samples_s16_interleaved[i]);
    fclose(f);
    return 1;
}

typedef struct button_event { uint32_t at_cycles; uint32_t mask; } button_event_t;
typedef struct dump_at_event { uint64_t at_cycles; const char *path; int done; } dump_at_event_t;

static uint32_t bios_auto_start_buttons_for_frame(uint64_t frame) {
    return ((frame >= 620u && frame < 660u) ||
            (frame >= 1450u && frame < 1490u) ||
            (frame >= 1800u && frame < 1840u) ||
            (frame >= 2200u && frame < 2240u)) ? GP32_BUTTON_A : 0u;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--bios gp32166m.bin] [--smc game.smc] [--fxe homebrew.fxe|--fpk package.fpk] [--input-script script.txt] [--load-state file.gp32st] [--no-bios-auto-start] [--cycles-per-frame N] [--frames N] [--hle-sef-rate HZ] [--buttons MASK] [--button-at CYCLES:MASK] [--cycles N] [--step-cycles N] [--dump-frame out.ppm] [--dump-at CYCLES:out.ppm] [--record-mkv out.mkv] [--dump-wav out.wav] [--save-smc out.smc] [--dump-mem ADDR LEN out.bin] [--trace] [--jit|--no-jit] [--jit-stats] [--dump-regs] [--dump-lcd-regs] [--dump-cp15] [--progress] [--rotate-ccw|--rotate-cw|--rotate-180]\n"
        "\n"
        "Headless standalone GP32 emulator smoke runner. No BIOS or game data is bundled. --fxe accepts classic scrambled FXE files and raw GXB payloads; --fpk extracts and loads the package's main FXE. Input scripts use FRAMEf:BUTTON names such as 1550f:P. BIOS+SMC headless runs synthesize a few A/confirm pulses unless --no-bios-auto-start or explicit input is supplied.\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *bios = NULL, *smc = NULL, *fxe = NULL, *fpk = NULL, *input_script_path = NULL, *load_state = NULL, *dump = NULL, *record_mkv = NULL, *dump_wav = NULL, *save_smc = NULL, *dump_mem = NULL;
    uint32_t dump_mem_addr = 0, dump_mem_len = 0;
    uint32_t buttons = 0, hle_sef_rate = 0;
    button_event_t events[64];
    size_t event_count = 0;
    dump_at_event_t dump_events[128];
    size_t dump_event_count = 0;
    uint32_t cycles = 100000, step_cycles = 1000000, cycles_per_frame = 1100000;
    uint64_t frames = 0;
    int cycles_per_frame_set = 0;
    int trace = 0, jit_enabled = 0, jit_stats = 0, dump_regs = 0, dump_lcd_regs = 0, dump_cp15 = 0, progress = 0, rotate = 0, bios_auto_start = 1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--bios") && i + 1 < argc) bios = argv[++i];
        else if (!strcmp(argv[i], "--smc") && i + 1 < argc) smc = argv[++i];
        else if (!strcmp(argv[i], "--fxe") && i + 1 < argc) fxe = argv[++i];
        else if (!strcmp(argv[i], "--fpk") && i + 1 < argc) fpk = argv[++i];
        else if (!strcmp(argv[i], "--input-script") && i + 1 < argc) input_script_path = argv[++i];
        else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) load_state = argv[++i];
        else if (!strcmp(argv[i], "--no-bios-auto-start")) bios_auto_start = 0;
        else if (!strcmp(argv[i], "--buttons") && i + 1 < argc) buttons = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--button-at") && i + 1 < argc) {
            if (event_count >= sizeof(events) / sizeof(events[0])) { fprintf(stderr, "too many --button-at events\n"); return 2; }
            char *spec = argv[++i];
            char *colon = strchr(spec, ':');
            if (!colon) { fprintf(stderr, "bad --button-at, expected CYCLES:MASK\n"); return 2; }
            *colon = '\0';
            events[event_count].at_cycles = (uint32_t)strtoul(spec, NULL, 0);
            events[event_count].mask = (uint32_t)strtoul(colon + 1, NULL, 0);
            event_count++;
        }
        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--step-cycles") && i + 1 < argc) step_cycles = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--cycles-per-frame") && i + 1 < argc) { cycles_per_frame = (uint32_t)strtoul(argv[++i], NULL, 0); cycles_per_frame_set = 1; }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--hle-sef-rate") && i + 1 < argc) hle_sef_rate = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--dump-frame") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--dump-at") && i + 1 < argc) {
            if (dump_event_count >= sizeof(dump_events) / sizeof(dump_events[0])) { fprintf(stderr, "too many --dump-at events\n"); return 2; }
            char *spec = argv[++i];
            char *colon = strchr(spec, ':');
            if (!colon) { fprintf(stderr, "bad --dump-at, expected CYCLES:PATH\n"); return 2; }
            *colon = '\0';
            dump_events[dump_event_count].at_cycles = strtoull(spec, NULL, 0);
            dump_events[dump_event_count].path = colon + 1;
            dump_events[dump_event_count].done = 0;
            dump_event_count++;
        }
        else if (!strcmp(argv[i], "--record-mkv") && i + 1 < argc) record_mkv = argv[++i];
        else if (!strcmp(argv[i], "--dump-wav") && i + 1 < argc) dump_wav = argv[++i];
        else if (!strcmp(argv[i], "--save-smc") && i + 1 < argc) save_smc = argv[++i];
        else if (!strcmp(argv[i], "--dump-mem") && i + 3 < argc) { dump_mem_addr = (uint32_t)strtoul(argv[++i], NULL, 0); dump_mem_len = (uint32_t)strtoul(argv[++i], NULL, 0); dump_mem = argv[++i]; }
        else if (!strcmp(argv[i], "--trace")) trace = 1;
        else if (!strcmp(argv[i], "--jit")) jit_enabled = 1;
        else if (!strcmp(argv[i], "--no-jit")) jit_enabled = 0;
        else if (!strcmp(argv[i], "--jit-stats")) jit_stats = 1;
        else if (!strcmp(argv[i], "--rotate-ccw")) rotate = 1;
        else if (!strcmp(argv[i], "--rotate-180")) rotate = 2;
        else if (!strcmp(argv[i], "--rotate-cw")) rotate = 3;
        else if (!strcmp(argv[i], "--dump-regs")) dump_regs = 1;
        else if (!strcmp(argv[i], "--dump-lcd-regs")) dump_lcd_regs = 1;
        else if (!strcmp(argv[i], "--dump-cp15")) dump_cp15 = 1;
        else if (!strcmp(argv[i], "--progress")) progress = 1;
        else { usage(argv[0]); return 2; }
    }
    if (!bios && !smc && !fxe && !fpk && !load_state) { usage(argv[0]); return 2; }
    if (load_state && !smc && !fxe && !fpk) {
        fprintf(stderr, "--load-state requires the original --smc, --fxe, or --fpk backing media for deterministic validation\n");
        return 2;
    }
    if (cycles_per_frame == 0) cycles_per_frame = 1100000u;
    int bios_auto_start_active = bios_auto_start && bios && smc && !fxe && !fpk && !input_script_path && event_count == 0u && buttons == 0u;
    gp32_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.bios_path = bios;
    opt.smartmedia_path = smc;
    opt.enable_trace = trace;
    opt.log = trace ? log_line : NULL;
    gp32_t *g = gp32_create(&opt);
    if (!g) { fprintf(stderr, "gp32_create failed\n"); return 1; }
    gp32_set_jit(g, jit_enabled && !trace);
    if (hle_sef_rate && gp32_set_hle_sef_rate(g, hle_sef_rate) != GP32_OK) { fprintf(stderr, "bad --hle-sef-rate: %s\n", gp32_get_error(g)); gp32_destroy(g); return 2; }
    if (fxe) {
        gp32_status_t load_st = gp32_load_fxe(g, fxe);
        if (load_st != GP32_OK) { fprintf(stderr, "FXE load failed: %s\n", gp32_get_error(g)); gp32_destroy(g); return 1; }
    }
    if (fpk) {
        gp32_status_t load_st = gp32_load_fpk(g, fpk);
        if (load_st != GP32_OK) { fprintf(stderr, "FPK load failed: %s\n", gp32_get_error(g)); gp32_destroy(g); return 1; }
    }
    if (load_state) {
        gp32_status_t load_st = gp32_load_state(g, load_state);
        if (load_st != GP32_OK) { fprintf(stderr, "state load failed: %s\n", gp32_get_error(g)); gp32_destroy(g); return 1; }
    }
    gp32_input_script_t *input_script = NULL;
    if (input_script_path) {
        char script_err[256] = {0};
        if (!gp32_input_script_load(input_script_path, &input_script, script_err, sizeof(script_err))) {
            fprintf(stderr, "input script load failed: %s\n", script_err[0] ? script_err : "unknown error");
            gp32_destroy(g);
            return 1;
        }
    }
    uint32_t script_buttons = 0;
    uint32_t auto_buttons = bios_auto_start_active ? bios_auto_start_buttons_for_frame(0u) : 0u;
    gp32_set_buttons(g, buttons | script_buttons | auto_buttons);
    for (size_t a = 1; a < event_count; ++a) {
        button_event_t key = events[a];
        size_t b = a;
        while (b > 0 && events[b - 1].at_cycles > key.at_cycles) { events[b] = events[b - 1]; --b; }
        events[b] = key;
    }
    for (size_t a = 1; a < dump_event_count; ++a) {
        dump_at_event_t key = dump_events[a];
        size_t b = a;
        while (b > 0 && dump_events[b - 1].at_cycles > key.at_cycles) { dump_events[b] = dump_events[b - 1]; --b; }
        dump_events[b] = key;
    }
    size_t next_event = 0;
    size_t next_dump_event = 0;
    gp32_status_t st = GP32_OK;
    uint32_t remaining = cycles;
    if (step_cycles == 0) step_cycles = cycles ? cycles : 1u;
    uint64_t script_frame = 0;
    uint64_t next_script_cycle = 0;
    uint64_t auto_frame = 0;
    uint64_t next_auto_cycle = 0;
    if (record_mkv && frames == 0) frames = (uint64_t)cycles / (uint64_t)(cycles_per_frame ? cycles_per_frame : 1u);
    if (record_mkv && frames == 0) frames = 1;
    gp32_media_recorder_t *recorder = NULL;
    uint64_t dynamic_cycle_accum = 0;
    if (frames) {
        if (record_mkv) {
            char rec_err[256] = {0};
            recorder = gp32_media_recorder_open(record_mkv, 44100u, rec_err, sizeof(rec_err));
            if (!recorder) { fprintf(stderr, "mkv recorder open failed: %s\n", rec_err[0] ? rec_err : "unknown error"); gp32_input_script_destroy(input_script); gp32_destroy(g); return 1; }
        }
        for (uint64_t frame = 0; frame < frames; ++frame) {
            uint32_t elapsed = (uint32_t)gp32_get_cycles(g);
            auto_buttons = bios_auto_start_active ? bios_auto_start_buttons_for_frame(frame) : 0u;
            gp32_set_buttons(g, buttons | script_buttons | auto_buttons);
            if (input_script) {
                script_buttons = gp32_input_script_frame(input_script, script_frame);
                gp32_set_buttons(g, buttons | script_buttons | auto_buttons);
                if (progress) {
                    char names[128];
                    fprintf(stderr, "input frame=%" PRIu64 " cycles=%" PRIu64 " buttons=%s (%08" PRIx32 ")\n", script_frame, gp32_get_cycles(g), gp32_input_button_names(script_buttons, names, sizeof(names)), script_buttons);
                }
                script_frame++;
            }
            while (next_event < event_count && events[next_event].at_cycles <= elapsed) {
                buttons = events[next_event].mask;
                gp32_set_buttons(g, buttons | script_buttons | auto_buttons);
                if (progress) fprintf(stderr, "button cycles=%" PRIu64 " mask=%08" PRIx32 "\n", gp32_get_cycles(g), events[next_event].mask);
                next_event++;
            }
            uint32_t n = frame_cycles_for(g, cycles_per_frame_set, cycles_per_frame, &dynamic_cycle_accum);
            st = gp32_run_cycles(g, n);
            if (st != GP32_OK) break;
            if (recorder) {
                gp32_framebuffer_desc_t fb;
                gp32_audio_desc_t aud;
                if (gp32_get_audio(g, &aud) == GP32_OK && aud.frame_count > 0) {
                    if (!gp32_media_recorder_add_audio(recorder, &aud)) { fprintf(stderr, "mkv audio write failed: %s\n", gp32_media_recorder_error(recorder)); st = GP32_ERR_IO; break; }
                    gp32_clear_audio(g);
                }
                if (gp32_get_framebuffer(g, &fb) == GP32_OK) {
                    (void)rotate;
                    if (!gp32_media_recorder_add_frame(recorder, &fb, frame)) { fprintf(stderr, "mkv video write failed: %s\n", gp32_media_recorder_error(recorder)); st = GP32_ERR_IO; break; }
                }
            }
            if (progress) fprintf(stderr, "frame=%" PRIu64 " cycles=%" PRIu64 " frame_cycles=%" PRIu32 " pc=%08" PRIx32 " cpsr=%08" PRIx32 " run_hz=%" PRIu32 " fclk=%" PRIu32 "\n", frame, gp32_get_cycles(g), n, gp32_get_pc(g), gp32_get_cpsr(g), gp32_get_run_clock_hz(g), gp32_get_fclk_hz(g));
        }
        if (recorder) {
            int rec_ok = gp32_media_recorder_close(recorder);
            recorder = NULL;
            if (st == GP32_OK && rec_ok) printf("wrote %s (%" PRIu64 " ZMBV/PCM MKV frames)\n", record_mkv, frames);
            else if (st == GP32_OK) { fprintf(stderr, "mkv close failed\n"); st = GP32_ERR_IO; }
        }
    } else while (remaining) {
        uint64_t elapsed64 = gp32_get_cycles(g);
        uint32_t elapsed = (uint32_t)elapsed64;
        while (next_dump_event < dump_event_count && dump_events[next_dump_event].at_cycles <= elapsed64) {
            if (!dump_events[next_dump_event].done) {
                dump_frame_now(g, dump_events[next_dump_event].path, rotate);
                dump_events[next_dump_event].done = 1;
            }
            next_dump_event++;
        }
        while (bios_auto_start_active && (uint64_t)elapsed >= next_auto_cycle) {
            auto_buttons = bios_auto_start_buttons_for_frame(auto_frame);
            gp32_set_buttons(g, buttons | script_buttons | auto_buttons);
            auto_frame++;
            next_auto_cycle += cycles_per_frame;
        }
        while (input_script && (uint64_t)elapsed >= next_script_cycle) {
            script_buttons = gp32_input_script_frame(input_script, script_frame);
            gp32_set_buttons(g, buttons | script_buttons | auto_buttons);
            if (progress) {
                char names[128];
                fprintf(stderr, "input frame=%" PRIu64 " cycles=%" PRIu64 " buttons=%s (%08" PRIx32 ")\n", script_frame, gp32_get_cycles(g), gp32_input_button_names(script_buttons, names, sizeof(names)), script_buttons);
            }
            script_frame++;
            next_script_cycle += cycles_per_frame;
        }
        while (next_event < event_count && events[next_event].at_cycles <= elapsed) {
            buttons = events[next_event].mask;
            gp32_set_buttons(g, buttons | script_buttons | auto_buttons);
            if (progress) fprintf(stderr, "button cycles=%" PRIu64 " mask=%08" PRIx32 "\n", gp32_get_cycles(g), events[next_event].mask);
            next_event++;
        }
        uint32_t n = remaining < step_cycles ? remaining : step_cycles;
        if (next_event < event_count && events[next_event].at_cycles > elapsed) {
            uint32_t until_event = events[next_event].at_cycles - elapsed;
            if (until_event < n) n = until_event;
        }
        if (input_script && next_script_cycle > (uint64_t)elapsed) {
            uint64_t until_script = next_script_cycle - (uint64_t)elapsed;
            if (until_script < (uint64_t)n) n = (uint32_t)until_script;
        }
        if (next_dump_event < dump_event_count && dump_events[next_dump_event].at_cycles > elapsed64) {
            uint64_t until_dump = dump_events[next_dump_event].at_cycles - elapsed64;
            if (until_dump < (uint64_t)n) n = (uint32_t)until_dump;
        }
        if (n == 0) n = 1;
        st = gp32_run_cycles(g, n);
        if (st != GP32_OK) break;
        remaining -= n;
        if (progress) fprintf(stderr, "progress cycles=%" PRIu64 " pc=%08" PRIx32 " cpsr=%08" PRIx32 "\n", gp32_get_cycles(g), gp32_get_pc(g), gp32_get_cpsr(g));
    }
    uint64_t final_elapsed64 = gp32_get_cycles(g);
    while (next_dump_event < dump_event_count && dump_events[next_dump_event].at_cycles <= final_elapsed64) {
        if (!dump_events[next_dump_event].done) {
            dump_frame_now(g, dump_events[next_dump_event].path, rotate);
            dump_events[next_dump_event].done = 1;
        }
        next_dump_event++;
    }
    if (st != GP32_OK) { fprintf(stderr, "run failed: %s\n", gp32_get_error(g)); gp32_destroy(g); return 1; }
    printf("cycles=%" PRIu64 " pc=%08" PRIx32 " cpsr=%08" PRIx32 "\n", gp32_get_cycles(g), gp32_get_pc(g), gp32_get_cpsr(g));
    if (jit_stats) printf("cpu block hits=%" PRIu64 " misses=%" PRIu64 " fallbacks=%" PRIu64 "\n", gp32_get_jit_hits(g), gp32_get_jit_misses(g), gp32_get_jit_fallbacks(g));
    if (dump_regs) {
        for (unsigned i = 0; i < 16; ++i) {
            printf("r%-2u=%08" PRIx32 "%c", i, gp32_get_cpu_reg(g, i), (i % 4u) == 3u ? '\n' : ' ');
        }
    }
    if (dump_lcd_regs) {
        for (uint32_t off = 0; off <= 0x1cu; off += 4u) printf("lcd[%02" PRIx32 "]=%08" PRIx32 "\n", off, gp32_debug_read32(g, 0x14a00000u + off));
    }
    if (dump_cp15) {
        for (unsigned i = 0; i < 16; ++i) printf("cp15[%u]=%08" PRIx32 "\n", i, gp32_get_cp15(g, i));
    }
    if (dump_mem) {
        FILE *mf = fopen(dump_mem, "wb");
        if (mf) {
            for (uint32_t off = 0; off < dump_mem_len; off += 4u) {
                uint32_t v = gp32_debug_read32(g, dump_mem_addr + off);
                unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
                uint32_t todo = dump_mem_len - off < 4u ? dump_mem_len - off : 4u;
                fwrite(b, 1, todo, mf);
            }
            fclose(mf);
            printf("wrote %s (0x%08" PRIx32 "+0x%08" PRIx32 ")\n", dump_mem, dump_mem_addr, dump_mem_len);
        } else fprintf(stderr, "memory dump failed: %s\n", strerror(errno));
    }
    if (dump_wav) {
        gp32_audio_desc_t audio;
        if (gp32_get_audio(g, &audio) == GP32_OK && write_wav(dump_wav, &audio)) {
            printf("wrote %s (%" PRIu64 " stereo frames @ %u Hz)\n", dump_wav, audio.frame_count, audio.sample_rate_hz);
        } else {
            fprintf(stderr, "wav dump failed or no audio samples captured\n");
        }
    }
    if (save_smc) {
        gp32_status_t save_st = gp32_save_smartmedia(g, save_smc);
        if (save_st == GP32_OK) printf("wrote %s\n", save_smc);
        else fprintf(stderr, "SmartMedia save failed: %s\n", gp32_get_error(g));
    }
    if (dump) dump_frame_now(g, dump, rotate);
    gp32_input_script_destroy(input_script);
    gp32_destroy(g);
    return 0;
}
