#include "gp32emu/gp32.h"
#include "gp32emu/platform.h"
#include "input_script.h"
#include "platform/sdl12/common.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct app_options {
    const char *bios;
    const char *smc;
    const char *fxe;
    const char *fpk;
    const char *input_script;
    const char *record_input;
    const char *state_path;
    uint32_t cycles_per_frame;
    int cycles_per_frame_set;
    uint32_t scale;
    uint32_t audio_rate;
    uint32_t hle_sef_rate;
    int fullscreen;
    int rotate;
    int lcd_persistence;
    int frame_interpolation;
    int enable_joystick;
    int enable_joystick_axis;
    int no_audio;
    int trace;
    int jit_enabled;
    int jit_stats;
    uint64_t max_frames;
} app_options_t;

static void log_line(void *user, const char *msg) {
    (void)user;
    fputs(msg, stderr);
    fputc('\n', stderr);
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--bios gp32166m.bin] [--smc game.smc] [--fxe homebrew.fxe|--fpk package.fpk] [--input-script script.txt] [--record-input out.script] [--state file.gp32st] [--max-frames N] [--scale N] [--fullscreen] [--lcd-persistence] [--frame-interpolation] [--cycles-per-frame N] [--audio-rate HZ] [--hle-sef-rate HZ] [--no-audio] [--joystick|--no-joystick] [--joystick-axis|--no-joystick-axis] [--trace] [--jit|--no-jit] [--jit-stats] [--rotate-ccw|--rotate-cw|--rotate-180|--no-rotate]\n"
        "\n"
        "SDL 1.2 interactive GP32 frontend. Headless builds do not depend on SDL.\n"
        "Keyboard: arrows=dpad, z=A, x=B, a=L, s=R, Enter=Start, Shift=Select, Esc=quit. Hotkeys: F5=save state, F8=load state. Input scripts use FRAMEf:BUTTON names such as 1550f:P.\n",
        argv0);
}

static int parse_args(int argc, char **argv, app_options_t *o) {
    memset(o, 0, sizeof(*o));
    o->cycles_per_frame = 1100000u;
    o->scale = 2u;
    o->rotate = 1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--bios") && i + 1 < argc) o->bios = argv[++i];
        else if (!strcmp(argv[i], "--smc") && i + 1 < argc) o->smc = argv[++i];
        else if (!strcmp(argv[i], "--fxe") && i + 1 < argc) o->fxe = argv[++i];
        else if (!strcmp(argv[i], "--fpk") && i + 1 < argc) o->fpk = argv[++i];
        else if (!strcmp(argv[i], "--input-script") && i + 1 < argc) o->input_script = argv[++i];
        else if (!strcmp(argv[i], "--record-input") && i + 1 < argc) o->record_input = argv[++i];
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) o->state_path = argv[++i];
        else if (!strcmp(argv[i], "--max-frames") && i + 1 < argc) o->max_frames = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) o->scale = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--cycles-per-frame") && i + 1 < argc) { o->cycles_per_frame = (uint32_t)strtoul(argv[++i], NULL, 0); o->cycles_per_frame_set = 1; }
        else if (!strcmp(argv[i], "--audio-rate") && i + 1 < argc) o->audio_rate = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--hle-sef-rate") && i + 1 < argc) o->hle_sef_rate = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fullscreen")) o->fullscreen = 1;
        else if (!strcmp(argv[i], "--lcd-persistence")) o->lcd_persistence = 1;
        else if (!strcmp(argv[i], "--frame-interpolation")) o->frame_interpolation = 1;
        else if (!strcmp(argv[i], "--joystick")) o->enable_joystick = 1;
        else if (!strcmp(argv[i], "--no-joystick")) o->enable_joystick = 0;
        else if (!strcmp(argv[i], "--joystick-axis")) { o->enable_joystick = 1; o->enable_joystick_axis = 1; }
        else if (!strcmp(argv[i], "--no-joystick-axis")) o->enable_joystick_axis = 0;
        else if (!strcmp(argv[i], "--no-audio")) o->no_audio = 1;
        else if (!strcmp(argv[i], "--trace")) o->trace = 1;
        else if (!strcmp(argv[i], "--jit")) o->jit_enabled = 1;
        else if (!strcmp(argv[i], "--no-jit")) o->jit_enabled = 0;
        else if (!strcmp(argv[i], "--jit-stats")) o->jit_stats = 1;
        else if (!strcmp(argv[i], "--rotate-ccw")) o->rotate = 1;
        else if (!strcmp(argv[i], "--rotate-180")) o->rotate = 2;
        else if (!strcmp(argv[i], "--rotate-cw")) o->rotate = 3;
        else if (!strcmp(argv[i], "--no-rotate")) o->rotate = 0;
        else return 0;
    }
    if (!o->bios && !o->smc && !o->fxe && !o->fpk) return 0;
    if (!o->state_path) o->state_path = "gp32_state.gp32st";
    if (o->cycles_per_frame == 0) o->cycles_per_frame = 1100000u;
    if (o->scale == 0) o->scale = 2u;
    return 1;
}

int main(int argc, char **argv) {
    app_options_t args;
    if (!parse_args(argc, argv, &args)) {
        usage(argv[0]);
        return 2;
    }

    gp32_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.bios_path = args.bios;
    opt.smartmedia_path = args.smc;
    opt.enable_trace = args.trace;
    opt.log = args.trace ? log_line : NULL;

    gp32_t *g = gp32_create(&opt);
    if (!g) {
        fprintf(stderr, "gp32_create failed\n");
        return 1;
    }
    gp32_set_jit(g, args.jit_enabled && !args.trace);
    if (args.hle_sef_rate && gp32_set_hle_sef_rate(g, args.hle_sef_rate) != GP32_OK) {
        fprintf(stderr, "bad --hle-sef-rate: %s\n", gp32_get_error(g));
        gp32_destroy(g);
        return 2;
    }

    if (args.fxe) {
        gp32_status_t st = gp32_load_fxe(g, args.fxe);
        if (st != GP32_OK) {
            fprintf(stderr, "FXE load failed: %s\n", gp32_get_error(g));
            gp32_destroy(g);
            return 1;
        }
    }
    if (args.fpk) {
        gp32_status_t st = gp32_load_fpk(g, args.fpk);
        if (st != GP32_OK) {
            fprintf(stderr, "FPK load failed: %s\n", gp32_get_error(g));
            gp32_destroy(g);
            return 1;
        }
    }

    gp32_input_script_t *input_script = NULL;
    if (args.input_script) {
        char script_err[256] = {0};
        if (!gp32_input_script_load(args.input_script, &input_script, script_err, sizeof(script_err))) {
            fprintf(stderr, "input script load failed: %s\n", script_err[0] ? script_err : "unknown error");
            gp32_destroy(g);
            return 1;
        }
    }

    gp32_input_recorder_t *recorder = NULL;
    if (args.record_input) {
        char rec_err[256] = {0};
        recorder = gp32_input_recorder_open(args.record_input, rec_err, sizeof(rec_err));
        if (!recorder) {
            fprintf(stderr, "input recorder open failed: %s\n", rec_err[0] ? rec_err : "unknown error");
            gp32_input_script_destroy(input_script);
            gp32_destroy(g);
            return 1;
        }
    }

    gp32_video_options_t vopt;
    memset(&vopt, 0, sizeof(vopt));
    vopt.title = "gp32emu";
    vopt.scale = args.scale;
    vopt.fullscreen = args.fullscreen;
    vopt.rotate = args.rotate;
    vopt.lcd_persistence = args.lcd_persistence;
    vopt.frame_interpolation = args.frame_interpolation;
    gp32_video_backend_t *video = gp32_video_sdl12_create(&vopt);
    if (!video) {
        fprintf(stderr, "SDL video init failed: %s\n", SDL_GetError());
        if (args.jit_stats) {
        fprintf(stderr, "cpu block hits=%" PRIu64 " misses=%" PRIu64 " fallbacks=%" PRIu64 "\n", gp32_get_jit_hits(g), gp32_get_jit_misses(g), gp32_get_jit_fallbacks(g));
    }
    gp32_input_recorder_close(recorder);
        gp32_input_script_destroy(input_script);
        gp32_destroy(g);
        return 1;
    }

    gp32_input_options_t iopt;
    memset(&iopt, 0, sizeof(iopt));
    iopt.enable_joystick = args.enable_joystick;
    iopt.enable_joystick_axis = args.enable_joystick_axis;
    gp32_input_backend_t *input = gp32_input_sdl12_create(&iopt);
    if (!input) {
        fprintf(stderr, "SDL input init failed: %s\n", SDL_GetError());
        gp32_video_destroy(video);
        if (args.jit_stats) {
        fprintf(stderr, "cpu block hits=%" PRIu64 " misses=%" PRIu64 " fallbacks=%" PRIu64 "\n", gp32_get_jit_hits(g), gp32_get_jit_misses(g), gp32_get_jit_fallbacks(g));
    }
    gp32_input_recorder_close(recorder);
        gp32_input_script_destroy(input_script);
        gp32_destroy(g);
        return 1;
    }

    gp32_audio_backend_t *audio = NULL;
    if (!args.no_audio) {
        gp32_audio_options_t aopt;
        memset(&aopt, 0, sizeof(aopt));
        aopt.sample_rate_hz = args.audio_rate;
        aopt.buffer_frames = 2048u;
        audio = gp32_audio_sdl12_create(&aopt);
        if (!audio) fprintf(stderr, "SDL audio unavailable: %s\n", SDL_GetError());
    }

    int quit = 0;
    uint32_t buttons = 0;
    uint64_t frame_index = 0;
    uint64_t last_tick_us = (uint64_t)SDL_GetTicks() * 1000u;
    uint64_t emu_accum_units = 1000000ull; /* run the first emulated frame immediately; units are microseconds * 60 */
    uint32_t fps_tick = (uint32_t)(last_tick_us / 1000u);
    uint64_t dynamic_cycle_accum = 0;
    uint32_t fps_frames = 0;
    uint32_t emu_fps_frames = 0;
    const char *game_label = args.fpk ? args.fpk : (args.fxe ? args.fxe : (args.smc ? args.smc : "BIOS"));
    while (!quit) {
        uint32_t physical_buttons = 0;
        if (gp32_input_poll(input, &physical_buttons, &quit) != GP32_OK) {
            fprintf(stderr, "input error: %s\n", gp32_input_error(input));
            break;
        }
        uint32_t actions = gp32_input_take_actions(input);
        if (actions & GP32_FRONTEND_ACTION_SAVE_STATE) {
            gp32_status_t sst = gp32_save_state(g, args.state_path);
            fprintf(stderr, "%s state: %s\n", sst == GP32_OK ? "saved" : "save", sst == GP32_OK ? args.state_path : gp32_get_error(g));
        }
        if (actions & GP32_FRONTEND_ACTION_LOAD_STATE) {
            gp32_status_t lst = gp32_load_state(g, args.state_path);
            fprintf(stderr, "%s state: %s\n", lst == GP32_OK ? "loaded" : "load", lst == GP32_OK ? args.state_path : gp32_get_error(g));
            if (lst == GP32_OK) {
                dynamic_cycle_accum = 0;
                emu_accum_units = 1000000ull;
            }
        }

        uint64_t now_us = (uint64_t)SDL_GetTicks() * 1000u;
        uint64_t elapsed_us = now_us >= last_tick_us ? now_us - last_tick_us : 0u;
        last_tick_us = now_us;
        if (elapsed_us > 250000u) elapsed_us = 250000u;
        emu_accum_units += elapsed_us * 60ull;

        unsigned steps = 0;
        int ran_emulation = 0;
        while (!quit && emu_accum_units >= 1000000ull && steps < 5u) {
            buttons = input_script ? gp32_input_script_frame(input_script, frame_index) : physical_buttons;
            gp32_set_buttons(g, buttons);
            gp32_input_recorder_sample(recorder, frame_index, buttons);
            uint32_t frame_cycles = args.cycles_per_frame;
            if (!args.cycles_per_frame_set) {
                uint32_t run_hz = gp32_get_run_clock_hz(g);
                if (!run_hz) run_hz = 66000000u;
                dynamic_cycle_accum += (uint64_t)run_hz;
                frame_cycles = (uint32_t)(dynamic_cycle_accum / 60u);
                dynamic_cycle_accum -= (uint64_t)frame_cycles * 60u;
                if (!frame_cycles) frame_cycles = args.cycles_per_frame;
            }
            gp32_status_t st = gp32_run_cycles(g, frame_cycles);
            if (st != GP32_OK) {
                fprintf(stderr, "run failed: %s\n", gp32_get_error(g));
                quit = 1;
                break;
            }
            if (audio) {
                gp32_audio_desc_t aud;
                if (gp32_get_audio(g, &aud) == GP32_OK && aud.frame_count > 0) {
                    gp32_audio_submit(audio, &aud);
                    gp32_clear_audio(g);
                }
            }
            frame_index++;
            emu_fps_frames++;
            ran_emulation = 1;
            emu_accum_units -= 1000000ull;
            steps++;
            if (args.max_frames && frame_index >= args.max_frames) quit = 1;
        }
        if (emu_accum_units >= 1000000ull && steps >= 5u) emu_accum_units = 1000000ull;

        if (ran_emulation && !quit) {
            gp32_framebuffer_desc_t fb;
            if (gp32_get_framebuffer(g, &fb) == GP32_OK) {
                if (gp32_video_present(video, &fb) != GP32_OK) {
                    fprintf(stderr, "video error: %s\n", gp32_video_error(video));
                    break;
                }
                ++fps_frames;
            }
        }
        {
            uint32_t now = (uint32_t)SDL_GetTicks();
            uint32_t elapsed = now - fps_tick;
            if (elapsed >= 1000u) {
                double render_fps = elapsed ? ((double)fps_frames * 1000.0 / (double)elapsed) : 0.0;
                double emu_fps = elapsed ? ((double)emu_fps_frames * 1000.0 / (double)elapsed) : 0.0;
                char title[256];
                snprintf(title, sizeof(title), "gp32emu SDL 1.2 - %.1f render / %.1f emu FPS - %s", render_fps, emu_fps, game_label);
                (void)gp32_video_set_title(video, title);
                fps_tick = now;
                fps_frames = 0;
                emu_fps_frames = 0;
            }
        }
        if (!ran_emulation) {
            uint64_t wait_units = 1000000ull - emu_accum_units;
            uint64_t delay_us = wait_units / 60ull;
            if (delay_us >= 1000u) SDL_Delay((uint32_t)(delay_us / 1000u));
            else SDL_Delay(1u);
        } else {
            uint64_t wait_units = emu_accum_units < 1000000ull ? 1000000ull - emu_accum_units : 0u;
            uint64_t delay_us = wait_units / 60ull;
            if (delay_us >= 2000u) SDL_Delay((uint32_t)(delay_us / 1000u - 1u));
        }
    }

    if (args.jit_stats) {
        fprintf(stderr, "cpu block hits=%" PRIu64 " misses=%" PRIu64 " fallbacks=%" PRIu64 "\n", gp32_get_jit_hits(g), gp32_get_jit_misses(g), gp32_get_jit_fallbacks(g));
    }
    gp32_input_recorder_close(recorder);
    gp32_input_script_destroy(input_script);
    gp32_audio_destroy(audio);
    gp32_input_destroy(input);
    gp32_video_destroy(video);
    gp32_destroy(g);
    return 0;
}
