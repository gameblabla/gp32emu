#include "gp32emu/gp32.h"
#include "common.h"
#include "arm920t.h"
#include "s3c2400.h"
#include "fxe.h"
#include "fpk.h"
#include "smc_direct.h"

#define GP32_RAM_BASE 0x0c000000u
#define ARM_MODE_SVC 0x13u
#define ARM_I_FLAG 0x00000080u
#define ARM_F_FLAG 0x00000040u
#define GP32_DIRECT_GPOS_TIMER_COUNT 4u
#define GP32_DIRECT_PCM_CHANNELS 4u

struct gp32 {
    s3c2400_t *soc;
    arm920t_t *cpu;
    gp32_log_fn log;
    void *log_user;
    char error[256];
    int direct_fxe_mode;
    uint32_t direct_fxe_entry;
    uint32_t direct_fxe_stack;
    uint32_t direct_fxe_fb_addr;
    uint32_t direct_fxe_image_end;
    uint32_t direct_fxe_lcd_surface[4];
    uint32_t direct_fxe_surface_checksum[4];
    uint32_t direct_fxe_bpp;
    uint32_t direct_fxe_palette_addr;
    uint32_t direct_fxe_palette_initialized;
    uint32_t direct_fxe_lcd_explicit;
    uint32_t direct_fxe_lcd_enabled;
    char direct_fxe_title[33];
    char direct_smc_executable_path[260];
    char direct_smc_game_dir[260];
    fxe_image_t direct_reset_image;
    uint32_t direct_reset_image_valid;
    uint32_t direct_reset_scan_file_hle;
    uint32_t direct_reset_init_smc_gpio;
    fpk_asset_t *direct_fpk_assets;
    size_t direct_fpk_asset_count;
    struct {
        const fpk_asset_t *asset;
        size_t pos;
        int used;
    } direct_fpk_handles[32];
    uint32_t direct_hle_file_open_addr;
    uint32_t direct_hle_file_read_addr[2];
    uint32_t direct_hle_file_close_addr;
    uint32_t direct_hle_file_size_addr;
    uint32_t direct_hle_file_seek_addr;
    uint32_t direct_hle_pathbuf_addr;
    uint32_t direct_hle_sound_dispatch_addr;
    uint32_t direct_hle_sound_table_addr;
    uint32_t direct_hle_sound_play_addr;
    uint32_t direct_hle_sound_state_addr;
    uint32_t direct_hle_pcm_env_addr;
    uint32_t direct_hle_pcm_init_addr;
    uint32_t direct_hle_pcm_play_addr;
    uint32_t direct_hle_pcm_stop_addr;
    uint32_t direct_hle_pcm_remove_addr;
    uint32_t direct_hle_pcm_lock_addr;
    uint32_t direct_hle_pcm_only_kill_addr;
    uint32_t direct_hle_pcm_initialized;
    uint32_t direct_hle_pcm_sr;
    uint32_t direct_hle_pcm_bit_count;
    uint32_t direct_hle_pcm_rate;
    uint32_t direct_hle_pcm_stereo;
    uint32_t direct_hle_pcm_bits;
    uint32_t direct_hle_pcm_active;
    uint32_t direct_hle_pcm_src_addr;
    uint32_t direct_hle_pcm_size_bytes;
    uint32_t direct_hle_pcm_pos_bytes;
    uint32_t direct_hle_pcm_repeat;
    uint64_t direct_hle_pcm_accum;
    struct {
        uint32_t active;
        uint32_t src_addr;
        uint32_t size_bytes;
        uint32_t pos_bytes;
        uint32_t repeat;
        uint32_t rate;
        uint32_t stereo;
        uint32_t bits;
        uint64_t accum;
    } direct_hle_pcm_ch[GP32_DIRECT_PCM_CHANNELS];
    uint32_t direct_hle_sdk_sndmixedbuf_addr;
    uint32_t direct_hle_sdk_sndsrcexist_addr;
    uint32_t direct_hle_sdk_pcm_workidx_addr;
    uint32_t direct_hle_sdk_sndmixer_addr;
    uint32_t direct_hle_sdk_mixbuf0_addr;
    uint32_t direct_hle_sdk_mixbuf1_addr;
    uint32_t direct_hle_sdk_mixbuf_bytes;
    uint32_t direct_hle_sdk_rate;
    uint64_t direct_hle_sdk_accum;
    uint64_t direct_hle_sdk_last_submit_cycle;
    uint64_t direct_hle_sdk_timer_accum;
    uint32_t direct_hle_sdk_submitted_frames;
    uint32_t direct_hle_sdk_timer_table_addr;
    struct {
        uint32_t configured;
        uint32_t enabled;
        uint32_t callback;
        uint32_t tps;
        uint32_t max_exec_tick;
        uint64_t accum;
    } direct_hle_gpos_timer[GP32_DIRECT_GPOS_TIMER_COUNT];
    uint32_t direct_hle_gpos_timers_enabled;
    uint32_t direct_hle_gpos_task_first;
    uint32_t direct_hle_gpos_task_last;
    uint32_t direct_hle_gpos_scheduler_callback;
    uint32_t direct_hle_callback_returned;
    uint32_t direct_hle_callback_running;
    uint32_t direct_hle_audio_rate_override;
    uint32_t direct_hle_audio_last_auto_rate;
    const fpk_asset_t *direct_hle_audio_asset;
    uint32_t direct_hle_audio_pos;
    uint32_t direct_hle_audio_size;
    uint32_t direct_hle_audio_rate;
    uint64_t direct_hle_audio_accum;
    struct {
        const fpk_asset_t *asset;
        uint32_t copied;
        uint32_t tries;
    } direct_hle_asset_autoload[16];
    uint64_t direct_vblank_next_cycle;
    uint64_t direct_vblank_wait_cycles;
    int direct_vblank_wait_requested;
};

static void seterr(gp32_t *g, const char *fmt, ...) {
    if (!g) return;
    va_list ap; va_start(ap, fmt); vsnprintf(g->error, sizeof(g->error), fmt, ap); va_end(ap);
}
static void bridge_log(void *user, const char *line) {
    gp32_t *g = (gp32_t *)user;
    if (g && g->log) g->log(g->log_user, line);
}

static int direct_ram_range(const gp32_t *g, uint32_t addr, uint32_t len) {
    if (!g || !g->soc || addr < GP32_RAM_BASE) return 0;
    uint32_t end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    if (addr > end) return 0;
    return len <= end - addr;
}
static void direct_write32_if_ram(gp32_t *g, uint32_t addr, uint32_t value) {
    if (direct_ram_range(g, addr, 4u)) s3c2400_write32(g->soc, addr, value);
}
static void direct_write8_if_ram(gp32_t *g, uint32_t addr, uint8_t value) {
    if (direct_ram_range(g, addr, 1u)) s3c2400_write8(g->soc, addr, value);
}
static void direct_write_fw_arg(gp32_t *g, uint32_t addr, uint32_t value) {
    if ((addr & 3u) == 0u) direct_write32_if_ram(g, addr, value);
    else direct_write8_if_ram(g, addr, (uint8_t)value);
}
static uint32_t direct_hle_work_base(const gp32_t *g) {
    uint32_t ram_size = (g && g->soc) ? (uint32_t)s3c2400_ram_size(g->soc) : 0u;
    /* Keep direct-FXE firmware work memory out of the application stack.
       GPSDK scheduler stacks commonly live at the top of RAM; aliasing the
       firmware HLE stubs there can corrupt saved task contexts. */
    if (ram_size >= 0x00800000u) return GP32_RAM_BASE + 0x007d0000u;
    if (ram_size >= 0x00400000u) return GP32_RAM_BASE + ram_size - 0x30000u;
    return GP32_RAM_BASE + 0x100u;
}
static uint32_t direct_stub_addr(const gp32_t *g) {
    return direct_hle_work_base(g);
}
static uint32_t direct_ret_stub_addr(const gp32_t *g) { return direct_stub_addr(g) + 16u; }
static uint32_t direct_callback_return_stub_addr(const gp32_t *g) { return direct_stub_addr(g) + 24u; }
static uint32_t direct_app_arg_addr(const gp32_t *g) { return direct_stub_addr(g) + 0x900u; }
static uint32_t direct_smc_cb_base_addr(const gp32_t *g) { return direct_stub_addr(g) + 0xb00u; }

static void direct_install_stubs(gp32_t *g) {
    uint32_t a = direct_stub_addr(g);
    if (!direct_ram_range(g, a, 32u)) return;
    /* GPSDK init stores the firmware display callback returned by SWI #0x0b
       selector 0 into both GpSurfaceSet and GpSurfaceFlip.  The callback is
       passed a GPDRAWSURFACE and makes that surface visible; it must not rewrite
       the application-owned descriptor.  Keep the descriptor intact and bounce
       through a private direct-mode SWI so the C-side HLE can install the LCD
       base address from ptgpds->ptbuffer. */
    s3c2400_write32(g->soc, a + 0u, 0xef000011u); /* svc #0x11 */
    s3c2400_write32(g->soc, a + 4u, 0xe12fff1eu); /* bx lr */
    s3c2400_write32(g->soc, a + 8u, 0xe12fff1eu);
    s3c2400_write32(g->soc, a + 12u, 0xe12fff1eu);
    /* Generic firmware callback slot: return 0. */
    s3c2400_write32(g->soc, a + 16u, 0xe3a00000u); /* mov r0,#0 */
    s3c2400_write32(g->soc, a + 20u, 0xe12fff1eu); /* bx lr */
    s3c2400_write32(g->soc, a + 24u, 0xef070020u); /* private callback-return trap */
    s3c2400_write32(g->soc, a + 28u, 0xeafffffeu); /* b . */
}
static void direct_init_smc_gpio(gp32_t *g) {
    if (!g || !g->soc) return;
    s3c2400_write32(g->soc, 0x15600008u, 0x00000001u);
    s3c2400_write32(g->soc, 0x1560000cu, 0x000000ffu);
    s3c2400_write32(g->soc, 0x15600024u, 0x000001c0u);
    s3c2400_write32(g->soc, 0x15600030u, 0x00000008u);
}

static void direct_install_smc_callbacks(gp32_t *g) {
    static const uint32_t code[] = {
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe591200cu,0xe3a030ffu,0xe0022403u,
        0xe1822000u,0xe581200cu,0xe5912030u,0xe3c22008u,0xe5812030u,0xe3822008u,0xe5812030u,0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5912024u,0xe3a03001u,0xe1c22403u,
        0xe5812024u,0xe591000cu,0xe20000ffu,0xe1822403u,0xe5812024u,0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe3a02002u,0xe1a02402u,0xe5910030u,
        0xe3100004u,0x1a000002u,0xe5910024u,0xe1100002u,0x0afffff9u,0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe3a020ffu,0xe1822402u,0xe5910008u,
        0xe1c00002u,0xe5810008u,0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe3a020ffu,0xe1822402u,0xe5910008u,
        0xe1c00002u,0xe3a02055u,0xe1822402u,0xe1800002u,0xe5810008u,0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910030u,0xe3800010u,0xe5810030u,
        0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910030u,0xe3c00010u,0xe5810030u,
        0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910030u,0xe3800020u,0xe5810030u,
        0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910030u,0xe3c00020u,0xe5810030u,
        0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910024u,0xe3c00080u,0xe5810024u,
        0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910024u,0xe3800080u,0xe5810024u,
        0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910024u,0xe3800040u,0xe5810024u,
        0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910024u,0xe3c00040u,0xe5810024u,
        0xe8bd8000u
    };
    uint32_t base = direct_smc_cb_base_addr(g);
    if (!direct_ram_range(g, base, (uint32_t)sizeof(code))) return;
    for (size_t i = 0; i < sizeof(code) / sizeof(code[0]); ++i) s3c2400_write32(g->soc, base + (uint32_t)i * 4u, code[i]);
}

static uint32_t direct_fwinfo_addr(const gp32_t *g) {
    return direct_hle_work_base(g) + 0x10000u;
}

static uint32_t direct_fw_tick_addr(const gp32_t *g) {
    return direct_fwinfo_addr(g) + 0x20u;
}

static uint32_t direct_pcm_cursor_addr(const gp32_t *g) {
    return direct_fwinfo_addr(g) + 0x80u;
}

static void direct_update_fw_tick(gp32_t *g);
static void direct_hle_audio_tick(gp32_t *g, uint32_t cycles);
static void direct_hle_gpos_timer_tick(gp32_t *g, uint32_t cycles);
static gp32_status_t gp32_load_fxe_image_internal(gp32_t *g, fxe_image_t *img, int update_reset_image, int scan_file_hle, int init_smc_gpio, int preserve_hle_options);
static int direct_handle_swi_gpos_timer(gp32_t *g, arm920t_t *cpu, uint32_t pc);
static void direct_tick_sdk_task_sleepers(gp32_t *g, uint32_t first_task, uint32_t last_task);
static int direct_resume_ready_sdk_task(gp32_t *g, uint32_t pc, uint32_t first_task, uint32_t last_task);
static int direct_task_record_plausible(gp32_t *g, uint32_t task_addr);

static uint32_t direct_firmware_clock_hz(const gp32_t *g) {
    uint32_t hz = 0u;
    if (g && g->soc) hz = s3c2400_fclk_hz(g->soc);
    return hz ? hz : 66000000u;
}

static uint32_t direct_run_clock_hz(const gp32_t *g) {
    uint32_t hz = 0u;
    if (g && g->soc) hz = s3c2400_run_clock_hz(g->soc);
    return hz ? hz : 66000000u;
}

static uint32_t direct_elapsed_ms(const gp32_t *g) {
    if (!g || !g->cpu) return 0u;
    return (uint32_t)((arm920t_get_cycles(g->cpu) * 1000ull) / (uint64_t)direct_run_clock_hz(g));
}

static uint32_t direct_lcd_frame_cycles(const gp32_t *g) {
    uint32_t hz = direct_run_clock_hz(g);
    uint32_t frame = hz / 60u;
    return frame ? frame : 1u;
}

static uint32_t direct_key_port_value(gp32_t *g, uint32_t value) {
    if (!g || !g->soc) return value;
    if (value >= 0x15600000u && value <= 0x1560005bu) return s3c2400_read32(g->soc, value & ~3u);
    return value;
}

static void direct_request_vblank_wait(gp32_t *g) {
    if (g && g->direct_fxe_mode) g->direct_vblank_wait_requested = 1;
}

static void direct_schedule_vblank_wait(gp32_t *g) {
    if (!g || !g->cpu || !g->direct_vblank_wait_requested) return;
    g->direct_vblank_wait_requested = 0;

    /*
     * Direct-loaded GPSDK titles call the firmware's surface callback from
     * GpSurfaceFlip/GpSurfaceSet. Real firmware does not let a title spin
     * through unlimited display flips: normal game loops are paced by the LCD
     * frame/vblank cadence. Without this emulated wait, light games such as OMG
     * can execute many logic iterations per host frame when the JIT is enabled.
     */
    uint32_t frame_cycles = direct_lcd_frame_cycles(g);
    uint64_t now = arm920t_get_cycles(g->cpu);
    if (!g->direct_vblank_next_cycle || g->direct_vblank_next_cycle <= now) {
        g->direct_vblank_next_cycle = now + (uint64_t)frame_cycles;
    }
    if (g->direct_vblank_next_cycle > now) {
        uint64_t wait = g->direct_vblank_next_cycle - now;
        uint64_t max_add = UINT64_MAX - g->direct_vblank_wait_cycles;
        g->direct_vblank_wait_cycles += wait < max_add ? wait : max_add;
    }
    g->direct_vblank_next_cycle += (uint64_t)frame_cycles;
}

static uint32_t direct_consume_idle_wait(gp32_t *g, uint32_t budget) {
    if (!g || !g->cpu || !g->soc || !g->direct_vblank_wait_cycles || !budget) return 0u;
    uint32_t n = budget;
    if (g->direct_vblank_wait_cycles < (uint64_t)n) n = (uint32_t)g->direct_vblank_wait_cycles;
    if (n > 32768u) n = 32768u;
    arm920t_add_idle_cycles(g->cpu, n);
    s3c2400_tick(g->soc, n);
    direct_hle_audio_tick(g, n);
    g->direct_vblank_wait_cycles -= (uint64_t)n;
    direct_update_fw_tick(g);
    return n;
}

static void direct_update_fw_tick(gp32_t *g) {
    if (!g || !g->direct_fxe_mode) return;
    direct_write32_if_ram(g, direct_fw_tick_addr(g), direct_elapsed_ms(g));
}

static void direct_sync_fwinfo(gp32_t *g) {
    uint32_t info = direct_fwinfo_addr(g);
    uint32_t hz = direct_firmware_clock_hz(g);
    direct_write32_if_ram(g, info + 0u, hz);
    direct_write32_if_ram(g, info + 4u, hz);
    direct_write32_if_ram(g, info + 8u, hz);
    direct_update_fw_tick(g);
}

static void direct_copy_bytes_if_ram(gp32_t *g, uint32_t dst, const uint8_t *src, uint32_t len) {
    if (!g || !src || !direct_ram_range(g, dst, len)) return;
    for (uint32_t i = 0; i < len; ++i) s3c2400_write8(g->soc, dst + i, src[i]);
}

static void direct_zero_if_ram(gp32_t *g, uint32_t dst, uint32_t len) {
    if (!g || !direct_ram_range(g, dst, len)) return;
    for (uint32_t i = 0; i < len; ++i) s3c2400_write8(g->soc, dst + i, 0u);
}

static void direct_write_cstr_if_ram(gp32_t *g, uint32_t dst, const char *s) {
    if (!g || !s) return;
    size_t n = strlen(s) + 1u;
    if (n > 512u) n = 512u;
    if (!direct_ram_range(g, dst, (uint32_t)n)) return;
    for (size_t i = 0; i < n; ++i) s3c2400_write8(g->soc, dst + (uint32_t)i, (uint8_t)s[i]);
}

static void direct_apply_gxb_scatterload(gp32_t *g, const fxe_image_t *img) {
    if (!g || !img || !img->payload || img->payload_size < 0x20u) return;
    uint32_t first = gp32_ld32le(img->payload + 0u);
    uint32_t rom_start = gp32_ld32le(img->payload + 4u);
    uint32_t ro_limit = gp32_ld32le(img->payload + 8u);
    uint32_t rw_base = gp32_ld32le(img->payload + 12u);
    uint32_t zi_limit = gp32_ld32le(img->payload + 16u);
    uint32_t rw_limit = gp32_ld32le(img->payload + 20u);
    if ((first & 0x0f000000u) != 0x0a000000u) return;
    if (rom_start != img->load_addr || ro_limit < rom_start) return;
    if (rw_base < GP32_RAM_BASE || rw_limit < rw_base || zi_limit < rw_limit) return;
    uint32_t ro_size = ro_limit - rom_start;
    uint32_t rw_size = rw_limit - rw_base;
    uint32_t zi_size = zi_limit - rw_limit;
    if (rw_size && (uint64_t)ro_size + (uint64_t)rw_size <= img->payload_size) {
        direct_copy_bytes_if_ram(g, rw_base, img->payload + ro_size, rw_size);
    }
    if (zi_size) direct_zero_if_ram(g, rw_limit, zi_size);
}


static void direct_update_stub_framebuffer(gp32_t *g) {
    GP32_UNUSED(g);
}

static uint32_t direct_default_surface_addr(uint32_t page) {
    /* Keep direct-mode LCD buffers high in RAM, away from the loaded image,
       BSS, heap and SDK task stacks.  Four 240x320 8-bpp pages fit below the
       callback stub area at the top of the 8 MiB GP32 RAM map. */
    return GP32_RAM_BASE + 0x00780000u + (page & 3u) * (240u * 320u);
}

static uint32_t direct_surface_addr_for_bpp(const gp32_t *g, uint32_t page) {
    if (g && g->direct_fxe_bpp == 16u) return GP32_RAM_BASE + 0x00780000u + (page & 1u) * (240u * 320u * 2u);
    return direct_default_surface_addr(page);
}

static uint32_t direct_palette_hw_mirror_addr(const gp32_t *g) { return direct_stub_addr(g) + 0x100u; }
static uint32_t direct_palette_sw_addr(const gp32_t *g) { return direct_stub_addr(g) + 0x500u; }

static uint32_t direct_pack_logpal_entry(uint32_t r, uint32_t gr, uint32_t b, uint32_t flags) {
    uint32_t v = ((r & 0xf8u) << 8) | ((gr & 0xf8u) << 3) | ((b & 0xfbu) >> 2);
    if (flags) v |= 1u;
    return v & 0xffffu;
}

static uint32_t direct_sdk_palette_to_lcd(uint32_t value) {
    /* GPSDK GP_PALETTEENTRY matches the S3C2400 TFT 5:5:5:I palette word. */
    return value & 0xffffu;
}

static uint32_t direct_lcd_palette_to_sdk(uint32_t value) {
    return value & 0xffffu;
}

static void direct_write_palette_entry(gp32_t *g, uint32_t index, uint32_t value) {
    if (!g || index >= 256u) return;
    value &= 0xffffu;
    uint32_t lcd = direct_sdk_palette_to_lcd(value);
    s3c2400_write16(g->soc, 0x14a00400u + index * 4u, (uint16_t)lcd);
    uint32_t hw = direct_palette_hw_mirror_addr(g);
    if (direct_ram_range(g, hw + index * 4u, 4u)) s3c2400_write32(g->soc, hw + index * 4u, lcd);
}

static void direct_copy_palette32_to_lcd(gp32_t *g, uint32_t src_addr) {
    if (!g || !direct_ram_range(g, src_addr, 256u * 4u)) return;
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t v = s3c2400_debug_read32(g->soc, src_addr + i * 4u) & 0xffffu;
        direct_write_palette_entry(g, i, v);
    }
    g->direct_fxe_palette_initialized = 1u;
}

static int direct_palette_looks_like_logpal(gp32_t *g, uint32_t pal_addr) {
    if (!g || !direct_ram_range(g, pal_addr, 16u * 4u)) return 0;
    unsigned nonzero_flags = 0, plausible_low = 0;
    for (uint32_t i = 0; i < 16u; ++i) {
        uint32_t v = s3c2400_debug_read32(g->soc, pal_addr + i * 4u);
        uint32_t flags = (v >> 24) & 0xffu;
        uint32_t r = v & 0xffu, gr = (v >> 8) & 0xffu, b = (v >> 16) & 0xffu;
        if (flags) nonzero_flags++;
        if (r <= 0xffu && gr <= 0xffu && b <= 0xffu) plausible_low++;
    }
    /* GP_LOGPALENTRY has byte order R,G,B,flags.  A nonzero flag byte in an
       application palette is a strong signal that the pointer is not an array
       of packed 5:5:5:1 words. */
    return plausible_low == 16u && nonzero_flags >= 8u;
}

static void direct_copy_logpal_to_lcd(gp32_t *g, uint32_t pal_addr) {
    if (!g || !direct_ram_range(g, pal_addr, 256u * 4u)) return;
    uint32_t sw = direct_palette_sw_addr(g);
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t raw = s3c2400_debug_read32(g->soc, pal_addr + i * 4u);
        uint32_t v = direct_pack_logpal_entry(raw & 0xffu, (raw >> 8) & 0xffu, (raw >> 16) & 0xffu, (raw >> 24) & 0xffu);
        direct_write_palette_entry(g, i, v);
        if (direct_ram_range(g, sw + i * 4u, 4u)) s3c2400_write32(g->soc, sw + i * 4u, v);
    }
    g->direct_fxe_palette_addr = sw;
    g->direct_fxe_palette_initialized = 1u;
}

static void direct_copy_lcd_palette_to_ram(gp32_t *g, uint32_t dst_addr) {
    if (!g || !direct_ram_range(g, dst_addr, 256u * 4u)) return;
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t v = s3c2400_debug_read32(g->soc, 0x14a00400u + i * 4u) & 0xffffu;
        s3c2400_write32(g->soc, dst_addr + i * 4u, direct_lcd_palette_to_sdk(v));
    }
}

static int direct_packed_palette_plausible(gp32_t *g, uint32_t pal_addr) {
    if (!g || !direct_ram_range(g, pal_addr, 256u * 4u)) return 0;
    unsigned high_clear = 0, varied = 0, pointer_like = 0;
    uint32_t first = s3c2400_debug_read32(g->soc, pal_addr + 0u) & 0xffffu;
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t v = s3c2400_debug_read32(g->soc, pal_addr + i * 4u);
        if ((v & 0xffff0000u) == 0u) high_clear++;
        if ((v & 0xffffu) != first) varied++;
        if ((v & 0xff000000u) == GP32_RAM_BASE || ((v & 0xff000000u) == 0x0c000000u)) pointer_like++;
    }
    return varied >= 16u && high_clear >= 224u && pointer_like < 8u;
}

static void direct_realize_software_palette_from(gp32_t *g, uint32_t pal_addr, int allow_direct_pointer) {
    if (!g) return;
    uint32_t sw = 0u;

    /* GPSDK exposes two distinct palette paths.  GpPaletteRealize and the
       synchronous/asynchronous realize helpers pass stale scratch registers
       through SWI #0x16; commercial code must therefore realize the firmware
       logical palette returned by the palette-address BIOS service, not any
       incidental RAM value left in r2/r3.  A few direct-HLE loaders did pass a
       literal 256-entry palette pointer, so keep that path only when the
       candidate looks like packed 5:5:5:1 palette data rather than a heap object
       or callback table. */
    if (allow_direct_pointer && direct_packed_palette_plausible(g, pal_addr)) sw = pal_addr;
    else sw = g->direct_fxe_palette_addr;
    if (!direct_ram_range(g, sw, 256u * 4u)) sw = direct_palette_sw_addr(g);
    if (direct_ram_range(g, sw, 256u * 4u)) {
        direct_copy_palette32_to_lcd(g, sw);
        g->direct_fxe_palette_addr = sw;
    }
}


static void direct_fill_default_palette(gp32_t *g) {
    if (!g) return;
    /* GP32 8-bpp examples frequently start with a NULL palette and later update
       individual entries.  Provide a deterministic RGB 3:3:2 palette instead
       of leaving the LCD palette black.  Entries are expanded to the S3C2400's
       5:5:5 packed palette format used by the renderer. */
    uint32_t sw = direct_palette_sw_addr(g);
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t r3 = (i >> 5) & 7u;
        uint32_t g3 = (i >> 2) & 7u;
        uint32_t b2 = i & 3u;
        uint32_t r5 = (r3 << 2) | (r3 >> 1);
        uint32_t g5 = (g3 << 2) | (g3 >> 1);
        uint32_t b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
        uint32_t v = (r5 << 11) | (g5 << 6) | (b5 << 1);
        direct_write_palette_entry(g, i, v);
        if (direct_ram_range(g, sw + i * 4u, 4u)) s3c2400_write32(g->soc, sw + i * 4u, v);
    }
    g->direct_fxe_palette_addr = sw;
    g->direct_fxe_palette_initialized = 1u;
}

static uint32_t direct_read_surface_bpp(gp32_t *g, uint32_t surf) {
    if (!direct_ram_range(g, surf, 28u)) return 0u;
    /* Official GPDRAWSURFACE offset +4 is bufflag, not bpp.  Older HLE builds
       abused that word as a bpp field; accept it only for backward-compatible
       hand-made surfaces, otherwise use the active graphics mode selected via
       GpGraphicModeSet. */
    uint32_t maybe_bpp = s3c2400_debug_read32(g->soc, surf + 4u);
    if (maybe_bpp == 8u || maybe_bpp == 16u) return maybe_bpp;
    return (g->direct_fxe_bpp == 16u) ? 16u : 8u;
}

static uint32_t direct_read_surface_buffer(gp32_t *g, uint32_t surf) {
    if (!direct_ram_range(g, surf, 28u)) return 0u;
    uint32_t fb = s3c2400_debug_read32(g->soc, surf + 0u);
    uint32_t bpp = direct_read_surface_bpp(g, surf);
    uint32_t bw = s3c2400_debug_read32(g->soc, surf + 8u);
    uint32_t bh = s3c2400_debug_read32(g->soc, surf + 12u);
    if ((bpp == 8u || bpp == 16u) && bw >= 240u && bw <= 320u && bh >= 240u && bh <= 320u &&
        direct_ram_range(g, fb, 240u * 320u * (bpp == 16u ? 2u : 1u))) {
        return fb;
    }
    return 0u;
}

static void direct_set_lcd_8bpp(gp32_t *g, uint32_t fb_addr, uint32_t pal_addr);

static void direct_fill_lcd_surface(gp32_t *g, uint32_t surf, uint32_t page) {
    if (!direct_ram_range(g, surf, 28u)) return;
    uint32_t fb = direct_surface_addr_for_bpp(g, page);
    g->direct_fxe_lcd_surface[page & 3u] = fb;
    /* Official GPDRAWSURFACE layout from gpgraphic.h:
       ptbuffer, bpp, buf_w, buf_h, ox, oy, o_buffer.  The buffer itself is the
       GP32 portrait LCD memory, while SDK drawing routines expose a 320x240
       landscape coordinate system.  Commercial SDK games request 16-bpp before
       asking for surfaces, so preserve that mode here instead of forcing an
       8-bpp descriptor. */
    direct_write32_if_ram(g, surf + 0u, fb);
    direct_write32_if_ram(g, surf + 4u, (g->direct_fxe_bpp == 16u) ? 16u : 8u);
    direct_write32_if_ram(g, surf + 8u, 320u);
    direct_write32_if_ram(g, surf + 12u, 240u);
    direct_write32_if_ram(g, surf + 16u, 0u);
    direct_write32_if_ram(g, surf + 20u, 0u);
    direct_write32_if_ram(g, surf + 24u, fb);
}



typedef struct direct_surface_stats {
    uint32_t checksum;
    uint32_t nonzero;
    uint32_t different_from_first;
    uint32_t transitions;
} direct_surface_stats_t;

static direct_surface_stats_t direct_surface_stats(gp32_t *g, uint32_t addr) {
    direct_surface_stats_t st;
    memset(&st, 0, sizeof(st));
    st.checksum = 2166136261u;
    if (!g || !direct_ram_range(g, addr, 240u * 320u)) return st;
    uint8_t first = 0, prev = 0;
    for (uint32_t off = 0; off < 240u * 320u; ++off) {
        uint32_t ba = (addr + off) & ~3u;
        uint32_t bw = s3c2400_debug_read32(g->soc, ba);
        uint8_t v = (uint8_t)(bw >> (((addr + off) & 3u) * 8u));
        if (off == 0u) first = v;
        else if (v != prev) st.transitions++;
        if (v) st.nonzero++;
        if (v != first) st.different_from_first++;
        st.checksum ^= v;
        st.checksum *= 16777619u;
        prev = v;
    }
    st.checksum ^= st.nonzero * 2654435761u;
    st.checksum ^= st.different_from_first * 2246822519u;
    st.checksum ^= st.transitions * 3266489917u;
    return st;
}

static uint32_t direct_surface_score(const direct_surface_stats_t *st) {
    if (!st || st->different_from_first == 0u) return 0u;
    /* A fully cleared white or black page has a large nonzero count but no
       picture content.  Score actual image structure instead. */
    uint32_t score = st->different_from_first + st->transitions * 4u;
    if (score < st->different_from_first) score = UINT32_MAX;
    return score;
}

static void direct_select_visible_surface(gp32_t *g) {
    if (!g || !g->direct_fxe_mode || g->direct_fxe_bpp != 8u) return;
    if (g->direct_fxe_lcd_explicit) return;
    uint32_t cur_score = 0u;
    if (g->direct_fxe_fb_addr) {
        direct_surface_stats_t cur = direct_surface_stats(g, g->direct_fxe_fb_addr);
        cur_score = direct_surface_score(&cur);
    }

    unsigned best = 4u;
    uint32_t best_score = 0u;
    int best_changed = 0;
    int prefer_changed = (cur_score == 0u);
    for (unsigned i = 0; i < 4u; ++i) {
        uint32_t addr = g->direct_fxe_lcd_surface[i];
        direct_surface_stats_t st = direct_surface_stats(g, addr);
        uint32_t score = direct_surface_score(&st);
        int changed = (st.checksum != g->direct_fxe_surface_checksum[i]);
        g->direct_fxe_surface_checksum[i] = st.checksum;
        if (score == 0u) continue;
        int take = 0;
        if (best == 4u) {
            take = 1;
        } else if (prefer_changed) {
            /* If the current LCD page is blank, use the best changed page, but
               do not let a low-content scratch/transition page win merely
               because it changed later. */
            if (changed != best_changed) take = changed;
            else if (score > best_score) take = 1;
        } else if (score > best_score) {
            /* Once a coherent LCD page is visible, choose by content quality
               rather than by recency. Some GPSDK programs update work pages during blits;
               choosing every changed page caused partially composed frames. */
            take = 1;
        }
        if (take) {
            best = i;
            best_score = score;
            best_changed = changed;
        }
    }
    if (best != 4u && g->direct_fxe_lcd_surface[best] != g->direct_fxe_fb_addr) {
        direct_set_lcd_8bpp(g, g->direct_fxe_lcd_surface[best], 0u);
    }
}

static void direct_set_lcd_16bpp(gp32_t *g, uint32_t fb_addr) {
    if (!g || !direct_ram_range(g, fb_addr, 240u * 320u * 2u)) return;
    g->direct_fxe_fb_addr = fb_addr;
    direct_update_stub_framebuffer(g);
    uint32_t start = fb_addr >> 1;
    uint32_t end = (fb_addr + 240u * 320u * 2u) >> 1;
    s3c2400_write32(g->soc, 0x14a00004u, (319u << 14));
    s3c2400_write32(g->soc, 0x14a00008u, (239u << 8));
    /* BIOS/GPSDK 16-bpp surfaces use LCDCON5 HWSWP.  Without it, the LCD DMA
       consumes each 32-bit word in the wrong halfword order.  Latin/UI shapes
       can still look mostly plausible, but Hangul glyphs become visually
       substituted or scrambled compared with a real BIOS boot. */
    s3c2400_write32(g->soc, 0x14a00010u, 0x00000701u);
    s3c2400_write32(g->soc, 0x14a00014u, start);
    s3c2400_write32(g->soc, 0x14a00018u, end & 0x001fffffu);
    s3c2400_write32(g->soc, 0x14a0001cu, 240u);
    g->direct_fxe_bpp = 16u;
    s3c2400_write32(g->soc, 0x14a00000u, 1u | (0x0cu << 1));
}

static void direct_set_lcd_8bpp(gp32_t *g, uint32_t fb_addr, uint32_t pal_addr) {
    /* Direct-loaded GPSDK programs do not have the BIOS display service behind
       SWI #0x16.  This installs an 8-bit TFT LCD view over the app's active
       portrait framebuffer and copies the caller-supplied 256-entry GP32
       palette into the S3C2400 LCD palette registers. */
    if (!g || !direct_ram_range(g, fb_addr, 240u * 320u)) return;
    g->direct_fxe_fb_addr = fb_addr;
    direct_update_stub_framebuffer(g);
    uint32_t start = fb_addr >> 1;
    uint32_t end = (fb_addr + 240u * 320u) >> 1;
    s3c2400_write32(g->soc, 0x14a00004u, (319u << 14));
    s3c2400_write32(g->soc, 0x14a00008u, (239u << 8));
    s3c2400_write32(g->soc, 0x14a00010u, 2u);
    s3c2400_write32(g->soc, 0x14a00014u, start);
    s3c2400_write32(g->soc, 0x14a00018u, end & 0x001fffffu);
    s3c2400_write32(g->soc, 0x14a0001cu, 120u);
    g->direct_fxe_bpp = 8u;
    if (direct_ram_range(g, pal_addr, 256u * 4u)) {
        uint32_t sw = direct_palette_sw_addr(g);
        if (direct_palette_looks_like_logpal(g, pal_addr)) {
            direct_copy_logpal_to_lcd(g, pal_addr);
        } else {
            g->direct_fxe_palette_addr = sw;
            for (uint32_t i = 0; i < 256u; ++i) {
                uint32_t v = s3c2400_debug_read32(g->soc, pal_addr + i * 4u) & 0xffffu;
                direct_write_palette_entry(g, i, v);
                if (direct_ram_range(g, sw + i * 4u, 4u)) s3c2400_write32(g->soc, sw + i * 4u, v);
            }
            g->direct_fxe_palette_initialized = 1u;
        }
    } else if (direct_ram_range(g, pal_addr, 256u * 2u)) {
        uint32_t sw = direct_palette_sw_addr(g);
        g->direct_fxe_palette_addr = sw;
        for (uint32_t i = 0; i < 256u; ++i) {
            uint32_t a = pal_addr + i * 2u;
            uint32_t raw = s3c2400_debug_read32(g->soc, a & ~3u);
            uint32_t v = ((a & 2u) ? (raw >> 16) : raw) & 0xffffu;
            direct_write_palette_entry(g, i, v);
            if (direct_ram_range(g, sw + i * 4u, 4u)) s3c2400_write32(g->soc, sw + i * 4u, v);
        }
        g->direct_fxe_palette_initialized = 1u;
    } else if (!g->direct_fxe_palette_initialized) {
        direct_fill_default_palette(g);
    }
    s3c2400_write32(g->soc, 0x14a00000u, 1u | (0x0bu << 1));
}


static void direct_reset_hle_runtime(gp32_t *g, int preserve_hle_options) {
    if (!g) return;
    uint32_t saved_rate_override = preserve_hle_options ? g->direct_hle_audio_rate_override : 0u;
    memset(g->direct_fpk_handles, 0, sizeof(g->direct_fpk_handles));
    g->direct_hle_file_open_addr = 0;
    g->direct_hle_file_read_addr[0] = 0;
    g->direct_hle_file_read_addr[1] = 0;
    g->direct_hle_file_close_addr = 0;
    g->direct_hle_file_size_addr = 0;
    g->direct_hle_file_seek_addr = 0;
    g->direct_hle_pathbuf_addr = 0;
    g->direct_hle_sound_dispatch_addr = 0;
    g->direct_hle_sound_table_addr = 0;
    g->direct_hle_sound_play_addr = 0;
    g->direct_hle_sound_state_addr = 0;
    g->direct_hle_pcm_env_addr = 0;
    g->direct_hle_pcm_init_addr = 0;
    g->direct_hle_pcm_play_addr = 0;
    g->direct_hle_pcm_stop_addr = 0;
    g->direct_hle_pcm_remove_addr = 0;
    g->direct_hle_pcm_lock_addr = 0;
    g->direct_hle_pcm_only_kill_addr = 0;
    g->direct_hle_pcm_initialized = 0;
    g->direct_hle_pcm_sr = 0;
    g->direct_hle_pcm_bit_count = 0;
    g->direct_hle_pcm_rate = 0;
    g->direct_hle_pcm_stereo = 0;
    g->direct_hle_pcm_bits = 16;
    g->direct_hle_pcm_active = 0;
    g->direct_hle_pcm_src_addr = 0;
    g->direct_hle_pcm_size_bytes = 0;
    g->direct_hle_pcm_pos_bytes = 0;
    g->direct_hle_pcm_repeat = 0;
    g->direct_hle_pcm_accum = 0;
    memset(g->direct_hle_pcm_ch, 0, sizeof(g->direct_hle_pcm_ch));
    g->direct_hle_sdk_sndmixedbuf_addr = 0;
    g->direct_hle_sdk_sndsrcexist_addr = 0;
    g->direct_hle_sdk_pcm_workidx_addr = 0;
    g->direct_hle_sdk_sndmixer_addr = 0;
    g->direct_hle_sdk_mixbuf0_addr = 0;
    g->direct_hle_sdk_mixbuf1_addr = 0;
    g->direct_hle_sdk_mixbuf_bytes = 0;
    g->direct_hle_sdk_rate = 0;
    g->direct_hle_sdk_accum = 0;
    g->direct_hle_sdk_last_submit_cycle = 0;
    g->direct_hle_sdk_timer_accum = 0;
    g->direct_hle_sdk_submitted_frames = 0;
    g->direct_hle_sdk_timer_table_addr = 0;
    memset(g->direct_hle_gpos_timer, 0, sizeof(g->direct_hle_gpos_timer));
    g->direct_hle_gpos_timers_enabled = 0;
    g->direct_hle_gpos_task_first = 0;
    g->direct_hle_gpos_task_last = 0;
    g->direct_hle_gpos_scheduler_callback = 0;
    g->direct_hle_callback_returned = 0;
    g->direct_hle_callback_running = 0;
    g->direct_hle_audio_rate_override = saved_rate_override;
    g->direct_hle_audio_last_auto_rate = 0;
    g->direct_hle_audio_asset = NULL;
    g->direct_hle_audio_pos = 0;
    g->direct_hle_audio_size = 0;
    g->direct_hle_audio_rate = 0;
    g->direct_hle_audio_accum = 0;
    memset(g->direct_hle_asset_autoload, 0, sizeof(g->direct_hle_asset_autoload));
}

static void direct_clear_reset_image(gp32_t *g) {
    if (!g) return;
    free(g->direct_reset_image.payload);
    memset(&g->direct_reset_image, 0, sizeof(g->direct_reset_image));
    g->direct_reset_image_valid = 0;
    g->direct_reset_scan_file_hle = 0;
    g->direct_reset_init_smc_gpio = 0;
}

static int direct_store_reset_image(gp32_t *g, const fxe_image_t *img, int scan_file_hle, int init_smc_gpio) {
    if (!g || !img || !img->payload || !img->payload_size) return 0;
    uint8_t *copy = (uint8_t *)malloc(img->payload_size);
    if (!copy) return 0;
    memcpy(copy, img->payload, img->payload_size);
    direct_clear_reset_image(g);
    g->direct_reset_image = *img;
    g->direct_reset_image.payload = copy;
    g->direct_reset_image_valid = 1u;
    g->direct_reset_scan_file_hle = scan_file_hle ? 1u : 0u;
    g->direct_reset_init_smc_gpio = init_smc_gpio ? 1u : 0u;
    return 1;
}

static void direct_clear_fpk_assets(gp32_t *g) {
    if (!g) return;
    for (size_t i = 0; i < g->direct_fpk_asset_count; ++i) free(g->direct_fpk_assets[i].data);
    free(g->direct_fpk_assets);
    g->direct_fpk_assets = NULL;
    g->direct_fpk_asset_count = 0;
    direct_reset_hle_runtime(g, 0);
}

static int direct_read_cstr(gp32_t *g, uint32_t addr, char *out, size_t out_len) {
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    if (!direct_ram_range(g, addr, 1u)) return 0;
    size_t n = 0;
    while (n + 1u < out_len && direct_ram_range(g, addr + (uint32_t)n, 1u)) {
        uint32_t ba = (addr + (uint32_t)n) & ~3u;
        uint32_t bw = s3c2400_debug_read32(g->soc, ba);
        uint8_t c = (uint8_t)(bw >> (((addr + (uint32_t)n) & 3u) * 8u));
        if (!c) { out[n] = '\0'; return n != 0; }
        if (c < 0x20u || c > 0x7eu) break;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return n != 0;
}

static void direct_norm_path(const char *in, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!in) return;
    size_t j = 0;
    const char *s = in;
    if ((s[0] == 'g' || s[0] == 'G') && (s[1] == 'p' || s[1] == 'P') && s[2] == ':') s += 3;
    while (*s == '\\' || *s == '/') ++s;
    for (; *s && j + 1u < out_len; ++s) {
        char c = *s;
        if (c == '\\') c = '/';
        out[j++] = (char)tolower((unsigned char)c);
    }
    while (j && out[j - 1u] == '/') --j;
    out[j] = '\0';
}

static const fpk_asset_t *direct_find_fpk_asset(gp32_t *g, const char *path) {
    if (!g || !path || !path[0]) return NULL;
    char q[320];
    direct_norm_path(path, q, sizeof(q));
    if (!q[0]) return NULL;

    int q_has_dir = strchr(q, '/') != NULL;
    size_t lq = strlen(q);
    for (size_t i = 0; i < g->direct_fpk_asset_count; ++i) {
        char p[320];
        direct_norm_path(g->direct_fpk_assets[i].path, p, sizeof(p));
        if (!strcmp(p, q)) return &g->direct_fpk_assets[i];

        size_t lp = strlen(p);
        if (q_has_dir) {
            if (lp >= lq && !strcmp(p + lp - lq, q) && (lp == lq || p[lp - lq - 1u] == '/')) return &g->direct_fpk_assets[i];
            if (lq >= lp && !strcmp(q + lq - lp, p) && (lq == lp || q[lq - lp - 1u] == '/')) return &g->direct_fpk_assets[i];
        } else {
            const char *base = strrchr(p, '/');
            base = base ? base + 1 : p;
            if (!strcmp(base, q)) return &g->direct_fpk_assets[i];
        }
    }
    return NULL;
}


static int direct_trace_enabled(void) {
    static int initialized = 0;
    static int enabled = 0;
    if (!initialized) {
        enabled = getenv("GP32_DIRECT_TRACE") ? 1 : 0;
        initialized = 1;
    }
    return enabled;
}

static void direct_trace_file_path(gp32_t *g, const char *op, uint32_t path_addr, const fpk_asset_t *a) {
    if (!direct_trace_enabled()) return;
    char path[320] = {0};
    if (path_addr) direct_read_cstr(g, path_addr, path, sizeof(path));
    fprintf(stderr, "[direct-hle] %s pc=%08x lr=%08x path_addr=%08x path='%s' asset='%s' size=%zu\n",
            op ? op : "file", g && g->cpu ? arm920t_get_pc(g->cpu) : 0u, g && g->cpu ? arm920t_get_reg(g->cpu, 14) : 0u,
            path_addr, path, a ? a->path : "", a ? a->size : (size_t)0u);
}

static void direct_trace_file_io(gp32_t *g, const char *op, uint32_t h, uint32_t dst, uint32_t want, size_t pos, size_t n, const fpk_asset_t *a, uint32_t st) {
    if (!direct_trace_enabled()) return;
    fprintf(stderr, "[direct-hle] %s pc=%08x lr=%08x h=%u dst=%08x want=%u pos=%zu n=%zu st=%u asset='%s'\n",
            op ? op : "io", g && g->cpu ? arm920t_get_pc(g->cpu) : 0u, g && g->cpu ? arm920t_get_reg(g->cpu, 14) : 0u,
            h, dst, want, pos, n, st, a ? a->path : "");
}


static int direct_ascii_equal_nocase(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void direct_asset_basename_lower(const fpk_asset_t *a, char *out, size_t out_len) {
    if (!out || !out_len) return;
    out[0] = '\0';
    if (!a || !a->path[0]) return;
    const char *base = a->path;
    for (const char *p = a->path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    size_t j = 0;
    while (base[j] && j + 1u < out_len) {
        out[j] = (char)tolower((unsigned char)base[j]);
        ++j;
    }
    out[j] = '\0';
}

static int direct_asset_is_gpg(const fpk_asset_t *a) {
    if (!a || !a->data || a->size <= 8u) return 0;
    char name[64];
    direct_asset_basename_lower(a, name, sizeof(name));
    size_t n = strlen(name);
    return n > 4u && strcmp(name + n - 4u, ".gpg") == 0 &&
           a->data[0] == 'g' && a->data[1] == 'p' && a->data[2] == 'g' && a->data[3] == ' ';
}

static void direct_note_asset_autoload(gp32_t *g, const fpk_asset_t *a) {
    if (!g || !direct_asset_is_gpg(a)) return;
    for (size_t i = 0; i < GP32_ARRAY_COUNT(g->direct_hle_asset_autoload); ++i) {
        if (g->direct_hle_asset_autoload[i].asset == a) return;
    }
    for (size_t i = 0; i < GP32_ARRAY_COUNT(g->direct_hle_asset_autoload); ++i) {
        if (!g->direct_hle_asset_autoload[i].asset || g->direct_hle_asset_autoload[i].copied) {
            g->direct_hle_asset_autoload[i].asset = a;
            g->direct_hle_asset_autoload[i].copied = 0u;
            g->direct_hle_asset_autoload[i].tries = 0u;
            return;
        }
    }
}

static int direct_buffer_looks_unloaded(gp32_t *g, uint32_t buf, uint32_t len) {
    if (!g || !len || !direct_ram_range(g, buf, len)) return 0;
    uint32_t sample = len < 256u ? len : 256u;
    uint32_t zero = 0u, ff = 0u;
    for (uint32_t i = 0; i < sample; ++i) {
        uint32_t ba = (buf + i) & ~3u;
        uint32_t bw = s3c2400_debug_read32(g->soc, ba);
        uint8_t c = (uint8_t)(bw >> (((buf + i) & 3u) * 8u));
        if (c == 0u) ++zero;
        if (c == 0xffu) ++ff;
    }
    return zero > (sample * 3u) / 4u || ff > (sample * 3u) / 4u;
}

static int direct_try_autoload_gpg_asset(gp32_t *g, const fpk_asset_t *a) {
    if (!g || !direct_asset_is_gpg(a)) return 0;
    char basename[64];
    direct_asset_basename_lower(a, basename, sizeof(basename));
    if (!basename[0]) return 0;
    uint32_t start = GP32_RAM_BASE;
    uint32_t end = g->direct_fxe_image_end;
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    if (end <= start || end > ram_end) end = ram_end;
    uint32_t payload_len = (uint32_t)(a->size - 8u);
    for (uint32_t saddr = start; saddr + 8u < end; ++saddr) {
        char lit[64];
        if (!direct_read_cstr(g, saddr, lit, sizeof(lit))) continue;
        if (!direct_ascii_equal_nocase(lit, basename)) continue;
        uint32_t back = (saddr > 0x28u) ? (saddr - 0x28u) : start;
        if (back < start) back = start;
        for (uint32_t p = saddr; p >= back + 4u; p -= 4u) {
            uint32_t slot_addr = s3c2400_debug_read32(g->soc, p - 4u);
            if (!direct_ram_range(g, slot_addr, 4u)) continue;
            uint32_t dst = s3c2400_debug_read32(g->soc, slot_addr);
            if (dst <= slot_addr || dst < end) break;
            if (!direct_ram_range(g, dst, payload_len)) break;
            if (!direct_buffer_looks_unloaded(g, dst, payload_len)) break;
            direct_copy_bytes_if_ram(g, dst, a->data + 8u, payload_len);
            if (direct_trace_enabled()) {
                fprintf(stderr, "[direct-hle] autoload/gpg asset='%s' literal=%08x slot=%08x dst=%08x n=%u\n",
                        a->path, saddr, slot_addr, dst, payload_len);
            }
            return 1;
        }
        saddr += (uint32_t)strlen(lit);
    }
    return 0;
}

static void direct_process_asset_autoload(gp32_t *g) {
    if (!g || !g->direct_fxe_mode || !g->direct_fpk_asset_count) return;
    for (size_t i = 0; i < GP32_ARRAY_COUNT(g->direct_hle_asset_autoload); ++i) {
        if (!g->direct_hle_asset_autoload[i].asset || g->direct_hle_asset_autoload[i].copied) continue;
        if (direct_try_autoload_gpg_asset(g, g->direct_hle_asset_autoload[i].asset)) {
            g->direct_hle_asset_autoload[i].copied = 1u;
        } else if (++g->direct_hle_asset_autoload[i].tries > 4096u) {
            g->direct_hle_asset_autoload[i].asset = NULL;
            g->direct_hle_asset_autoload[i].copied = 0u;
            g->direct_hle_asset_autoload[i].tries = 0u;
        }
    }
}

static const fpk_asset_t *direct_fpk_asset_from_cpu_path(gp32_t *g, uint32_t path_addr) {
    char path[320];

    /* Prefer the live string argument supplied by the intercepted open/size
       call.  The scanned SDK wrapper path-buffer pointer is only a fallback:
       some retail wrappers reuse one global filename buffer, and using it
       first can bind a new request to the previous filename.  Dyhard exposes
       this with title.pal: the stale buffer still names title.spr, causing the
       palette load to read sprite pixels as palette entries. */
    if (path_addr && direct_read_cstr(g, path_addr, path, sizeof(path))) {
        const fpk_asset_t *a = direct_find_fpk_asset(g, path);
        if (a) return a;
        for (uint32_t off = 0x20u; off <= 0x400u; off += 4u) {
            if (direct_read_cstr(g, path_addr + off, path, sizeof(path))) {
                a = direct_find_fpk_asset(g, path);
                if (a) return a;
            }
        }
    }
    if (g->direct_hle_pathbuf_addr && direct_read_cstr(g, g->direct_hle_pathbuf_addr, path, sizeof(path))) {
        const fpk_asset_t *a = direct_find_fpk_asset(g, path);
        if (a) return a;
    }
    return NULL;
}

static int direct_fpk_asset_is_sef(const fpk_asset_t *a, uint32_t *payload_size) {
    if (!a || a->size < 8u || !a->data) return 0;
    if (a->data[0] != 's' || a->data[1] != 'e' || a->data[2] != 'f' || a->data[3] != ' ') return 0;
    uint32_t n = gp32_ld32le(a->data + 4u);
    if (n > a->size - 8u) n = (uint32_t)(a->size - 8u);
    if (!n) return 0;
    if (payload_size) *payload_size = n;
    return 1;
}

static uint32_t direct_nearest_standard_audio_rate(uint32_t rate) {
    static const uint32_t rates[] = { 8000u, 11025u, 16000u, 22050u, 32000u, 44100u };
    uint32_t best = rates[0];
    uint32_t best_delta = rate > best ? rate - best : best - rate;
    for (size_t i = 1u; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        uint32_t r = rates[i];
        uint32_t delta = rate > r ? rate - r : r - rate;
        if (delta < best_delta) {
            best = r;
            best_delta = delta;
        }
    }
    return best;
}

static uint32_t direct_hle_sef_playback_rate(gp32_t *g) {
    if (!g) return 22050u;
    if (g->direct_hle_audio_rate_override >= 4000u && g->direct_hle_audio_rate_override <= 192000u) return g->direct_hle_audio_rate_override;

    /* GPSDK's stream helper feeds SEF data through GpPcmPlay() as raw unsigned
       8-bit mono PCM.  The SEF header only carries "sef " + payload length;
       the playback rate comes from the SDK stream ring-buffer byte rate.
       OMG reads 1024 bytes every 50 ms, i.e. 20480 8-bit samples/s, which
       snaps to the GP32's 22050 Hz PCM mode. */
    if (g->soc && g->direct_hle_sound_state_addr && direct_ram_range(g, g->direct_hle_sound_state_addr, 0x58u)) {
        uint32_t ring_bytes = s3c2400_debug_read32(g->soc, g->direct_hle_sound_state_addr + 0x14u);
        uint32_t period_ms = s3c2400_debug_read32(g->soc, g->direct_hle_sound_state_addr + 0x54u);
        if (ring_bytes >= 128u && ring_bytes <= 65536u && period_ms >= 1u && period_ms <= 1000u) {
            uint64_t samples_per_sec = ((uint64_t)ring_bytes * 1000u + (uint64_t)period_ms / 2u) / (uint64_t)period_ms;
            if (samples_per_sec >= 4000u && samples_per_sec <= 96000u) {
                uint32_t rate = direct_nearest_standard_audio_rate((uint32_t)samples_per_sec);
                g->direct_hle_audio_last_auto_rate = rate;
                return rate;
            }
        }
    }

    return g->direct_hle_audio_last_auto_rate ? g->direct_hle_audio_last_auto_rate : 22050u;
}

static int direct_play_sef_asset(gp32_t *g, const fpk_asset_t *a) {
    uint32_t n = 0;
    if (!g || !g->soc || !direct_fpk_asset_is_sef(a, &n)) return 0;
    if (g->direct_hle_audio_asset == a && g->direct_hle_audio_pos < g->direct_hle_audio_size) return 1;
    g->direct_hle_audio_asset = a;
    g->direct_hle_audio_pos = 0;
    g->direct_hle_audio_size = n;
    g->direct_hle_audio_rate = direct_hle_sef_playback_rate(g);
    g->direct_hle_audio_accum = 0;
    return 1;
}

static int direct_play_sef_path(gp32_t *g, uint32_t path_addr) {
    char path[320];
    if (!g || !path_addr || !direct_read_cstr(g, path_addr, path, sizeof(path))) return 0;
    return direct_play_sef_asset(g, direct_find_fpk_asset(g, path));
}

static void direct_stop_sef(gp32_t *g) {
    if (!g) return;
    g->direct_hle_audio_asset = NULL;
    g->direct_hle_audio_pos = 0;
    g->direct_hle_audio_size = 0;
    g->direct_hle_audio_accum = 0;
}

static uint8_t direct_read8_if_ram(gp32_t *g, uint32_t addr) {
    if (!direct_ram_range(g, addr, 1u)) return 0u;
    uint32_t bw = s3c2400_debug_read32(g->soc, addr & ~3u);
    return (uint8_t)(bw >> ((addr & 3u) * 8u));
}


static int direct_is_gp32_shadow_ramp_context(gp32_t *g, uint32_t addr, uint8_t lo, uint8_t hi) {
    if (!direct_ram_range(g, addr, 256u)) return 0;
    unsigned in_range = 0u;
    unsigned top = 0u;
    unsigned transitions = 0u;
    uint8_t prev = direct_read8_if_ram(g, addr);
    for (uint32_t i = 0; i < 256u; ++i) {
        uint8_t v = direct_read8_if_ram(g, addr + i);
        if (v >= lo && v <= hi) in_range++;
        if (v == hi) top++;
        if (i && v != prev) transitions++;
        prev = v;
    }
    return in_range >= 248u && top <= 8u && transitions >= 8u;
}

static int direct_blue_angelo_shadow_lut_present(gp32_t *g) {
    if (!g) return 0;
    uint32_t lut = GP32_RAM_BASE + 0x747200u;
    if (!direct_is_gp32_shadow_ramp_context(g, lut - 0x130u, 0x36u, 0x40u)) return 0;
    if (!direct_is_gp32_shadow_ramp_context(g, lut, 0x71u, 0x78u)) return 0;
    return 1;
}

static int direct_shadow_index(uint8_t v) {
    return v >= 0x71u && v <= 0x77u;
}

static int direct_surface_has_shadow_neighbour(gp32_t *g, uint32_t fb_addr, uint32_t width, uint32_t height, uint32_t x, uint32_t y) {
    for (int dy = -1; dy <= 1; ++dy) {
        int yy = (int)y + dy;
        if (yy < 0 || yy >= (int)height) continue;
        for (int dx = -1; dx <= 1; ++dx) {
            int xx = (int)x + dx;
            if ((dx == 0 && dy == 0) || xx < 0 || xx >= (int)width) continue;
            if (direct_shadow_index(direct_read8_if_ram(g, fb_addr + (uint32_t)yy * width + (uint32_t)xx))) return 1;
        }
    }
    return 0;
}

static void direct_fix_gp32_additive_blend_shadow_pixels(gp32_t *g) {
    /*
     * Repair already-composited Blue Angelo additive shadow pixels.  The LUT
     * endpoint fix below prevents newly drawn shadow from selecting palette
     * slot 0x78, but savestates and frame snapshots can already contain 0x78
     * in the active 8-bpp surface.  Do not globally remap yellow: only when the
     * Blue Angelo paired shadow LUT is present, walk the active 240x320 8-bpp
     * LCD surface and convert 0x78 pixels that are connected to the purple
     * shadow ramp (0x71..0x77).  Ordinary yellow HUD/moon artwork remains 0x78
     * because it is not part of that shadow-ramp component.
     */
    if (!g || !direct_blue_angelo_shadow_lut_present(g)) return;
    uint32_t lcdcon1 = s3c2400_debug_read32(g->soc, 0x14a00000u);
    uint32_t bppmode = GP32_BITS(lcdcon1, 4, 1);
    if (bppmode != 0x0bu) return;
    uint32_t lcdcon2 = s3c2400_debug_read32(g->soc, 0x14a00004u);
    uint32_t lcdcon3 = s3c2400_debug_read32(g->soc, 0x14a00008u);
    uint32_t height = GP32_BITS(lcdcon2, 23, 14) + 1u;
    uint32_t width = GP32_BITS(lcdcon3, 18, 8) + 1u;
    if (width == 0u || height == 0u || width > 240u || height > 320u) return;
    uint32_t fb_addr = s3c2400_debug_read32(g->soc, 0x14a00014u) << 1;
    if (!direct_ram_range(g, fb_addr, width * height)) return;

    for (unsigned pass = 0; pass < 8u; ++pass) {
        uint32_t changed = 0u;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t addr = fb_addr + y * width + x;
                if (direct_read8_if_ram(g, addr) != 0x78u) continue;
                if (!direct_surface_has_shadow_neighbour(g, fb_addr, width, height, x, y)) continue;
                direct_write8_if_ram(g, addr, 0x77u);
                changed++;
            }
        }
        if (!changed) break;
    }
}

static void direct_fix_gp32_additive_blend_shadow_endpoint(gp32_t *g) {
    /*
     * Additive-blend shadow-LUT compatibility guard.  v55 applied this data-shaped Blue
     * Angelo endpoint correction to a single broad byte-table shape.  That made
     * unrelated BIOS/game runtime data at the same RAM address eligible for
     * mutation and caused colour regressions outside the shadow test case.
     *
     * Do not key this to the current boot path: savestates and BIOS-launched
     * gameplay can legitimately contain the same table.  Instead, require the
     * neighbouring luminance/shadow table pair used by the same routine before
     * touching RAM.  The LCD palette/register model is left untouched; only the
     * runtime blend lookup endpoint is corrected when the paired LUTs prove this
     * is the additive-blend shadow table.
     */
    if (!g) return;
    uint32_t lut = GP32_RAM_BASE + 0x747200u;
    if (!direct_blue_angelo_shadow_lut_present(g)) return;
    for (uint32_t i = 0; i < 256u; ++i) {
        if (direct_read8_if_ram(g, lut + i) == 0x78u) direct_write8_if_ram(g, lut + i, 0x77u);
    }
}

static uint32_t direct_read32_if_ram(gp32_t *g, uint32_t addr) {
    if (!direct_ram_range(g, addr, 4u)) return 0u;
    return s3c2400_debug_read32(g->soc, addr);
}

static uint16_t direct_read_u16_if_ram(gp32_t *g, uint32_t addr) {
    uint16_t lo = direct_read8_if_ram(g, addr + 0u);
    uint16_t hi = direct_read8_if_ram(g, addr + 1u);
    return (uint16_t)(lo | (uint16_t)(hi << 8));
}

static int16_t direct_read_s16_if_ram(gp32_t *g, uint32_t addr) {
    return (int16_t)direct_read_u16_if_ram(g, addr);
}

static uint32_t direct_pcm_rate_from_sr(uint32_t sr) {
    switch (sr) {
    case 0u: case 1u: return 11025u; /* PCM_M11 / PCM_S11 */
    case 2u: case 3u: return 22050u; /* PCM_M22 / PCM_S22 */
    case 4u: case 5u: return 44100u; /* PCM_M44 / PCM_S44 */
    default: return 11025u;
    }
}

static uint32_t direct_pcm_stereo_from_sr(uint32_t sr) {
    return (sr == 1u || sr == 3u || sr == 5u) ? 1u : 0u;
}

static uint32_t direct_pcm_cursor_addr_channel(const gp32_t *g, uint32_t ch) {
    return direct_pcm_cursor_addr(g) + ch * 4u;
}

static void direct_pcm_sync_legacy(gp32_t *g) {
    if (!g) return;
    uint32_t first = GP32_DIRECT_PCM_CHANNELS;
    for (uint32_t ch = 0; ch < GP32_DIRECT_PCM_CHANNELS; ++ch) {
        if (g->direct_hle_pcm_ch[ch].active) { first = ch; break; }
    }
    if (first < GP32_DIRECT_PCM_CHANNELS) {
        g->direct_hle_pcm_active = 1u;
        g->direct_hle_pcm_src_addr = g->direct_hle_pcm_ch[first].src_addr;
        g->direct_hle_pcm_size_bytes = g->direct_hle_pcm_ch[first].size_bytes;
        g->direct_hle_pcm_pos_bytes = g->direct_hle_pcm_ch[first].pos_bytes;
        g->direct_hle_pcm_repeat = g->direct_hle_pcm_ch[first].repeat;
    } else {
        g->direct_hle_pcm_active = 0u;
        g->direct_hle_pcm_src_addr = 0u;
        g->direct_hle_pcm_size_bytes = 0u;
        g->direct_hle_pcm_pos_bytes = 0u;
        g->direct_hle_pcm_repeat = 0u;
    }
}

static void direct_pcm_update_cursor(gp32_t *g) {
    if (!g) return;
    for (uint32_t ch = 0; ch < GP32_DIRECT_PCM_CHANNELS; ++ch) {
        uint32_t cursor = g->direct_hle_pcm_ch[ch].active ?
            (g->direct_hle_pcm_ch[ch].src_addr + g->direct_hle_pcm_ch[ch].pos_bytes) : 0u;
        direct_write32_if_ram(g, direct_pcm_cursor_addr_channel(g, ch), cursor);
    }
    direct_pcm_sync_legacy(g);
}

static void direct_stop_pcm_channel(gp32_t *g, uint32_t ch) {
    if (!g || ch >= GP32_DIRECT_PCM_CHANNELS) return;
    memset(&g->direct_hle_pcm_ch[ch], 0, sizeof(g->direct_hle_pcm_ch[ch]));
    direct_write32_if_ram(g, direct_pcm_cursor_addr_channel(g, ch), 0u);
    direct_pcm_sync_legacy(g);
}

static void direct_stop_pcm(gp32_t *g) {
    if (!g) return;
    for (uint32_t ch = 0; ch < GP32_DIRECT_PCM_CHANNELS; ++ch) direct_stop_pcm_channel(g, ch);
    g->direct_hle_pcm_accum = 0;
    direct_pcm_update_cursor(g);
}

static void direct_stop_pcm_src(gp32_t *g, uint32_t src_addr) {
    if (!g) return;
    if (!src_addr) { direct_stop_pcm(g); return; }
    for (uint32_t ch = 0; ch < GP32_DIRECT_PCM_CHANNELS; ++ch) {
        if (g->direct_hle_pcm_ch[ch].active && g->direct_hle_pcm_ch[ch].src_addr == src_addr) {
            direct_stop_pcm_channel(g, ch);
        }
    }
    direct_pcm_update_cursor(g);
}

static int direct_start_pcm(gp32_t *g, uint32_t src_addr, uint32_t size_bytes, uint32_t repeat, uint32_t *out_ch) {
    if (out_ch) *out_ch = 0xffffffffu;
    if (!g || !g->soc || !src_addr || size_bytes < 1u || !direct_ram_range(g, src_addr, size_bytes)) return 0;
    if (!g->direct_hle_pcm_rate) g->direct_hle_pcm_rate = direct_pcm_rate_from_sr(g->direct_hle_pcm_sr);
    if (!g->direct_hle_pcm_bits) g->direct_hle_pcm_bits = 16u;

    uint32_t ch = GP32_DIRECT_PCM_CHANNELS;
    for (uint32_t i = 0; i < GP32_DIRECT_PCM_CHANNELS; ++i) {
        if (g->direct_hle_pcm_ch[i].active && g->direct_hle_pcm_ch[i].src_addr == src_addr) { ch = i; break; }
    }
    if (ch == GP32_DIRECT_PCM_CHANNELS) {
        for (uint32_t i = 0; i < GP32_DIRECT_PCM_CHANNELS; ++i) {
            if (!g->direct_hle_pcm_ch[i].active) { ch = i; break; }
        }
    }
    if (ch == GP32_DIRECT_PCM_CHANNELS) return 0;

    g->direct_hle_pcm_ch[ch].active = 1u;
    g->direct_hle_pcm_ch[ch].src_addr = src_addr;
    g->direct_hle_pcm_ch[ch].size_bytes = size_bytes;
    g->direct_hle_pcm_ch[ch].pos_bytes = 0u;
    g->direct_hle_pcm_ch[ch].repeat = repeat ? 1u : 0u;
    g->direct_hle_pcm_ch[ch].rate = g->direct_hle_pcm_rate ? g->direct_hle_pcm_rate : direct_pcm_rate_from_sr(g->direct_hle_pcm_sr);
    g->direct_hle_pcm_ch[ch].stereo = g->direct_hle_pcm_stereo;
    g->direct_hle_pcm_ch[ch].bits = g->direct_hle_pcm_bits ? g->direct_hle_pcm_bits : 16u;
    g->direct_hle_pcm_ch[ch].accum = 0u;
    direct_write32_if_ram(g, direct_pcm_cursor_addr_channel(g, ch), src_addr);
    direct_pcm_sync_legacy(g);
    if (out_ch) *out_ch = ch;
    return 1;
}

static uint32_t direct_pcm_channel_frame_bytes(const gp32_t *g, uint32_t ch) {
    if (!g || ch >= GP32_DIRECT_PCM_CHANNELS) return 0u;
    uint32_t b = (g->direct_hle_pcm_ch[ch].bits == 8u) ? 1u : 2u;
    if (g->direct_hle_pcm_ch[ch].stereo) b *= 2u;
    return b;
}

static int direct_pcm_channel_sample(gp32_t *g, uint32_t ch, uint32_t out_rate, int32_t *left, int32_t *right) {
    if (!g || ch >= GP32_DIRECT_PCM_CHANNELS || !left || !right) return 0;
    if (!g->direct_hle_pcm_ch[ch].active) return 0;
    uint32_t frame_bytes = direct_pcm_channel_frame_bytes(g, ch);
    if (!frame_bytes || g->direct_hle_pcm_ch[ch].size_bytes < frame_bytes) { direct_stop_pcm_channel(g, ch); return 0; }
    if (g->direct_hle_pcm_ch[ch].pos_bytes + frame_bytes > g->direct_hle_pcm_ch[ch].size_bytes) {
        if (g->direct_hle_pcm_ch[ch].repeat) g->direct_hle_pcm_ch[ch].pos_bytes = 0u;
        else { direct_stop_pcm_channel(g, ch); return 0; }
    }

    uint32_t addr = g->direct_hle_pcm_ch[ch].src_addr + g->direct_hle_pcm_ch[ch].pos_bytes;
    int16_t l = 0, r = 0;
    if (g->direct_hle_pcm_ch[ch].bits == 8u) {
        l = (int16_t)(((int)direct_read8_if_ram(g, addr) - 128) << 8);
        if (g->direct_hle_pcm_ch[ch].stereo) r = (int16_t)(((int)direct_read8_if_ram(g, addr + 1u) - 128) << 8);
        else r = l;
    } else {
        l = (int16_t)((int32_t)direct_read_u16_if_ram(g, addr) - 32768);
        if (g->direct_hle_pcm_ch[ch].stereo) r = (int16_t)((int32_t)direct_read_u16_if_ram(g, addr + 2u) - 32768);
        else r = l;
    }
    *left += l;
    *right += r;

    uint32_t rate = g->direct_hle_pcm_ch[ch].rate ? g->direct_hle_pcm_ch[ch].rate : 11025u;
    g->direct_hle_pcm_ch[ch].accum += (uint64_t)rate;
    uint32_t adv = (uint32_t)(g->direct_hle_pcm_ch[ch].accum / (uint64_t)out_rate);
    g->direct_hle_pcm_ch[ch].accum %= (uint64_t)out_rate;
    if (!adv) return 1;
    uint64_t new_pos = (uint64_t)g->direct_hle_pcm_ch[ch].pos_bytes + (uint64_t)adv * (uint64_t)frame_bytes;
    if (new_pos >= g->direct_hle_pcm_ch[ch].size_bytes) {
        if (g->direct_hle_pcm_ch[ch].repeat) new_pos %= g->direct_hle_pcm_ch[ch].size_bytes;
        else { direct_stop_pcm_channel(g, ch); return 1; }
    }
    g->direct_hle_pcm_ch[ch].pos_bytes = (uint32_t)new_pos;
    direct_write32_if_ram(g, direct_pcm_cursor_addr_channel(g, ch), g->direct_hle_pcm_ch[ch].src_addr + g->direct_hle_pcm_ch[ch].pos_bytes);
    direct_pcm_sync_legacy(g);
    return 1;
}

static int direct_hle_pcm_any_active(gp32_t *g) {
    if (!g) return 0;
    for (uint32_t ch = 0; ch < GP32_DIRECT_PCM_CHANNELS; ++ch) if (g->direct_hle_pcm_ch[ch].active) return 1;
    return 0;
}

static int direct_hle_sef_sample(gp32_t *g, uint32_t out_rate, int32_t *left, int32_t *right) {
    if (!g || !left || !right || !g->direct_hle_audio_asset || !g->direct_hle_audio_rate) return 0;
    if (g->direct_hle_audio_pos >= g->direct_hle_audio_size) { direct_stop_sef(g); return 0; }
    uint32_t off = 8u + g->direct_hle_audio_pos;
    const uint8_t *d = g->direct_hle_audio_asset->data;
    int16_t s = (int16_t)(((int)d[off] - 128) << 8);
    *left += s;
    *right += s;

    uint32_t rate = g->direct_hle_audio_rate ? g->direct_hle_audio_rate : 11025u;
    g->direct_hle_audio_accum += (uint64_t)rate;
    uint32_t adv = (uint32_t)(g->direct_hle_audio_accum / (uint64_t)out_rate);
    g->direct_hle_audio_accum %= (uint64_t)out_rate;
    if (adv) {
        uint64_t new_pos = (uint64_t)g->direct_hle_audio_pos + (uint64_t)adv;
        if (new_pos >= g->direct_hle_audio_size) direct_stop_sef(g);
        else g->direct_hle_audio_pos = (uint32_t)new_pos;
    }
    return 1;
}

static int16_t direct_mix_clamp_i16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static uint32_t direct_hle_mix_output_rate(gp32_t *g) {
    /* Keep the direct-HLE mixer at one output rate for the whole generated
       stream.  Individual GPSDK PCM sources can be 11/22/44 kHz and the SEF
       stream path is normally 22 kHz; switching the emulator's appended sample
       rate while a WAV/frontend buffer is already open makes earlier samples
       play at the wrong speed.  Mix every HLE stream into 44.1 kHz and resample
       each active source into that domain. */
    GP32_UNUSED(g);
    return 44100u;
}

static void direct_hle_pcm_tick(gp32_t *g, uint32_t cycles) {
    if (!g || !g->soc || !cycles) return;
    if (!g->direct_hle_audio_asset && !direct_hle_pcm_any_active(g)) return;
    uint32_t out_rate = direct_hle_mix_output_rate(g);
    uint32_t clock = direct_firmware_clock_hz(g);
    uint64_t scaled = g->direct_hle_pcm_accum + (uint64_t)cycles * (uint64_t)out_rate;
    uint32_t frames = (uint32_t)(scaled / (uint64_t)clock);
    g->direct_hle_pcm_accum = scaled % (uint64_t)clock;
    if (!frames) return;
    for (uint32_t i = 0; i < frames; ++i) {
        int32_t left = 0, right = 0;
        int active = 0;
        if (direct_hle_sef_sample(g, out_rate, &left, &right)) active = 1;
        for (uint32_t ch = 0; ch < GP32_DIRECT_PCM_CHANNELS; ++ch) {
            if (direct_pcm_channel_sample(g, ch, out_rate, &left, &right)) active = 1;
        }
        if (active) s3c2400_audio_append_s16_stereo(g->soc, direct_mix_clamp_i16(left), direct_mix_clamp_i16(right), out_rate);
    }
    direct_pcm_update_cursor(g);
}


static uint32_t direct_sdk_guess_rate_from_word(uint32_t v) {
    if (v == 11025u || v == 0x2b11u) return 11025u;
    if (v == 22050u || v == 0x5622u) return 22050u;
    if (v == 44100u || v == 0xac44u) return 44100u;
    return 0u;
}

static void direct_sdk_update_srcexist(gp32_t *g) {
    if (!g || !g->direct_hle_sdk_sndmixer_addr || !g->direct_hle_sdk_sndsrcexist_addr) return;
    uint32_t active = 0u;
    for (uint32_t ch = 0; ch < 4u; ++ch) {
        uint32_t e = g->direct_hle_sdk_sndmixer_addr + ch * 20u;
        if (direct_read32_if_ram(g, e + 4u) && direct_read32_if_ram(g, e + 12u)) { active = 1u; break; }
        if (direct_read32_if_ram(g, e + 0u) && direct_read32_if_ram(g, e + 8u)) { active = 1u; break; }
    }
    direct_write32_if_ram(g, g->direct_hle_sdk_sndsrcexist_addr, active);
}

static uint32_t direct_sdk_sound_buffer_base(gp32_t *g, uint32_t table_addr, uint32_t bytes) {
    if (!g || !g->soc || !bytes) return 0u;
    uint32_t need = (bytes + 15u) * 2u + 32u;
    uint32_t base = (table_addr + 0x400u + 15u) & ~15u;
    if (direct_ram_range(g, base, need)) return base;
    base = (g->direct_fxe_image_end + 0xfffu) & ~0xfffu;
    if (direct_ram_range(g, base, need) && base + need < g->direct_fxe_stack - 0x4000u) return base;
    base = GP32_RAM_BASE + 0x00770000u;
    if (direct_ram_range(g, base, need)) return base;
    return 0u;
}

static void direct_sdk_sound_alloc_buffers(gp32_t *g, uint32_t table_addr, uint32_t bytes, uint32_t state_addr) {
    if (!g || !table_addr) return;
    if (bytes < 16u) bytes = 0x180u;
    if (bytes > 0x10000u) bytes = 0x10000u;
    uint32_t base = direct_sdk_sound_buffer_base(g, table_addr, bytes);
    if (!base) return;
    uint32_t buf0 = base;
    uint32_t buf1 = (base + bytes + 15u) & ~15u;
    direct_zero_if_ram(g, buf0, bytes);
    direct_zero_if_ram(g, buf1, bytes);
    direct_write32_if_ram(g, table_addr + 0u, buf0);
    direct_write32_if_ram(g, table_addr + 4u, buf1);
    g->direct_hle_sdk_sndmixedbuf_addr = table_addr;
    g->direct_hle_sdk_mixbuf0_addr = buf0;
    g->direct_hle_sdk_mixbuf1_addr = buf1;
    g->direct_hle_sdk_mixbuf_bytes = bytes;
    if (state_addr && direct_ram_range(g, state_addr, 4u)) {
        g->direct_hle_sdk_sndsrcexist_addr = state_addr;
        g->direct_hle_sdk_pcm_workidx_addr = state_addr + 4u;
        if (state_addr >= 0x50u) g->direct_hle_sdk_sndmixer_addr = state_addr - 0x50u;
    }
    if (g->direct_hle_sdk_pcm_workidx_addr) direct_write32_if_ram(g, g->direct_hle_sdk_pcm_workidx_addr, 0u);
    direct_sdk_update_srcexist(g);
}

static void direct_sdk_submit_pcm_buffer(gp32_t *g, uint32_t buf, uint32_t bytes, uint32_t rate) {
    if (!g || !g->soc || !buf || bytes < 2u || !direct_ram_range(g, buf, bytes)) return;
    if (!rate) rate = g->direct_hle_sdk_rate ? g->direct_hle_sdk_rate : 44100u;
    if (rate < 4000u || rate > 96000u) rate = 44100u;
    uint32_t frames = bytes / 2u;
    for (uint32_t i = 0; i < frames; ++i) {
        int16_t s = direct_read_s16_if_ram(g, buf + i * 2u);
        s3c2400_audio_append_s16_stereo(g->soc, s, s, rate);
    }
    g->direct_hle_sdk_rate = rate;
    g->direct_hle_sdk_submitted_frames += frames;
    if (g->cpu) g->direct_hle_sdk_last_submit_cycle = arm920t_get_cycles(g->cpu);
}

static int direct_sdk_sound_mix_one(gp32_t *g, int16_t *out) {
    if (!g || !out || !g->direct_hle_sdk_sndmixer_addr) return 0;
    int32_t acc = 0;
    uint32_t active = 0u;
    for (uint32_t ch = 0; ch < 4u; ++ch) {
        uint32_t e = g->direct_hle_sdk_sndmixer_addr + ch * 20u;
        uint32_t src0 = direct_read32_if_ram(g, e + 0u);
        uint32_t cur = direct_read32_if_ram(g, e + 4u);
        uint32_t reload = direct_read32_if_ram(g, e + 8u);
        uint32_t remain = direct_read32_if_ram(g, e + 12u);
        uint32_t repeat = direct_read32_if_ram(g, e + 16u);
        if (!cur || !remain) {
            if (src0 && reload) {
                cur = src0;
                remain = reload;
            } else {
                continue;
            }
        }
        if (!direct_ram_range(g, cur, 2u)) {
            direct_write32_if_ram(g, e + 0u, 0u);
            direct_write32_if_ram(g, e + 4u, 0u);
            direct_write32_if_ram(g, e + 12u, 0u);
            continue;
        }
        acc += (int32_t)direct_read_u16_if_ram(g, cur) - 32768;
        cur += 2u;
        remain--;
        if (!remain) {
            if (repeat && src0 && reload) {
                cur = src0;
                remain = reload;
            } else {
                direct_write32_if_ram(g, e + 0u, 0u);
                cur = 0u;
            }
        }
        direct_write32_if_ram(g, e + 4u, cur);
        direct_write32_if_ram(g, e + 12u, remain);
        active++;
    }
    if (!active) {
        direct_sdk_update_srcexist(g);
        return 0;
    }
    int32_t mixed = acc / (int32_t)active;
    if (mixed < -32768) mixed = -32768;
    if (mixed > 32767) mixed = 32767;
    *out = (int16_t)mixed;
    direct_sdk_update_srcexist(g);
    return 1;
}


static uint32_t direct_find_sdk_timer_table(gp32_t *g) {
    if (!g || !g->direct_hle_sdk_sndmixer_addr) return 0u;
    if (g->direct_hle_sdk_timer_table_addr && direct_ram_range(g, g->direct_hle_sdk_timer_table_addr, 4u * 16u)) return g->direct_hle_sdk_timer_table_addr;
    uint32_t start = (g->direct_hle_sdk_sndmixer_addr > 0x2000u) ? (g->direct_hle_sdk_sndmixer_addr - 0x2000u) : GP32_RAM_BASE;
    if (start < GP32_RAM_BASE) start = GP32_RAM_BASE;
    uint32_t end = g->direct_hle_sdk_sndmixer_addr + 0x1000u;
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    if (end > ram_end) end = ram_end;
    for (uint32_t base = start; base + 64u <= end; base += 4u) {
        uint32_t good = 0u;
        for (uint32_t slot = 0; slot < 4u; ++slot) {
            uint32_t e = base + slot * 16u;
            uint32_t on = direct_read32_if_ram(g, e + 0u);
            uint32_t interval = direct_read32_if_ram(g, e + 8u);
            uint32_t cb = direct_read32_if_ram(g, e + 12u);
            if (on == 1u && interval && interval < 100000u && direct_ram_range(g, cb & ~1u, 4u)) good++;
        }
        if (good) {
            g->direct_hle_sdk_timer_table_addr = base;
            return base;
        }
    }
    return 0u;
}

static int direct_call_guest_function3(gp32_t *g, uint32_t fn, uint32_t r0, uint32_t r1, uint32_t r2) {
    if (!g || !g->cpu || g->direct_hle_callback_running || !direct_ram_range(g, fn & ~1u, 4u)) return 0;
    uint32_t saved_regs[16];
    for (unsigned i = 0; i < 16u; ++i) saved_regs[i] = arm920t_get_reg(g->cpu, i);
    uint32_t saved_cpsr = arm920t_get_cpsr(g->cpu);
    uint32_t cb_stack = direct_stub_addr(g) + 0x1f00u;
    if (!direct_ram_range(g, cb_stack - 0x300u, 0x300u)) return 0;
    g->direct_hle_callback_running = 1u;
    g->direct_hle_callback_returned = 0u;
    arm920t_set_cpsr(g->cpu, ARM_MODE_SVC | ARM_I_FLAG | ARM_F_FLAG);
    arm920t_set_reg(g->cpu, 0, r0);
    arm920t_set_reg(g->cpu, 1, r1);
    arm920t_set_reg(g->cpu, 2, r2);
    arm920t_set_reg(g->cpu, 3, 0u);
    arm920t_set_reg(g->cpu, 13, cb_stack);
    arm920t_set_reg(g->cpu, 14, direct_callback_return_stub_addr(g));
    arm920t_set_reg(g->cpu, 15, fn & ~1u);
    for (unsigned i = 0; i < 256u && !g->direct_hle_callback_returned; ++i) {
        uint32_t done = arm920t_run(g->cpu, 4096u);
        if (!done) break;
    }
    for (unsigned i = 0; i < 16u; ++i) arm920t_set_reg(g->cpu, i, saved_regs[i]);
    arm920t_set_cpsr(g->cpu, saved_cpsr);
    g->direct_hle_callback_running = 0u;
    return g->direct_hle_callback_returned ? 1 : 0;
}

static int direct_call_guest_callback(gp32_t *g, uint32_t callback) {
    return direct_call_guest_function3(g, callback, 0u, 0u, 0u);
}

static void direct_sdk_poll_timers(gp32_t *g) {
    if (g && g->direct_hle_sdk_sndmixer_addr) (void)direct_find_sdk_timer_table(g);
    /* Direct-FXE HLE must not re-enter arbitrary SDK timer callbacks from
       the host audio tick.  These callbacks are IRQ-context firmware code and
       can run while the foreground task owns an APCS stack frame; running them
       as normal guest calls corrupts homebrew such as AKA NOID when it reaches
       menu/gameplay code.  GPOS timer state is handled by
       direct_hle_gpos_timer_tick(), and PCM refill callbacks are dispatched
       separately on the private HLE callback stack. */
}




static void direct_gpos_timer_reset(gp32_t *g) {
    if (!g) return;
    memset(g->direct_hle_gpos_timer, 0, sizeof(g->direct_hle_gpos_timer));
    g->direct_hle_gpos_timers_enabled = 0u;
    g->direct_hle_gpos_task_first = 0u;
    g->direct_hle_gpos_task_last = 0u;
    g->direct_hle_gpos_scheduler_callback = 0u;
}

static int direct_gpos_task_table_bounds_valid(gp32_t *g, uint32_t first, uint32_t last_after) {
    if (!g || last_after < 0x34u) return 0;
    if (last_after <= first || last_after - first > 16u * 0x34u) return 0;
    if (((last_after - first) % 0x34u) != 0u) return 0;
    if (!direct_ram_range(g, first, 0x34u) || !direct_ram_range(g, last_after - 0x34u, 0x34u)) return 0;
    return direct_task_record_plausible(g, first) && direct_task_record_plausible(g, last_after - 0x34u);
}

static void direct_gpos_neutralize_internal_timer_tasks(gp32_t *g) {
    if (!g || !g->direct_hle_gpos_task_first || !g->direct_hle_gpos_task_last || !g->direct_hle_gpos_scheduler_callback) return;
    uint32_t cb = g->direct_hle_gpos_scheduler_callback & ~1u;
    uint32_t lo = cb + 0x280u;
    uint32_t hi = cb + 0x330u;
    for (uint32_t t = g->direct_hle_gpos_task_first; t <= g->direct_hle_gpos_task_last; t += 0x34u) {
        if (!direct_task_record_plausible(g, t)) continue;
        uint32_t entry = s3c2400_debug_read32(g->soc, t + 0x30u) & ~1u;
        if (entry >= lo && entry < hi) {
            /* The resident GPOS timer-process threads are entered from the
               firmware timer IRQ scheduler.  Direct-FXE HLE does not construct
               that IRQ return frame, so these firmware contexts must remain
               host-emulated rather than being restored as normal game threads. */
            direct_write32_if_ram(g, t + 0x14u, 8u);
            direct_write32_if_ram(g, t + 0x20u, 0u);
            direct_write32_if_ram(g, t + 0x24u, 0xffffffffu);
        }
    }
}

static void direct_gpos_timer_note_scheduler_bounds(gp32_t *g, uint32_t callback) {
    if (!g || !callback) return;
    uint32_t cb = callback & ~1u;
    if (!direct_ram_range(g, cb, 4u)) return;
    for (uint32_t off = 0x180u; off <= 0x580u; off += 4u) {
        uint32_t first = direct_read32_if_ram(g, cb + off + 0x10u);
        uint32_t last_after = direct_read32_if_ram(g, cb + off + 0x14u);
        if (!direct_gpos_task_table_bounds_valid(g, first, last_after)) continue;
        g->direct_hle_gpos_task_first = first;
        g->direct_hle_gpos_task_last = last_after - 0x34u;
        g->direct_hle_gpos_scheduler_callback = cb;
        direct_gpos_neutralize_internal_timer_tasks(g);
        return;
    }
}

static int direct_gpos_task_is_internal_timer(gp32_t *g, uint32_t task_addr) {
    if (!g || !g->direct_hle_gpos_scheduler_callback || !direct_ram_range(g, task_addr, 0x34u)) return 0;
    uint32_t entry = s3c2400_debug_read32(g->soc, task_addr + 0x30u) & ~1u;
    uint32_t cb = g->direct_hle_gpos_scheduler_callback & ~1u;
    return entry >= cb + 0x280u && entry < cb + 0x330u;
}

static int direct_try_emulate_gpos_counter_callback(gp32_t *g, uint32_t callback, uint32_t fires) {
    if (!g || !fires) return 0;
    uint32_t cb = callback & ~1u;
    for (uint32_t off = 0x180u; off <= 0x380u; off += 4u) {
        uint32_t sub_addr = direct_read32_if_ram(g, cb + off + 0u);
        uint32_t threshold_addr = direct_read32_if_ram(g, cb + off + 4u);
        uint32_t tick_addr = direct_read32_if_ram(g, cb + off + 8u);
        if (!direct_ram_range(g, sub_addr, 4u) || !direct_ram_range(g, threshold_addr, 4u) || !direct_ram_range(g, tick_addr, 4u)) continue;
        uint32_t threshold = direct_read32_if_ram(g, threshold_addr);
        if (threshold == 0u || threshold > 1000000u) continue;
        uint64_t limit = (uint64_t)threshold * 2ull;
        uint64_t sub = direct_read32_if_ram(g, sub_addr);
        uint64_t total = sub + (uint64_t)fires;
        uint32_t ticks = direct_read32_if_ram(g, tick_addr);
        ticks += (uint32_t)(total / limit);
        sub = total % limit;
        direct_write32_if_ram(g, sub_addr, (uint32_t)sub);
        direct_write32_if_ram(g, tick_addr, ticks);
        return 1;
    }
    return 0;
}

static int direct_gpos_callback_is_scheduler(gp32_t *g, uint32_t callback) {
    if (!g || !callback) return 0;
    direct_gpos_timer_note_scheduler_bounds(g, callback);
    return g->direct_hle_gpos_scheduler_callback == (callback & ~1u);
}

static void direct_hle_gpos_timer_tick(gp32_t *g, uint32_t cycles) {
    if (!g || !g->direct_fxe_mode || !g->direct_hle_gpos_timers_enabled || !cycles) return;
    uint32_t clock = direct_run_clock_hz(g);
    if (!clock) clock = 66000000u;
    for (uint32_t i = 0; i < GP32_DIRECT_GPOS_TIMER_COUNT; ++i) {
        uint32_t cb = g->direct_hle_gpos_timer[i].callback;
        uint32_t tps = g->direct_hle_gpos_timer[i].tps;
        if (!g->direct_hle_gpos_timer[i].configured || !g->direct_hle_gpos_timer[i].enabled || !cb || !tps) continue;
        if (tps > 200000u) tps = 200000u;
        uint64_t scaled = g->direct_hle_gpos_timer[i].accum + (uint64_t)cycles * (uint64_t)tps;
        uint32_t fires = (uint32_t)(scaled / (uint64_t)clock);
        g->direct_hle_gpos_timer[i].accum = scaled % (uint64_t)clock;
        if (!fires) continue;
        if (direct_gpos_callback_is_scheduler(g, cb)) {
            direct_gpos_neutralize_internal_timer_tasks(g);
            uint32_t ticks = fires > 64u ? 64u : fires;
            for (uint32_t n = 0; n < ticks; ++n) direct_tick_sdk_task_sleepers(g, g->direct_hle_gpos_task_first, g->direct_hle_gpos_task_last);
            continue;
        }
        if (direct_try_emulate_gpos_counter_callback(g, cb, fires)) continue;
        if (tps <= 1000u && direct_ram_range(g, cb & ~1u, 4u)) {
            uint32_t calls = fires > 8u ? 8u : fires;
            for (uint32_t n = 0; n < calls; ++n) direct_call_guest_callback(g, cb);
        }
    }
}

static int direct_handle_swi_gpos_timer(gp32_t *g, arm920t_t *cpu, uint32_t pc) {
    if (!g || !cpu) return 0;
    uint32_t cmdp = arm920t_get_reg(cpu, 0);
    if (!direct_ram_range(g, cmdp, 4u)) {
        arm920t_set_reg(cpu, 0, 0u);
        return 1;
    }
    uint32_t cmd = direct_read32_if_ram(g, cmdp + 0u);
    /* Some GPSDK/GPOS task code invokes SWI #0x13 from an IRQ-return style
       wrapper.  In direct-FXE HLE the SWI itself is handled inline, so the
       guest's following MOVS PC,LR/SPSR path is not a real exception return.
       Emulate the wrapper epilogue and continue at its saved LR instead. */
    if (cmd == 2u && direct_ram_range(g, cmdp, 32u)) {
        uint32_t selector = direct_read32_if_ram(g, cmdp + 4u);
        uint32_t saved_spsr = direct_read32_if_ram(g, cmdp + 8u);
        if (selector == 1u && ((saved_spsr & 0x1fu) == ARM_MODE_SVC || (saved_spsr & 0x1fu) == 0x10u || (saved_spsr & 0x1fu) == 0x1fu)) {
            uint32_t saved_lr = direct_read32_if_ram(g, cmdp + 28u);
            if (direct_ram_range(g, saved_lr & ~1u, 4u)) {
                arm920t_set_reg(cpu, 0, direct_read32_if_ram(g, cmdp + 12u));
                arm920t_set_reg(cpu, 1, direct_read32_if_ram(g, cmdp + 16u));
                arm920t_set_reg(cpu, 2, direct_read32_if_ram(g, cmdp + 20u));
                arm920t_set_reg(cpu, 3, direct_read32_if_ram(g, cmdp + 24u));
                arm920t_set_reg(cpu, 13, cmdp + 32u);
                arm920t_set_reg(cpu, 14, saved_lr);
                arm920t_set_cpsr(cpu, saved_spsr);
                arm920t_set_reg(cpu, 15, saved_lr & ~1u);
                (void)pc;
                return 1;
            }
        }
    }
    switch (cmd) {
    case 0u:
        direct_gpos_timer_reset(g);
        break;
    case 1u: {
        uint32_t idx = direct_read32_if_ram(g, cmdp + 4u);
        uint32_t cb = direct_read32_if_ram(g, cmdp + 8u);
        uint32_t tps = direct_read32_if_ram(g, cmdp + 12u);
        uint32_t max_exec = direct_read32_if_ram(g, cmdp + 16u);
        if (idx < GP32_DIRECT_GPOS_TIMER_COUNT && direct_ram_range(g, cb & ~1u, 4u) && tps) {
            g->direct_hle_gpos_timer[idx].configured = 1u;
            g->direct_hle_gpos_timer[idx].enabled = 0u;
            g->direct_hle_gpos_timer[idx].callback = cb;
            g->direct_hle_gpos_timer[idx].tps = tps;
            g->direct_hle_gpos_timer[idx].max_exec_tick = max_exec;
            g->direct_hle_gpos_timer[idx].accum = 0u;
            direct_gpos_timer_note_scheduler_bounds(g, cb);
        }
        break;
    }
    case 2u: {
        uint32_t arg = direct_read32_if_ram(g, cmdp + 4u);
        if (arg < GP32_DIRECT_GPOS_TIMER_COUNT && g->direct_hle_gpos_timer[arg].configured) {
            g->direct_hle_gpos_timer[arg].enabled = 1u;
        } else {
            for (uint32_t i = 0; i < GP32_DIRECT_GPOS_TIMER_COUNT; ++i) {
                if (g->direct_hle_gpos_timer[i].configured) g->direct_hle_gpos_timer[i].enabled = 1u;
            }
        }
        g->direct_hle_gpos_timers_enabled = 1u;
        direct_gpos_neutralize_internal_timer_tasks(g);
        break;
    }
    case 3u: {
        uint32_t idx = direct_read32_if_ram(g, cmdp + 4u);
        if (idx < GP32_DIRECT_GPOS_TIMER_COUNT) g->direct_hle_gpos_timer[idx].enabled = 0u;
        break;
    }
    case 4u: {
        uint32_t idx = direct_read32_if_ram(g, cmdp + 4u);
        if (idx < GP32_DIRECT_GPOS_TIMER_COUNT && g->direct_hle_gpos_timer[idx].configured) {
            g->direct_hle_gpos_timer[idx].enabled = 1u;
            g->direct_hle_gpos_timers_enabled = 1u;
            direct_gpos_neutralize_internal_timer_tasks(g);
        }
        break;
    }
    case 5u: {
        uint32_t idx = direct_read32_if_ram(g, cmdp + 4u);
        if (idx < GP32_DIRECT_GPOS_TIMER_COUNT) memset(&g->direct_hle_gpos_timer[idx], 0, sizeof(g->direct_hle_gpos_timer[idx]));
        break;
    }
    default:
        break;
    }
    arm920t_set_reg(cpu, 0, 0u);
    return 1;
}

static void direct_sdk_pcm_refill_tick(gp32_t *g) {
    if (!g || !g->direct_hle_sdk_sndmixer_addr) return;
    uint32_t entry = g->direct_hle_sdk_sndmixer_addr;
    uint32_t cursor_ptr_addr = 0u;
    uint32_t start = (entry > 0x1000u) ? entry - 0x1000u : GP32_RAM_BASE;
    uint32_t end = entry + 0x1000u;
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    if (end > ram_end) end = ram_end;
    for (uint32_t a = start; a + 0x24u < end; a += 4u) {
        if (direct_read32_if_ram(g, a) != entry + 4u) continue;
        uint32_t base = direct_read32_if_ram(g, a + 0x0cu);
        uint32_t block = a + 0x14u;
        uint32_t obj = direct_read32_if_ram(g, block + 8u);
        uint32_t fill = direct_read32_if_ram(g, block + 12u);
        if (direct_ram_range(g, base, 2u) && direct_ram_range(g, obj, 4u) && direct_ram_range(g, fill & ~1u, 4u)) {
            cursor_ptr_addr = a;
            break;
        }
    }
    if (!cursor_ptr_addr) return;
    uint32_t last_half_addr = cursor_ptr_addr - 4u;
    uint32_t shift = direct_read32_if_ram(g, cursor_ptr_addr + 8u);
    uint32_t base = direct_read32_if_ram(g, cursor_ptr_addr + 0x0cu);
    uint32_t block = cursor_ptr_addr + 0x14u;
    uint32_t half_units = direct_read32_if_ram(g, block + 4u);
    uint32_t obj = direct_read32_if_ram(g, block + 8u);
    uint32_t fill = direct_read32_if_ram(g, block + 12u);
    uint32_t cur = direct_read32_if_ram(g, entry + 4u);
    if (!base || !cur || !half_units || shift > 4u || !direct_ram_range(g, fill & ~1u, 4u)) return;
    uint32_t half_bytes = half_units << shift;
    if (!half_bytes || half_bytes > 0x20000u) return;
    uint32_t current_half = ((cur - base) >= half_bytes) ? 1u : 0u;
    uint32_t previous_half = direct_read32_if_ram(g, last_half_addr) & 1u;
    if (current_half == previous_half) return;
    uint32_t dst = base + (previous_half ? half_bytes : 0u);
    if (!direct_ram_range(g, dst, half_bytes)) return;
    if (direct_call_guest_function3(g, fill, obj, dst, half_bytes)) direct_write32_if_ram(g, last_half_addr, current_half);
}

static void direct_sdk_sound_tick(gp32_t *g, uint32_t cycles) {
    if (!g || !g->soc || !cycles || !g->direct_hle_sdk_sndmixer_addr) return;
    uint32_t rate = g->direct_hle_sdk_rate ? g->direct_hle_sdk_rate : 44100u;
    uint32_t clock = direct_run_clock_hz(g);
    if (!clock) clock = 66000000u;
    if (g->direct_hle_sdk_last_submit_cycle && g->cpu) {
        uint64_t now = arm920t_get_cycles(g->cpu);
        if (now >= g->direct_hle_sdk_last_submit_cycle && now - g->direct_hle_sdk_last_submit_cycle < (uint64_t)clock / 30u) return;
    }
    uint64_t scaled = g->direct_hle_sdk_accum + (uint64_t)cycles * (uint64_t)rate;
    uint32_t frames = (uint32_t)(scaled / (uint64_t)clock);
    g->direct_hle_sdk_accum = scaled % (uint64_t)clock;
    if (!frames) return;
    g->direct_hle_sdk_timer_accum += frames;
    if (g->direct_hle_sdk_timer_accum >= 64u) {
        g->direct_hle_sdk_timer_accum = 0u;
        direct_sdk_poll_timers(g);
        direct_sdk_pcm_refill_tick(g);
    }
    for (uint32_t i = 0; i < frames; ++i) {
        int16_t sample = 0;
        if (!direct_sdk_sound_mix_one(g, &sample)) break;
        s3c2400_audio_append_s16_stereo(g->soc, sample, sample, rate);
    }
}

static int direct_handle_swi_set_sndbuffer(gp32_t *g, arm920t_t *cpu) {
    if (!g || !cpu) return 0;
    uint32_t table_addr = arm920t_get_reg(cpu, 0);
    uint32_t bytes = arm920t_get_reg(cpu, 1);
    uint32_t state_addr = arm920t_get_reg(cpu, 3);
    direct_sdk_sound_alloc_buffers(g, table_addr, bytes, state_addr);
    arm920t_set_reg(cpu, 0, 0u);
    return 1;
}

static int direct_handle_swi_iis(gp32_t *g, arm920t_t *cpu) {
    if (!g || !cpu) return 0;
    uint32_t cmdp = arm920t_get_reg(cpu, 0);
    uint32_t cmd = direct_read32_if_ram(g, cmdp + 0u);
    if (!cmd) cmd = arm920t_get_reg(cpu, 3);
    for (uint32_t i = 0; i < 10u && direct_ram_range(g, cmdp + i * 4u, 4u); ++i) {
        uint32_t rate = direct_sdk_guess_rate_from_word(direct_read32_if_ram(g, cmdp + i * 4u));
        if (rate) { g->direct_hle_sdk_rate = rate; break; }
    }
    switch (cmd & 0xffffu) {
    case 0x0180u: {
        uint32_t buf = direct_read32_if_ram(g, cmdp + 8u);
        uint32_t bytes = direct_read32_if_ram(g, cmdp + 16u);
        if (!buf) buf = arm920t_get_reg(cpu, 1);
        if (!bytes) bytes = arm920t_get_reg(cpu, 3);
        direct_sdk_submit_pcm_buffer(g, buf, bytes, g->direct_hle_sdk_rate);
        break;
    }
    case 0x1000u:
    case 0x8010u:
    case 0x4010u:
    case 0x2000u:
    case 0x0020u:
    case 0x0002u:
    default:
        break;
    }
    arm920t_set_reg(cpu, 0, 1u);
    return 1;
}

static void direct_hle_audio_tick(gp32_t *g, uint32_t cycles) {
    if (!g || !cycles) return;
    direct_hle_pcm_tick(g, cycles);
    direct_sdk_sound_tick(g, cycles);
}

static uint32_t direct_alloc_fpk_handle(gp32_t *g, const fpk_asset_t *asset) {
    if (!g || !asset) return 0;
    for (uint32_t i = 1u; i < 32u; ++i) {
        if (!g->direct_fpk_handles[i].used) {
            g->direct_fpk_handles[i].used = 1;
            g->direct_fpk_handles[i].asset = asset;
            g->direct_fpk_handles[i].pos = 0;
            return i;
        }
    }
    return 0;
}

static const fpk_asset_t *direct_handle_asset(gp32_t *g, uint32_t h, size_t **posp) {
    uint32_t idx = h & 31u;
    if (!g || idx == 0u || idx >= 32u || !g->direct_fpk_handles[idx].used || (h >> 24u) != 0u) return NULL;
    if (posp) *posp = &g->direct_fpk_handles[idx].pos;
    return g->direct_fpk_handles[idx].asset;
}


static uint32_t direct_arm_branch_target(uint32_t pc, uint32_t insn) {
    int32_t imm = (int32_t)(insn & 0x00ffffffu);
    if (imm & 0x00800000) imm |= (int32_t)0xff000000u;
    return pc + 8u + ((uint32_t)imm << 2);
}

static int direct_arm_bl_to(uint32_t pc, uint32_t insn, uint32_t target) {
    return (insn & 0x0f000000u) == 0x0b000000u && direct_arm_branch_target(pc, insn) == target;
}

static int direct_arm_is_bl(uint32_t insn) {
    return (insn & 0x0f000000u) == 0x0b000000u;
}

static int direct_file_helper_looks_read(gp32_t *g, uint32_t target) {
    if (!g || !direct_ram_range(g, target, 0x18u)) return 0;
    uint32_t w0 = s3c2400_debug_read32(g->soc, target + 0x00u);
    uint32_t w1 = s3c2400_debug_read32(g->soc, target + 0x04u);
    uint32_t w2 = s3c2400_debug_read32(g->soc, target + 0x08u);
    uint32_t w3 = s3c2400_debug_read32(g->soc, target + 0x0cu);
    uint32_t w4 = s3c2400_debug_read32(g->soc, target + 0x10u);
    if (w0 == 0xe92d4fffu && w1 == 0xe24dd00cu && w2 == 0xe1a05001u &&
        w3 == 0xe1a04002u && w4 == 0xe3a0107fu) return 1;
    return 0;
}

static int direct_file_helper_looks_seek(gp32_t *g, uint32_t target) {
    if (!g || !direct_ram_range(g, target, 0x20u)) return 0;
    uint32_t w0 = s3c2400_debug_read32(g->soc, target + 0x00u);
    uint32_t w1 = s3c2400_debug_read32(g->soc, target + 0x04u);
    uint32_t w2 = s3c2400_debug_read32(g->soc, target + 0x08u);
    uint32_t w3 = s3c2400_debug_read32(g->soc, target + 0x0cu);
    uint32_t w4 = s3c2400_debug_read32(g->soc, target + 0x10u);
    uint32_t w5 = s3c2400_debug_read32(g->soc, target + 0x14u);
    /* Retail file seek helper variants keep handle in r0, seek mode in r1,
       signed offset in r2, and the out-position pointer in r3.  W.B.W. uses
       the second ordering below; treating it as read made asset loaders copy
       data to address 0x00000001 instead of seeking inside .PAK files. */
    if (w0 == 0xe92d4ff8u && w1 == 0xe1a05001u && w2 == 0xe1a04002u &&
        w3 == 0xe1a06003u && w4 == 0xe3a0107fu && (w5 & 0x0fffff00u) == 0x00017c00u) return 1;
    if (w0 == 0xe92d4ff8u && w1 == 0xe1a05001u && w2 == 0xe3a0107fu &&
        w3 == 0xe0017c40u && w4 == 0xe1a04002u && w5 == 0xe1a06003u) return 1;
    return 0;
}

static void direct_scan_file_hle(gp32_t *g) {
    if (!g || !g->direct_fpk_asset_count || !g->direct_fxe_mode) return;
    uint32_t start = GP32_RAM_BASE;
    uint32_t end = g->direct_fxe_image_end;
    if (end <= start || end > GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc)) end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    for (uint32_t a = start; a + 0x90u < end; a += 4u) {
        uint32_t w0 = s3c2400_debug_read32(g->soc, a + 0u);
        uint32_t w1 = s3c2400_debug_read32(g->soc, a + 4u);
        uint32_t w2 = s3c2400_debug_read32(g->soc, a + 8u);
        uint32_t w3 = s3c2400_debug_read32(g->soc, a + 12u);
        uint32_t w4 = s3c2400_debug_read32(g->soc, a + 16u);
        if (w0 == 0xe92d40f0u && w1 == 0xe1a06000u && w2 == 0xe1a05001u && w3 == 0xe1a04002u) {
            uint32_t bl = s3c2400_debug_read32(g->soc, a + 0x10u);
            uint32_t after_bl = s3c2400_debug_read32(g->soc, a + 0x14u);
            uint32_t cmp = s3c2400_debug_read32(g->soc, a + 0x18u);
            uint32_t ret_err2 = s3c2400_debug_read32(g->soc, a + 0x20u);
            int open_like = (after_bl & 0xffff0000u) == 0xe51f0000u && cmp == 0xe3500000u && ret_err2 == 0x03a00002u && direct_arm_bl_to(a + 0x10u, bl, GP32_RAM_BASE + 0x00000e64u);
            int legacy_open_like = (s3c2400_debug_read32(g->soc, a + 0x60u) == 0xe51f0224u);
            int retail_open_like = (w0 == 0xe92d40f0u && w1 == 0xe1a06000u && w2 == 0xe1a05001u && w3 == 0xe1a04002u &&
                                    (w4 & 0x0f000000u) == 0x0b000000u && s3c2400_debug_read32(g->soc, a + 0x28u) == 0xe1a00006u &&
                                    (direct_arm_bl_to(a + 0x60u, s3c2400_debug_read32(g->soc, a + 0x60u), GP32_RAM_BASE + 0x00109118u) ||
                                     direct_arm_bl_to(a + 0x60u, s3c2400_debug_read32(g->soc, a + 0x60u), GP32_RAM_BASE + 0x00109428u)));
            if ((open_like || legacy_open_like || retail_open_like) && g->direct_hle_file_open_addr != a) {
                if (!g->direct_hle_file_open_addr) g->direct_hle_file_open_addr = a;
                if (legacy_open_like) {
                    uint32_t lit = a + 0x60u + 8u - (s3c2400_debug_read32(g->soc, a + 0x60u) & 0xfffu);
                    if (direct_ram_range(g, lit, 4u)) g->direct_hle_pathbuf_addr = s3c2400_debug_read32(g->soc, lit);
                } else {
                    uint32_t lit = a + 0x14u + 8u - (after_bl & 0xfffu);
                    if (direct_ram_range(g, lit, 4u)) g->direct_hle_pathbuf_addr = s3c2400_debug_read32(g->soc, lit);
                }
                s3c2400_write32(g->soc, a, 0xef070001u);
            }
        }
        if ((w0 == 0xe92d40f0u && w1 == 0xe1a07000u && w2 == 0xe1a06001u && w3 == 0xe1a05002u && w4 == 0xe1a04003u)) {
            uint32_t body_pc = a + 0x54u;
            uint32_t body_bl = s3c2400_debug_read32(g->soc, body_pc);
            if (!direct_arm_is_bl(body_bl)) {
                body_pc = a + 0x58u;
                body_bl = s3c2400_debug_read32(g->soc, body_pc);
            }
            uint32_t body_target = direct_arm_is_bl(body_bl) ? direct_arm_branch_target(body_pc, body_bl) : 0u;
            int retail_seek_like = direct_arm_bl_to(body_pc, body_bl, GP32_RAM_BASE + 0x00109b68u) || direct_file_helper_looks_seek(g, body_target);
            int retail_write_like = direct_arm_bl_to(body_pc, body_bl, GP32_RAM_BASE + 0x00109964u);
            int retail_read_like = direct_file_helper_looks_read(g, body_target);
            if (retail_seek_like) {
                g->direct_hle_file_seek_addr = a;
                s3c2400_write32(g->soc, a, 0xef070005u);
            } else if (!retail_write_like && (retail_read_like || body_target == 0u || g->direct_hle_file_read_addr[0] == body_target || g->direct_hle_file_read_addr[1] == body_target)) {
                if (!g->direct_hle_file_read_addr[0]) {
                    g->direct_hle_file_read_addr[0] = a;
                } else if (g->direct_hle_file_read_addr[0] != a && !g->direct_hle_file_read_addr[1]) {
                    g->direct_hle_file_read_addr[1] = a;
                }
                s3c2400_write32(g->soc, a, 0xef070002u);
            }
        }
        /* Retail BIOS file layer.  Commercial GXE/GXC titles such as Princess Maker 2 often call the lower
           firmware file entry points directly after the BIOS has installed the card/FAT service table.  Direct
           BIOSless SMC loading has already extracted the FAT files into direct_fpk_assets, so patch these SDK
           entry points to the same asset-backed HLE used by the higher GPSDK wrappers. */
        if (w0 == 0xe92d4ff0u && w1 == 0xe24dd060u && w2 == 0xe1a05001u && w3 == 0xe1a06002u && w4 == 0xe1a07000u) {
            g->direct_hle_file_open_addr = a;
            s3c2400_write32(g->soc, a, 0xef070001u);
        }
        if (w0 == 0xe92d4fffu && w1 == 0xe24dd00cu && w2 == 0xe1a05001u && w3 == 0xe3a0107fu && w4 == 0xe0016c40u) {
            if (!g->direct_hle_file_read_addr[0]) {
                g->direct_hle_file_read_addr[0] = a;
            } else if (g->direct_hle_file_read_addr[0] != a && !g->direct_hle_file_read_addr[1]) {
                g->direct_hle_file_read_addr[1] = a;
            }
            s3c2400_write32(g->soc, a, 0xef070002u);
        }
        if (w0 == 0xe92d41f0u && w1 == 0xe24dd040u && w2 == 0xe3a0107fu && w3 == 0xe0014c40u) {
            g->direct_hle_file_close_addr = a;
            s3c2400_write32(g->soc, a, 0xef070003u);
        }
        if (w0 == 0xe92d40f0u && w1 == 0xe24dd048u && w2 == 0xe1a05001u && w3 == 0xe1a06000u) {
            g->direct_hle_file_size_addr = a;
            s3c2400_write32(g->soc, a, 0xef070004u);
        }
        if (w0 == 0xe92d4010u && w1 == 0xe1a04000u) {
            uint32_t bl = s3c2400_debug_read32(g->soc, a + 0x08u);
            uint32_t after_bl = s3c2400_debug_read32(g->soc, a + 0x0cu);
            uint32_t cmp = s3c2400_debug_read32(g->soc, a + 0x10u);
            uint32_t err2 = s3c2400_debug_read32(g->soc, a + 0x18u);
            int close_like = (after_bl & 0xffff0000u) == 0xe51f0000u && cmp == 0xe3500000u && err2 == 0x03a00002u && direct_arm_bl_to(a + 0x08u, bl, GP32_RAM_BASE + 0x00000e64u);
            int legacy_close_like = s3c2400_debug_read32(g->soc, a + 0x40u) == 0xebffe409u;
            int retail_close_like = (w0 == 0xe92d4010u && w1 == 0xe1a04000u && (w2 & 0x0f000000u) == 0x0b000000u &&
                                     s3c2400_debug_read32(g->soc, a + 0x38u) == 0xe1a00004u &&
                                     (s3c2400_debug_read32(g->soc, a + 0x3cu) & 0x0f000000u) == 0x0b000000u);
            if ((close_like || legacy_close_like || retail_close_like) && g->direct_hle_file_close_addr != a) {
                if (!g->direct_hle_file_close_addr) g->direct_hle_file_close_addr = a;
                s3c2400_write32(g->soc, a, 0xef070003u);
            }
        }
        if (w0 == 0xe92d4010u && w1 == 0xe1a04001u && s3c2400_debug_read32(g->soc, a + 0x54u) == 0xebffe483u) {
            if (!g->direct_hle_file_size_addr) g->direct_hle_file_size_addr = a;
            s3c2400_write32(g->soc, a, 0xef070004u);
        }
        if (w0 == 0xe92d4010u && w1 == 0xe1a04001u && (w2 & 0x0f000000u) == 0x0b000000u &&
            s3c2400_debug_read32(g->soc, a + 0x48u) == 0xe1a01004u && (s3c2400_debug_read32(g->soc, a + 0x50u) & 0x0f000000u) == 0x0b000000u) {
            if (!g->direct_hle_file_size_addr) g->direct_hle_file_size_addr = a;
            s3c2400_write32(g->soc, a, 0xef070004u);
        }
        if (w0 == 0xe92d4010u && w1 == 0xe1a04001u && (w2 & 0x0f000000u) == 0x0b000000u &&
            s3c2400_debug_read32(g->soc, a + 0x40u) == 0xe1a01004u &&
            direct_arm_bl_to(a + 0x50u, s3c2400_debug_read32(g->soc, a + 0x50u), GP32_RAM_BASE + 0x0010a150u)) {
            g->direct_hle_file_size_addr = a;
            s3c2400_write32(g->soc, a, 0xef070004u);
        }
        if (w0 == 0xe92d40f0u && w1 == 0xe51f6b14u && w2 == 0xe3a07000u && w3 == 0xe5867040u) {
            s3c2400_write32(g->soc, a, 0xef070008u);
        }
        /* GPSDK PCM library.  BIOSless direct loading has no firmware IIS/PCM
           service behind the library's SWI wrappers, so patch the stable GPSDK
           PCM entry points and stream guest RAM directly into the emulator's
           audio sink.  This stays at the SDK ABI level instead of keying on a
           title-specific address. */
        if (!g->direct_hle_pcm_env_addr && w0 == 0xe59f302cu && w1 == 0xe593c000u && w2 == 0xe35c0000u && w3 == 0x03a00002u) {
            uint32_t lit = a + 8u + (w0 & 0xfffu);
            g->direct_hle_pcm_env_addr = direct_ram_range(g, lit, 4u) ? s3c2400_debug_read32(g->soc, lit) : 0u;
            s3c2400_write32(g->soc, a, 0xef070009u);
        }
        if (!g->direct_hle_pcm_play_addr && w0 == 0xe92d4ff0u && w1 == 0xe1a04000u && w2 == 0xe51f00d8u &&
            w3 == 0xe1a06001u && w4 == 0xe5900000u && s3c2400_debug_read32(g->soc, a + 0x14u) == 0xe1a05002u &&
            s3c2400_debug_read32(g->soc, a + 0x18u) == 0xe3500000u && s3c2400_debug_read32(g->soc, a + 0x1cu) == 0x03a00002u) {
            g->direct_hle_pcm_play_addr = a;
            s3c2400_write32(g->soc, a, 0xef07000au);
        }
        if (!g->direct_hle_pcm_init_addr && (w0 == 0xe92d43f0u || w0 == 0xe92d41f0u) &&
            w1 == 0xe24dd01cu && w2 == 0xe1a06000u && w3 == 0xe1a05001u &&
            w4 == 0xe3a07000u &&
            ((w0 == 0xe92d43f0u && s3c2400_debug_read32(g->soc, a + 0x24u) == 0xe3a00a01u) ||
             (w0 == 0xe92d41f0u && s3c2400_debug_read32(g->soc, a + 0x18u) == 0xe3a00a01u))) {
            /*
             * GP32 SDK PCM init has two register-save variants in commercial
             * stripped/self-loader GXBs.  Older HLE matched only the r4-r9/lr
             * prologue (E92D43F0).  Little Wizard, Rally Pop, Tanggle's Magic
             * Square, and Dooly use the r4-r8/lr variant (E92D41F0), whose
             * unpatched body falls through a partly packed jump-table path and
             * can jump into non-code before the actual game loop starts.  Treat
             * both as the same SDK ABI and service them through the PCM HLE.
             */
            g->direct_hle_pcm_init_addr = a;
            s3c2400_write32(g->soc, a, 0xef07000bu);
        }
        if (!g->direct_hle_pcm_stop_addr && w0 == 0xe92d41f0u && w1 == 0xe24dd01cu && w3 == 0xe51f8398u) {
            g->direct_hle_pcm_stop_addr = a;
            s3c2400_write32(g->soc, a, 0xef07000cu);
        }
        if (!g->direct_hle_pcm_remove_addr && w0 == 0xe3500000u && w1 == 0x01a0f00eu && w2 == 0xe92d4010u && w3 == 0xe1a04000u &&
            s3c2400_debug_read32(g->soc, a + 0x10u) == 0xebfbdf9du) {
            g->direct_hle_pcm_remove_addr = a;
            s3c2400_write32(g->soc, a, 0xef07000du);
        }
        if (!g->direct_hle_pcm_lock_addr && w0 == 0xe52de004u && w1 == 0xe51fc2a0u && w2 == 0xe3a03000u &&
            w3 == 0xe083e103u && w4 == 0xe79ce10eu) {
            g->direct_hle_pcm_lock_addr = a;
            s3c2400_write32(g->soc, a, 0xef07000eu);
        }
        if (!g->direct_hle_pcm_only_kill_addr && w0 == 0xe92d4070u && w1 == 0xe51f62ecu && w2 == 0xe1a04000u && w3 == 0xe3a05000u) {
            g->direct_hle_pcm_only_kill_addr = a;
            s3c2400_write32(g->soc, a, 0xef07000fu);
        }
        if (!g->direct_hle_sound_dispatch_addr && w0 == 0xe92d4030u && w1 == 0xe24dd040u && w2 == 0xe1a05000u && w3 == 0xe1a04001u && w4 == 0xe59f1054u &&
            s3c2400_debug_read32(g->soc, a + 0x14u) == 0xe1a0000du && s3c2400_debug_read32(g->soc, a + 0x18u) == 0xe3a02040u) {
            uint32_t literal = a + 0x10u + 8u + (w4 & 0xfffu);
            g->direct_hle_sound_dispatch_addr = a;
            if (direct_ram_range(g, literal, 4u)) g->direct_hle_sound_table_addr = s3c2400_debug_read32(g->soc, literal);
            s3c2400_write32(g->soc, a, 0xef070006u);
        }
        if (!g->direct_hle_sound_play_addr && w0 == 0xe92d40f8u && w1 == 0xe51f6018u && w2 == 0xe1a04000u && w3 == 0xe5960030u && w4 == 0xe1a05001u) {
            uint32_t state_lit = a + 12u - 0x18u;
            g->direct_hle_sound_play_addr = a;
            if (direct_ram_range(g, state_lit, 4u)) g->direct_hle_sound_state_addr = s3c2400_debug_read32(g->soc, state_lit);
            /* Keep this as a fallback for titles that call the GPSDK PCM path directly
               instead of going through the small sample-index dispatcher above. */
            if (!g->direct_hle_sound_dispatch_addr) s3c2400_write32(g->soc, a, 0xef070007u);
        }
    }
}



static uint32_t direct_fpk_handle_seek(gp32_t *g, uint32_t h, uint32_t seek_mode, int32_t offset, uint32_t old_offset_addr) {
    size_t *posp = NULL;
    const fpk_asset_t *a = direct_handle_asset(g, h, &posp);
    if (!a || !posp) return 0x0bu;
    size_t old_pos = *posp;
    if (old_offset_addr) direct_write32_if_ram(g, old_offset_addr, (uint32_t)old_pos);
    int64_t base = 0;
    if (seek_mode == 0u) base = (int64_t)old_pos;       /* FROM_CURRENT */
    else if (seek_mode == 1u) base = 0;                 /* FROM_BEGIN */
    else if (seek_mode == 2u) base = (int64_t)a->size;  /* FROM_END */
    else return 0x26u;
    int64_t np = base + (int64_t)offset;
    if (np < 0) np = 0;
    if ((uint64_t)np > (uint64_t)a->size) np = (int64_t)a->size;
    *posp = (size_t)np;
    return 0u;
}


static void direct_write16_if_ram(gp32_t *g, uint32_t addr, uint16_t value) {
    direct_write8_if_ram(g, addr + 0u, (uint8_t)(value & 0xffu));
    direct_write8_if_ram(g, addr + 1u, (uint8_t)(value >> 8));
}

static int direct_asset_basename_83(const char *path, char name[11]) {
    if (!path || !name) return 0;
    const char *base = strrchr(path, '/');
    const char *base2 = strrchr(path, '\\');
    if (base2 && (!base || base2 > base)) base = base2;
    base = base ? base + 1 : path;
    memset(name, ' ', 11u);
    size_t n = 0;
    while (base[n] && base[n] != '.' && n < 8u) {
        unsigned char c = (unsigned char)base[n];
        if (c <= 0x20u || c == '/' || c == '\\') return 0;
        name[n] = (char)toupper(c);
        ++n;
    }
    if (base[n] && base[n] != '.') return 0;
    if (base[n] == '.') {
        ++n;
        size_t e = 0;
        while (base[n] && e < 3u) {
            unsigned char c = (unsigned char)base[n++];
            if (c <= 0x20u || c == '/' || c == '\\' || c == '.') return 0;
            name[8u + e++] = (char)toupper(c);
        }
        if (base[n]) return 0;
    }
    return name[0] != ' ';
}

static int direct_asset_path_has_prefix_file(const fpk_asset_t *a, const char *prefix) {
    if (!a || !prefix || !prefix[0]) return 0;
    char p[320];
    direct_norm_path(a->path, p, sizeof(p));
    size_t lp = strlen(prefix);
    if (strncmp(p, prefix, lp) != 0) return 0;
    const char *tail = p + lp;
    return tail[0] != '\0' && strchr(tail, '/') == NULL;
}

static uint32_t direct_arm_ldr_pc_literal_value(gp32_t *g, uint32_t insn_addr) {
    if (!g || !direct_ram_range(g, insn_addr, 4u)) return 0u;
    uint32_t insn = s3c2400_debug_read32(g->soc, insn_addr);
    if ((insn & 0x0e100000u) != 0x04100000u) return 0u;
    if (((insn >> 16) & 0x0fu) != 15u) return 0u;
    uint32_t imm = insn & 0xfffu;
    uint32_t addr = insn_addr + 8u;
    if (insn & 0x00800000u) addr += imm;
    else addr -= imm;
    if (!direct_ram_range(g, addr, 4u)) return 0u;
    return s3c2400_debug_read32(g->soc, addr);
}

static void direct_write_fat_entry(gp32_t *g, uint32_t addr, const fpk_asset_t *a, uint16_t fallback_cluster) {
    char name[11];
    if (!direct_asset_basename_83(a ? a->path : NULL, name)) return;
    for (uint32_t i = 0; i < 32u; ++i) direct_write8_if_ram(g, addr + i, 0u);
    for (uint32_t i = 0; i < 11u; ++i) direct_write8_if_ram(g, addr + i, (uint8_t)name[i]);
    direct_write8_if_ram(g, addr + 11u, a->attr ? a->attr : 0x20u);
    direct_write16_if_ram(g, addr + 26u, a->first_cluster ? a->first_cluster : fallback_cluster);
    direct_write32_if_ram(g, addr + 28u, (uint32_t)(a->size > 0xffffffffu ? 0xffffffffu : a->size));
}

static void direct_write_dot_entry(gp32_t *g, uint32_t addr, int parent) {
    for (uint32_t i = 0; i < 32u; ++i) direct_write8_if_ram(g, addr + i, 0u);
    direct_write8_if_ram(g, addr + 0u, '.');
    if (parent) direct_write8_if_ram(g, addr + 1u, '.');
    for (uint32_t i = parent ? 2u : 1u; i < 11u; ++i) direct_write8_if_ram(g, addr + i, ' ');
    direct_write8_if_ram(g, addr + 11u, 0x10u);
    direct_write16_if_ram(g, addr + 26u, parent ? 3u : 4u);
}

static unsigned direct_write_smc_dir_entries_limited(gp32_t *g, uint32_t addr, const char *prefix, uint16_t first_fallback_cluster, unsigned max_entries) {
    unsigned n = 0;
    if (!g || !prefix || !max_entries) return 0;
    for (size_t i = 0; i < g->direct_fpk_asset_count && n < max_entries; ++i) {
        const fpk_asset_t *a = &g->direct_fpk_assets[i];
        if (!direct_asset_path_has_prefix_file(a, prefix)) continue;
        direct_write_fat_entry(g, addr + n * 32u, a, (uint16_t)(first_fallback_cluster + n));
        ++n;
    }
    for (uint32_t i = 0; i < 32u; ++i) direct_write8_if_ram(g, addr + n * 32u + i, 0u);
    return n;
}

static unsigned direct_write_smc_dir_entries(gp32_t *g, uint32_t addr, const char *prefix, uint16_t first_fallback_cluster) {
    return direct_write_smc_dir_entries_limited(g, addr, prefix, first_fallback_cluster, 64u);
}

static unsigned direct_count_assets_in_prefix(gp32_t *g, const char *prefix) {
    unsigned n = 0;
    if (!g || !prefix || !prefix[0]) return 0;
    for (size_t i = 0; i < g->direct_fpk_asset_count; ++i) {
        if (direct_asset_path_has_prefix_file(&g->direct_fpk_assets[i], prefix)) ++n;
    }
    return n;
}

static void direct_smc_asset_prefixes(gp32_t *g, char *dat_prefix, size_t dat_len, char *game_prefix, size_t game_len) {
    if (dat_prefix && dat_len) dat_prefix[0] = '\0';
    if (game_prefix && game_len) game_prefix[0] = '\0';
    if (!g) return;

    char exe[320];
    const char *src = g->direct_smc_executable_path[0] ? g->direct_smc_executable_path : g->direct_smc_game_dir;
    direct_norm_path(src, exe, sizeof(exe));

    char parent[320];
    snprintf(parent, sizeof(parent), "%s", exe);
    char *slash = strrchr(parent, '/');
    char *base = parent;
    if (slash) {
        *slash = '\0';
        base = slash + 1;
    } else {
        parent[0] = '\0';
    }

    char stem[128] = {0};
    if (base && base[0]) {
        size_t n = 0;
        while (base[n] && base[n] != '.' && n + 1u < sizeof(stem)) { stem[n] = base[n]; ++n; }
        stem[n] = '\0';
    }

    char selected[360];
    if (parent[0]) snprintf(selected, sizeof(selected), "%s/", parent);
    else selected[0] = '\0';

    if (parent[0] && stem[0]) {
        char subdir[360];
        snprintf(subdir, sizeof(subdir), "%s/%s/", parent, stem);
        if (direct_count_assets_in_prefix(g, subdir) > 0u) snprintf(selected, sizeof(selected), "%s", subdir);
    }

    if (dat_prefix && dat_len) snprintf(dat_prefix, dat_len, "%sdat/", selected);
    if (game_prefix && game_len) snprintf(game_prefix, game_len, "%s", selected);
}

static int direct_find_retail_dir_pool(gp32_t *g, uint32_t start, uint32_t end, uint32_t *pool, uint32_t *head_ptr) {
    if (pool) *pool = 0u;
    if (head_ptr) *head_ptr = 0u;
    if (!g || start >= end) return 0;
    if (start < GP32_RAM_BASE) start = GP32_RAM_BASE;
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    if (end > ram_end) end = ram_end;
    for (uint32_t a = start; a + 0x64u < end; a += 4u) {
        uint32_t w0 = s3c2400_debug_read32(g->soc, a + 0x00u);
        uint32_t w1 = s3c2400_debug_read32(g->soc, a + 0x04u);
        uint32_t w2 = s3c2400_debug_read32(g->soc, a + 0x08u);
        uint32_t w3 = s3c2400_debug_read32(g->soc, a + 0x0cu);
        uint32_t w4 = s3c2400_debug_read32(g->soc, a + 0x10u);
        if (w0 == 0xe59f1034u && w1 == 0xe3a00000u && w2 == 0xe0802300u &&
            w3 == 0xe0813182u && w4 == 0xe2833f82u) {
            uint32_t p = direct_arm_ldr_pc_literal_value(g, a + 0x00u);
            uint32_t h = direct_arm_ldr_pc_literal_value(g, a + 0x30u);
            if (direct_ram_range(g, p, 0x4000u)) {
                if (pool) *pool = p;
                if (head_ptr) *head_ptr = h;
                return 1;
            }
        }
    }
    return 0;
}

static void direct_seed_retail_asset_fat(gp32_t *g, uint32_t fat_buf, uint32_t cluster_bytes) {
    if (!g || !fat_buf || !cluster_bytes || !direct_ram_range(g, fat_buf, 4096u)) return;
    uint32_t max_cluster = 0u;
    for (size_t i = 0; i < g->direct_fpk_asset_count; ++i) {
        const fpk_asset_t *a = &g->direct_fpk_assets[i];
        if (!a->first_cluster) continue;
        uint32_t clusters = (uint32_t)((a->size + cluster_bytes - 1u) / cluster_bytes);
        if (!clusters) clusters = 1u;
        uint32_t end = (uint32_t)a->first_cluster + clusters + 1u;
        if (end > max_cluster) max_cluster = end;
    }
    if (max_cluster < 1024u) max_cluster = 1024u;
    if (max_cluster > 8192u) max_cluster = 8192u;
    uint32_t bytes = max_cluster * 2u;
    if (!direct_ram_range(g, fat_buf, bytes)) return;
    for (uint32_t i = 0; i < max_cluster; ++i) direct_write16_if_ram(g, fat_buf + i * 2u, 0xffffu);
    for (size_t i = 0; i < g->direct_fpk_asset_count; ++i) {
        const fpk_asset_t *a = &g->direct_fpk_assets[i];
        if (!a->first_cluster) continue;
        uint32_t clusters = (uint32_t)((a->size + cluster_bytes - 1u) / cluster_bytes);
        if (!clusters) clusters = 1u;
        for (uint32_t c = 0; c < clusters; ++c) {
            uint32_t cl = (uint32_t)a->first_cluster + c;
            if (cl >= max_cluster) break;
            uint16_t next = (c + 1u < clusters) ? (uint16_t)(cl + 1u) : 0xffffu;
            direct_write16_if_ram(g, fat_buf + cl * 2u, next);
        }
    }
}

static int direct_retail_smc_init_hle(gp32_t *g, uint32_t init_pc) {
    if (!g || !g->direct_smc_game_dir[0]) return 0;

    /* Commercial GXE/GXC builds do not all use the same statically linked
       BIOS SmartMedia work area.  The older HLE seed used Princess Maker 2's
       addresses, which let that title enumerate assets but left Dooly Soccer's
       relocated callback table all zeros; its loader then reached rendering
       with uninitialised asset/surface pointers.  Decode the generic SDK init
       thunk literals at the patched call site and populate the title's own
       card-ready flag and callback tables.  Keep the historical work-area seed
       below as a compatibility fallback for builds whose init thunk does not
       expose those literals. */
    direct_install_stubs(g);
    direct_install_smc_callbacks(g);
    static const uint32_t cb_offsets[] = {
        0x000u, 0x040u, 0x078u, 0x0b0u, 0x0dcu, 0x114u, 0x138u,
        0x15cu, 0x180u, 0x1a4u, 0x1c8u, 0x1ecu, 0x210u
    };
    if (init_pc && direct_ram_range(g, init_pc, 0x140u)) {
        uint32_t state_from_lit = direct_arm_ldr_pc_literal_value(g, init_pc + 4u);
        if (direct_ram_range(g, state_from_lit, 0x44u)) {
            direct_write32_if_ram(g, state_from_lit + 0x40u, 1u);
        }
        uint32_t src_table = direct_arm_ldr_pc_literal_value(g, init_pc + 0x24u);
        uint32_t dst_table = direct_arm_ldr_pc_literal_value(g, init_pc + 0x34u);
        for (uint32_t i = 0; i < (uint32_t)(sizeof(cb_offsets) / sizeof(cb_offsets[0])); ++i) {
            uint32_t v = direct_smc_cb_base_addr(g) + cb_offsets[i];
            if (direct_ram_range(g, src_table + i * 4u, 4u)) direct_write32_if_ram(g, src_table + i * 4u, v);
            if (direct_ram_range(g, dst_table + i * 4u, 4u)) direct_write32_if_ram(g, dst_table + i * 4u, v);
        }
    }

    if (init_pc && direct_ram_range(g, init_pc, 0x140u)) {
        uint32_t state_from_lit = direct_arm_ldr_pc_literal_value(g, init_pc + 4u);
        uint32_t pool = 0u, head_ptr = 0u;
        uint32_t scan_end = g->direct_fxe_image_end;
        if (scan_end < init_pc + 0x4000u) scan_end = init_pc + 0x4000u;
        if (direct_find_retail_dir_pool(g, init_pc, scan_end, &pool, &head_ptr)) {
            char dat_prefix[360], game_prefix[360];
            direct_smc_asset_prefixes(g, dat_prefix, sizeof(dat_prefix), game_prefix, sizeof(game_prefix));
            unsigned n = direct_write_smc_dir_entries_limited(g, pool + 4u, dat_prefix, 5u, 16u);
            if (!n) n = direct_write_smc_dir_entries_limited(g, pool + 4u, game_prefix, 5u, 16u);
            direct_write32_if_ram(g, pool, pool + 4u + n * 32u + 4u);
            if (head_ptr && direct_ram_range(g, head_ptr, 4u)) direct_write32_if_ram(g, head_ptr, pool);
            if (direct_ram_range(g, state_from_lit, 0x58u)) {
                uint32_t fat_buf = (pool + 0x1b000u + 15u) & ~15u;
                if (!direct_ram_range(g, fat_buf, 0x4000u)) fat_buf = (pool + 0x8000u + 15u) & ~15u;
                direct_write32_if_ram(g, state_from_lit + 0x04u, fat_buf);
                direct_write32_if_ram(g, state_from_lit + 0x08u, 42u);
                direct_write32_if_ram(g, state_from_lit + 0x0cu, 3u);
                direct_write32_if_ram(g, state_from_lit + 0x10u, 48u);
                direct_write32_if_ram(g, state_from_lit + 0x14u, 16u);
                direct_write32_if_ram(g, state_from_lit + 0x18u, 64u);
                direct_write32_if_ram(g, state_from_lit + 0x1cu, 1000u);
                direct_write32_if_ram(g, state_from_lit + 0x24u, 10u);
                direct_write32_if_ram(g, state_from_lit + 0x2cu, 512u);
                direct_write32_if_ram(g, state_from_lit + 0x44u, pool);
                direct_write32_if_ram(g, state_from_lit + 0x48u, 1u);
                direct_write32_if_ram(g, state_from_lit + 0x4cu, 10u);
                direct_write32_if_ram(g, state_from_lit + 0x50u, 4u);
                direct_write32_if_ram(g, state_from_lit + 0x54u, 512u);
                direct_seed_retail_asset_fat(g, fat_buf, 0x4000u);
            }
        }
    }

    const uint32_t state = 0x0c12b0acu;
    const uint32_t geom = 0x0c12b0f4u;
    const uint32_t dir_meta = 0x0c131ed0u;
    const uint32_t dir_list = 0x0c131f20u;

    direct_write32_if_ram(g, state + 0x40u, 1u);
    direct_write32_if_ram(g, geom + 0u, 1u);
    direct_write32_if_ram(g, geom + 4u, 10u);
    direct_write32_if_ram(g, geom + 8u, 4u);
    direct_write32_if_ram(g, geom + 12u, 512u);

    {
        static const uint32_t cb_offsets[] = {
            0x000u, 0x040u, 0x078u, 0x0b0u, 0x0dcu, 0x114u, 0x138u,
            0x15cu, 0x180u, 0x1a4u, 0x1c8u, 0x1ecu, 0x210u
        };
        const uint32_t src_table = 0x0c131d98u;
        const uint32_t dst_table = 0x0c12b14cu;
        direct_install_stubs(g);
        direct_install_smc_callbacks(g);
        for (uint32_t i = 0; i < 16u; ++i) {
            uint32_t v = direct_ret_stub_addr(g);
            if (i < (uint32_t)(sizeof(cb_offsets) / sizeof(cb_offsets[0]))) v = direct_smc_cb_base_addr(g) + cb_offsets[i];
            if (i == 13u) v = 0x80u;
            if (i == 14u) v = 0u;
            if (i == 15u) v = 2u;
            direct_write32_if_ram(g, src_table + i * 4u, v);
            direct_write32_if_ram(g, dst_table + i * 4u, v);
        }
    }

    char dat_prefix[360];
    char game_prefix[360];
    direct_smc_asset_prefixes(g, dat_prefix, sizeof(dat_prefix), game_prefix, sizeof(game_prefix));

    direct_zero_if_ram(g, dir_meta, 0x280u);
    direct_write8_if_ram(g, dir_meta + 0u, 'g');
    direct_write8_if_ram(g, dir_meta + 1u, 'p');
    direct_write32_if_ram(g, dir_meta + 0x0cu, dir_meta + 0x214u);
    direct_write_dot_entry(g, dir_meta + 0x10u, 0);
    direct_write_dot_entry(g, dir_meta + 0x30u, 1);

    unsigned n = direct_write_smc_dir_entries(g, dir_list, dat_prefix, 5u);
    if (!n) n = direct_write_smc_dir_entries(g, dir_list, game_prefix, 5u);
    for (uint32_t i = 0; i < (n + 1u) * 32u; ++i) {
        uint8_t v = 0u;
        uint32_t ba = (dir_list + i) & ~3u;
        uint32_t bw = s3c2400_debug_read32(g->soc, ba);
        v = (uint8_t)(bw >> (((dir_list + i) & 3u) * 8u));
        direct_write8_if_ram(g, dir_meta + 0x50u + i, v);
    }
    return 1;
}

static int direct_file_hle_swi(gp32_t *g, arm920t_t *cpu, uint32_t id, uint32_t pc) {
    if (!g || !cpu) return 0;
    uint32_t lr = arm920t_get_reg(cpu, 14);
    if (id == 0x20u) {
        g->direct_hle_callback_returned = 1u;
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (!g->direct_fpk_asset_count) return 0;
    if (id == 1u) {
        const fpk_asset_t *a = direct_fpk_asset_from_cpu_path(g, arm920t_get_reg(cpu, 0));
        direct_trace_file_path(g, "open/swi", arm920t_get_reg(cpu, 0), a);
        if (!a) { arm920t_set_reg(cpu, 0, 0x24u); arm920t_set_reg(cpu, 15, lr); return 1; }
        uint32_t h = direct_alloc_fpk_handle(g, a);
        if (!h) { arm920t_set_reg(cpu, 0, 0x10u); arm920t_set_reg(cpu, 15, lr); return 1; }
        direct_write32_if_ram(g, arm920t_get_reg(cpu, 2), h);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 2u) {
        size_t *posp = NULL;
        const fpk_asset_t *a = direct_handle_asset(g, arm920t_get_reg(cpu, 0), &posp);
        if (!a || !posp) { arm920t_set_reg(cpu, 0, 0x0bu); arm920t_set_reg(cpu, 15, lr); return 1; }
        uint32_t dst = arm920t_get_reg(cpu, 1);
        uint32_t want = arm920t_get_reg(cpu, 2);
        uint32_t out_count = arm920t_get_reg(cpu, 3);
        size_t avail = (*posp < a->size) ? (a->size - *posp) : 0u;
        size_t n = want < avail ? (size_t)want : avail;
        direct_trace_file_io(g, "read/swi", arm920t_get_reg(cpu, 0), dst, want, *posp, n, a, 0u);
        if (n && direct_ram_range(g, dst, (uint32_t)n)) {
            for (size_t i = 0; i < n; ++i) s3c2400_write8(g->soc, dst + (uint32_t)i, a->data[*posp + i]);
        }
        *posp += n;
        direct_write32_if_ram(g, out_count, (uint32_t)n);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 3u) {
        uint32_t h = arm920t_get_reg(cpu, 0) & 31u;
        if (h < 32u) memset(&g->direct_fpk_handles[h], 0, sizeof(g->direct_fpk_handles[h]));
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 4u) {
        size_t *posp = NULL;
        const fpk_asset_t *a = direct_fpk_asset_from_cpu_path(g, arm920t_get_reg(cpu, 0));
        if (!a) a = direct_handle_asset(g, arm920t_get_reg(cpu, 0), &posp);
        direct_trace_file_path(g, "size/swi", arm920t_get_reg(cpu, 0), a);
        if (!a) { arm920t_set_reg(cpu, 0, 0x24u); arm920t_set_reg(cpu, 15, lr); return 1; }
        direct_note_asset_autoload(g, a);
        direct_process_asset_autoload(g);
        direct_write32_if_ram(g, arm920t_get_reg(cpu, 1), (uint32_t)a->size);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 5u) {
        size_t *dbg_posp = NULL;
        const fpk_asset_t *dbg_a = direct_handle_asset(g, arm920t_get_reg(cpu, 0), &dbg_posp);
        uint32_t st = direct_fpk_handle_seek(g, arm920t_get_reg(cpu, 0), arm920t_get_reg(cpu, 1), (int32_t)arm920t_get_reg(cpu, 2), arm920t_get_reg(cpu, 3));
        direct_trace_file_io(g, "seek/swi", arm920t_get_reg(cpu, 0), arm920t_get_reg(cpu, 1), arm920t_get_reg(cpu, 2), dbg_posp ? *dbg_posp : 0u, 0u, dbg_a, st);
        arm920t_set_reg(cpu, 0, st);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 6u) {
        uint32_t sound_id = arm920t_get_reg(cpu, 0);
        uint32_t mode = arm920t_get_reg(cpu, 1);
        if (mode == 2u) direct_stop_sef(g);
        else if (g->direct_hle_sound_table_addr && sound_id < 64u && mode == 0u) {
            uint32_t path_addr = s3c2400_debug_read32(g->soc, g->direct_hle_sound_table_addr + sound_id * 4u);
            direct_play_sef_path(g, path_addr);
        }
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 7u) {
        direct_play_sef_path(g, arm920t_get_reg(cpu, 0));
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 8u) {
        direct_retail_smc_init_hle(g, pc);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 9u) { /* GpPcmEnvGet(PCM_SR*, PCM_BIT*, int*) */
        if (!g->direct_hle_pcm_initialized) { arm920t_set_reg(cpu, 0, 2u); arm920t_set_reg(cpu, 15, lr); return 1; }
        direct_write8_if_ram(g, arm920t_get_reg(cpu, 0), (uint8_t)g->direct_hle_pcm_sr);
        direct_write8_if_ram(g, arm920t_get_reg(cpu, 1), (uint8_t)g->direct_hle_pcm_bit_count);
        direct_write32_if_ram(g, arm920t_get_reg(cpu, 2), g->direct_hle_pcm_rate);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 10u) { /* GpPcmPlay(src, size, repeatflag) */
        if (!g->direct_hle_pcm_initialized) {
            g->direct_hle_pcm_initialized = 1u;
            g->direct_hle_pcm_sr = 0u;
            g->direct_hle_pcm_bit_count = 1u;
            g->direct_hle_pcm_rate = direct_pcm_rate_from_sr(0u);
            g->direct_hle_pcm_stereo = 0u;
            g->direct_hle_pcm_bits = 16u;
        }
        uint32_t ch = 0xffffffffu;
        int ok = direct_start_pcm(g, arm920t_get_reg(cpu, 0), arm920t_get_reg(cpu, 1), arm920t_get_reg(cpu, 2), &ch);
        GP32_UNUSED(ch);
        arm920t_set_reg(cpu, 0, ok ? 0u : 1u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 11u) { /* GpPcmInit(sr, bit_count) */
        uint32_t sr = arm920t_get_reg(cpu, 0);
        uint32_t bit = arm920t_get_reg(cpu, 1);
        if (sr > 5u) sr = 0u;
        if (bit > 1u) bit = 1u;
        g->direct_hle_pcm_initialized = 1u;
        g->direct_hle_pcm_sr = sr;
        g->direct_hle_pcm_bit_count = bit;
        g->direct_hle_pcm_rate = direct_pcm_rate_from_sr(sr);
        g->direct_hle_pcm_stereo = direct_pcm_stereo_from_sr(sr);
        g->direct_hle_pcm_bits = bit ? 16u : 8u;
        g->direct_hle_pcm_accum = 0;
        direct_pcm_update_cursor(g);
        arm920t_set_reg(cpu, 0, g->direct_hle_pcm_rate);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 12u) { /* GpPcmStop() */
        direct_stop_pcm(g);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 13u) { /* GpPcmRemove(src) */
        uint32_t src = arm920t_get_reg(cpu, 0);
        direct_stop_pcm_src(g, src);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 14u) { /* GpPcmLock(src, &idx_buf, &addr_of_playing_buf) */
        uint32_t src = arm920t_get_reg(cpu, 0);
        uint32_t found = GP32_DIRECT_PCM_CHANNELS;
        for (uint32_t ch = 0; ch < GP32_DIRECT_PCM_CHANNELS; ++ch) {
            if (g->direct_hle_pcm_ch[ch].active && (!src || g->direct_hle_pcm_ch[ch].src_addr == src)) { found = ch; break; }
        }
        if (found < GP32_DIRECT_PCM_CHANNELS) {
            direct_pcm_update_cursor(g);
            direct_write32_if_ram(g, arm920t_get_reg(cpu, 1), found);
            direct_write32_if_ram(g, arm920t_get_reg(cpu, 2), direct_pcm_cursor_addr_channel(g, found));
            arm920t_set_reg(cpu, 0, 1u);
        } else {
            arm920t_set_reg(cpu, 0, 0u);
        }
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 15u) { /* GpPcmOnlyKill(src) */
        uint32_t src = arm920t_get_reg(cpu, 0);
        direct_stop_pcm_src(g, src);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    return 0;
}

static int direct_try_file_hle(gp32_t *g) {
    if (!g || !g->cpu || !g->direct_fpk_asset_count) return 0;
    uint32_t pc = arm920t_get_pc(g->cpu);
    uint32_t lr = arm920t_get_reg(g->cpu, 14);
    if (pc == g->direct_hle_file_open_addr) {
        const fpk_asset_t *a = direct_fpk_asset_from_cpu_path(g, arm920t_get_reg(g->cpu, 0));
        direct_trace_file_path(g, "open/pc", arm920t_get_reg(g->cpu, 0), a);
        if (!a) { arm920t_set_reg(g->cpu, 0, 0x24u); arm920t_set_reg(g->cpu, 15, lr); return 1; }
        uint32_t h = direct_alloc_fpk_handle(g, a);
        if (!h) { arm920t_set_reg(g->cpu, 0, 0x10u); arm920t_set_reg(g->cpu, 15, lr); return 1; }
        uint32_t out = arm920t_get_reg(g->cpu, 2);
        direct_write32_if_ram(g, out, h);
        arm920t_set_reg(g->cpu, 0, 0u);
        arm920t_set_reg(g->cpu, 15, lr);
        return 1;
    }
    if (pc == g->direct_hle_file_seek_addr) {
        size_t *dbg_posp = NULL;
        const fpk_asset_t *dbg_a = direct_handle_asset(g, arm920t_get_reg(g->cpu, 0), &dbg_posp);
        uint32_t st = direct_fpk_handle_seek(g, arm920t_get_reg(g->cpu, 0), arm920t_get_reg(g->cpu, 1), (int32_t)arm920t_get_reg(g->cpu, 2), arm920t_get_reg(g->cpu, 3));
        direct_trace_file_io(g, "seek/pc", arm920t_get_reg(g->cpu, 0), arm920t_get_reg(g->cpu, 1), arm920t_get_reg(g->cpu, 2), dbg_posp ? *dbg_posp : 0u, 0u, dbg_a, st);
        arm920t_set_reg(g->cpu, 0, st);
        arm920t_set_reg(g->cpu, 15, lr);
        return 1;
    }
    if (pc == g->direct_hle_file_size_addr) {
        const fpk_asset_t *a = direct_fpk_asset_from_cpu_path(g, arm920t_get_reg(g->cpu, 0));
        direct_trace_file_path(g, "size/pc", arm920t_get_reg(g->cpu, 0), a);
        if (!a) { arm920t_set_reg(g->cpu, 0, 0x24u); arm920t_set_reg(g->cpu, 15, lr); return 1; }
        direct_note_asset_autoload(g, a);
        direct_process_asset_autoload(g);
        direct_write32_if_ram(g, arm920t_get_reg(g->cpu, 1), (uint32_t)a->size);
        arm920t_set_reg(g->cpu, 0, 0u);
        arm920t_set_reg(g->cpu, 15, lr);
        return 1;
    }
    if (pc == g->direct_hle_file_read_addr[0] || pc == g->direct_hle_file_read_addr[1]) {
        size_t *posp = NULL;
        const fpk_asset_t *a = direct_handle_asset(g, arm920t_get_reg(g->cpu, 0), &posp);
        if (!a || !posp) { arm920t_set_reg(g->cpu, 0, 0x0bu); arm920t_set_reg(g->cpu, 15, lr); return 1; }
        uint32_t dst = arm920t_get_reg(g->cpu, 1);
        uint32_t want = arm920t_get_reg(g->cpu, 2);
        uint32_t out_count = arm920t_get_reg(g->cpu, 3);
        size_t avail = (*posp < a->size) ? (a->size - *posp) : 0u;
        size_t n = want < avail ? (size_t)want : avail;
        direct_trace_file_io(g, "read/pc", arm920t_get_reg(g->cpu, 0), dst, want, *posp, n, a, 0u);
        if (n && direct_ram_range(g, dst, (uint32_t)n)) {
            for (size_t i = 0; i < n; ++i) s3c2400_write8(g->soc, dst + (uint32_t)i, a->data[*posp + i]);
        }
        *posp += n;
        direct_write32_if_ram(g, out_count, (uint32_t)n);
        arm920t_set_reg(g->cpu, 0, 0u);
        arm920t_set_reg(g->cpu, 15, lr);
        return 1;
    }
    if (pc == g->direct_hle_file_close_addr) {
        uint32_t h = arm920t_get_reg(g->cpu, 0) & 31u;
        if (h < 32u) memset(&g->direct_fpk_handles[h], 0, sizeof(g->direct_fpk_handles[h]));
        arm920t_set_reg(g->cpu, 0, 0u);
        arm920t_set_reg(g->cpu, 15, lr);
        return 1;
    }
    return 0;
}

static int direct_restore_saved_context(gp32_t *g, uint32_t task_addr);
static void direct_tick_sdk_task_sleepers(gp32_t *g, uint32_t first_task, uint32_t last_task);
static int direct_resume_ready_sdk_task(gp32_t *g, uint32_t pc, uint32_t first_task, uint32_t last_task);

static int direct_fxe_swi(void *user, arm920t_t *cpu, uint32_t imm, uint32_t pc, int thumb) {
    GP32_UNUSED(thumb);
    gp32_t *g = (gp32_t *)user;
    if (!g || !g->direct_fxe_mode || !cpu) return 0;
    if ((imm & 0xffff00u) == 0x070000u && direct_file_hle_swi(g, cpu, imm & 0xffu, pc)) return 1;
    if (direct_trace_enabled() && (imm == 0x08u || imm == 0x11u || imm == 0x13u || imm == 0x14u || imm == 0x16u)) {
        fprintf(stderr, "[direct-hle] swi imm=%03x pc=%08x lr=%08x r0=%08x r1=%08x r2=%08x r3=%08x\n",
                imm, pc, arm920t_get_reg(cpu, 14), arm920t_get_reg(cpu, 0), arm920t_get_reg(cpu, 1), arm920t_get_reg(cpu, 2), arm920t_get_reg(cpu, 3));
    }
    switch (imm) {
    case 0x08: { /* GPSDK LCD/surface service. */
        uint32_t a0 = arm920t_get_reg(cpu, 0);
        uint32_t a1 = arm920t_get_reg(cpu, 1);
        uint32_t selector = arm920t_get_reg(cpu, 2);
        switch (selector) {
        case 0u: { /* GpGraphicModeSet(gd_bpp, gp_pal) */
            uint32_t bpp = (a0 == 16u) ? 16u : 8u;
            g->direct_fxe_bpp = bpp;
            g->direct_fxe_lcd_enabled = 1u;
            if (bpp == 16u) direct_set_lcd_16bpp(g, g->direct_fxe_fb_addr ? g->direct_fxe_fb_addr : direct_default_surface_addr(0));
            else direct_set_lcd_8bpp(g, g->direct_fxe_fb_addr ? g->direct_fxe_fb_addr : direct_default_surface_addr(0), a1);
            arm920t_set_reg(cpu, 0, (bpp == 16u) ? 2u : 4u);
            return 1;
        }
        case 1u: { /* GpLcdSurfaceGet(&surface, idx) */
            uint32_t page = a1 & 3u;
            if (direct_ram_range(g, a0, 28u)) {
                direct_fill_lcd_surface(g, a0, page);
                if (g->direct_fxe_fb_addr == 0u || page == 0u) {
                    if (g->direct_fxe_bpp == 16u) direct_set_lcd_16bpp(g, direct_surface_addr_for_bpp(g, page));
                    else direct_set_lcd_8bpp(g, direct_surface_addr_for_bpp(g, page), 0u);
                }
            }
            arm920t_set_reg(cpu, 0, 0u);
            return 1;
        }
        case 2u: /* GpFlipModeSet */
            arm920t_set_reg(cpu, 0, 0u);
            return 1;
        case 3u: /* GpLcdEnable */
            g->direct_fxe_lcd_enabled = 1u;
            if (g->direct_fxe_fb_addr) {
                if (g->direct_fxe_bpp == 16u) direct_set_lcd_16bpp(g, g->direct_fxe_fb_addr);
                else direct_set_lcd_8bpp(g, g->direct_fxe_fb_addr, 0u);
            }
            arm920t_set_reg(cpu, 0, 0u);
            return 1;
        case 4u: /* GpLcdDisable */
            g->direct_fxe_lcd_enabled = 0u;
            s3c2400_write32(g->soc, 0x14a00000u, s3c2400_debug_read32(g->soc, 0x14a00000u) & ~1u);
            arm920t_set_reg(cpu, 0, 0u);
            return 1;
        case 5u: /* GpLcdStatusGet */
            arm920t_set_reg(cpu, 0, g->direct_fxe_lcd_enabled ? (0x80u | 0x40u | 0x20u) : 0u);
            return 1;
        case 6u: /* GpLcdLock */
            arm920t_set_reg(cpu, 0, g->direct_fxe_fb_addr ? g->direct_fxe_fb_addr : direct_default_surface_addr(0));
            return 1;
        case 7u: { /* GpLcdInfoGet(GPLCDINFO *p_info) */
            if (direct_ram_range(g, a0, 36u)) {
                uint32_t bpp = (g->direct_fxe_bpp == 16u) ? 16u : 8u;
                uint32_t buf_count = (bpp == 16u) ? 2u : 4u;
                uint32_t lcd_global = (0u << 0) | (buf_count << 8) | (bpp << 16) | ((g->direct_fxe_lcd_enabled ? (0x80u | 0x40u | 0x20u) : 0u) << 24);
                direct_write32_if_ram(g, a0 + 0u, lcd_global);
                direct_write32_if_ram(g, a0 + 4u, 240u * 320u * (bpp == 16u ? 2u : 1u));
                for (uint32_t i = 0; i < 4u; ++i) direct_write32_if_ram(g, a0 + 8u + i * 4u, direct_default_surface_addr(i));
                direct_write32_if_ram(g, a0 + 24u, 0x14a00400u);
                direct_write32_if_ram(g, a0 + 28u, g->direct_fxe_palette_addr);
            }
            arm920t_set_reg(cpu, 0, 0u);
            return 1;
        }
        default:
            arm920t_set_reg(cpu, 0, 0u);
            return 1;
        }
    }
    case 0x0e: /* GPSDK sound-buffer allocator used by GpPcmInit. */
        return direct_handle_swi_set_sndbuffer(g, cpu);
    case 0x13: /* GPSDK/GPOS timer and scheduler command-block service. */
        return direct_handle_swi_gpos_timer(g, cpu, pc);
    case 0x11: { /* Direct-mode display callback for GpSurfaceSet/GpSurfaceFlip. */
        uint32_t a0 = arm920t_get_reg(cpu, 0);
        uint32_t fb = direct_read_surface_buffer(g, a0);
        if (fb) {
            uint32_t bpp = s3c2400_debug_read32(g->soc, a0 + 4u);
            if (bpp == 16u) direct_set_lcd_16bpp(g, fb);
            else direct_set_lcd_8bpp(g, fb, 0u);
            g->direct_fxe_lcd_explicit = 1u;
            direct_request_vblank_wait(g);
        }
        arm920t_set_reg(cpu, 0, 1u);
        return 1;
    }
    case 0x14: { /* GPSDK thread sleep on scheduler builds; IIS/audio on PCM builds. */
        uint32_t cmdp = arm920t_get_reg(cpu, 0);
        uint32_t cmd = direct_read32_if_ram(g, cmdp);
        if (direct_ram_range(g, cmdp, 4u) &&
            (cmd == 0x20u || cmd == 0x80u || cmd == 0x1000u || cmd == 0x2000u || cmd == 0x4010u)) {
            direct_tick_sdk_task_sleepers(g, arm920t_get_reg(cpu, 2), arm920t_get_reg(cpu, 3));
            if (direct_resume_ready_sdk_task(g, pc, arm920t_get_reg(cpu, 2), arm920t_get_reg(cpu, 3))) return 1;
        }
        return direct_handle_swi_iis(g, cpu);
    }
    case 0x0b: { /* Firmware memory/control helper used by GP32 SDK CRTs. */
        uint32_t selector = arm920t_get_reg(cpu, 0);
        uint32_t a1 = arm920t_get_reg(cpu, 1);
        uint32_t a2 = arm920t_get_reg(cpu, 2);
        uint32_t value = 0u;
        direct_sync_fwinfo(g);
        switch (selector) {
        case 0u:
            direct_install_stubs(g);
            value = direct_stub_addr(g);
            break;
        case 2u:
            /* Palette address query used by several GPSDK palette wrappers.
               The first output is the hardware/register palette and the second
               is the firmware logical/work palette.  Older HLE returned zero
               here, which made GpPaletteSelect copy a selected palette to NULL
               and left later GpPaletteRealize calls displaying the default or
               corrupted palette. */
            if (!g->direct_fxe_palette_initialized) direct_fill_default_palette(g);
            direct_write_fw_arg(g, a1, 0x14a00400u);
            direct_write_fw_arg(g, a2, direct_palette_sw_addr(g));
            g->direct_fxe_palette_addr = direct_palette_sw_addr(g);
            value = 0u;
            break;
        case 3u:
            /* BIOS callback-vector table request.  Several GPSDK builds ask
               firmware to populate a table of service callbacks, copy it into
               BSS, then call through selected entries.  In direct FXE mode the
               services are represented by a safe return-only ARM stub. */
            if (direct_ram_range(g, a1, 16u * 4u)) {
                static const uint32_t cb_offsets[] = { 0x000u, 0x040u, 0x078u, 0x0b0u, 0x0dcu, 0x114u, 0x138u, 0x15cu, 0x180u, 0x1a4u, 0x1c8u, 0x1ecu, 0x210u };
                direct_install_stubs(g);
                direct_install_smc_callbacks(g);
                for (uint32_t i = 0; i < 16u; ++i) {
                    uint32_t v = direct_ret_stub_addr(g);
                    if (i < (uint32_t)(sizeof(cb_offsets) / sizeof(cb_offsets[0]))) v = direct_smc_cb_base_addr(g) + cb_offsets[i];
                    direct_write32_if_ram(g, a1 + i * 4u, v);
                }
            }
            value = a1;
            break;
        case 4u:
            value = direct_fwinfo_addr(g);
            break;
        case 5u:
            value = g->direct_fxe_stack;
            break;
        case 6u:
            direct_update_fw_tick(g);
            value = direct_fw_tick_addr(g);
            break;
        case 8u:
            value = 0u;
            break;
        default:
            value = 0u;
            break;
        }
        arm920t_set_reg(cpu, 0, value);
        return 1;
    }
    case 0x0f: { /* b2fxec/commercial CRT inherited app-argument query. */
        uint32_t selector = arm920t_get_reg(cpu, 4);
        uint32_t argp = arm920t_get_reg(cpu, 0);
        if ((selector == 1u || selector == 0u) && g->direct_smc_game_dir[0]) {
            uint32_t dst = direct_app_arg_addr(g);
            direct_write_cstr_if_ram(g, dst, g->direct_smc_game_dir);
            arm920t_set_reg(cpu, 0, dst);
            return 1;
        }
        if ((selector == 2u || selector == 3u) && g->direct_smc_executable_path[0]) {
            uint32_t dst = direct_app_arg_addr(g) + 0x100u;
            direct_write_cstr_if_ram(g, dst, g->direct_smc_executable_path);
            arm920t_set_reg(cpu, 0, dst);
            return 1;
        }
        if (argp >= GP32_RAM_BASE && argp + 4u < GP32_RAM_BASE + s3c2400_ram_size(g->soc)) {
            s3c2400_write32(g->soc, argp, 0u);
            arm920t_set_reg(cpu, 0, argp);
        } else {
            arm920t_set_reg(cpu, 0, 0u);
        }
        return 1;
    }
    case 0x10: { /* GP32 SDK key helper: selector 0 returns GPIO key ports; selector 1 maps active-low GPIO bits to SDK key bits. */
        uint32_t selector = arm920t_get_reg(cpu, 0);
        if (selector == 0u) {
            arm920t_set_reg(cpu, 0, 0x1560000cu);
            arm920t_set_reg(cpu, 1, 0x15600030u);
            return 1;
        }
        if (selector == 1u) {
            uint32_t in0 = direct_key_port_value(g, arm920t_get_reg(cpu, 1));
            uint32_t in1 = direct_key_port_value(g, arm920t_get_reg(cpu, 2));
            /*
             * This is the low-level firmware virtual-key mapper used by the
             * GPSDK _VirtualKeyMap routine, not the public GpKeyGet() ABI.
             * The SDK routine calls SWI #0x10 and then MVN's R0 before
             * returning to the game, so the firmware value must remain
             * active-low: 1 bits mean released, 0 bits mean pressed.
             * Returning active-high here made GpKeyGet() become ~keys, which
             * looked like every control was held; AKA NOID then drifted left
             * and ran its menus/gameplay controls erratically.
             */
            uint32_t keys = 0u;
            if ((in0 & 0x0100u) == 0u) keys |= 0x001u; /* GPC_VK_LEFT */
            if ((in0 & 0x0200u) == 0u) keys |= 0x002u; /* GPC_VK_DOWN */
            if ((in0 & 0x0400u) == 0u) keys |= 0x004u; /* GPC_VK_RIGHT */
            if ((in0 & 0x0800u) == 0u) keys |= 0x008u; /* GPC_VK_UP */
            if ((in0 & 0x1000u) == 0u) keys |= 0x010u; /* GPC_VK_FL */
            if ((in0 & 0x2000u) == 0u) keys |= 0x020u; /* GPC_VK_FB */
            if ((in0 & 0x4000u) == 0u) keys |= 0x040u; /* GPC_VK_FA */
            if ((in0 & 0x8000u) == 0u) keys |= 0x080u; /* GPC_VK_FR */
            if ((in1 & 0x0040u) == 0u) keys |= 0x100u; /* GPC_VK_START */
            if ((in1 & 0x0080u) == 0u) keys |= 0x200u; /* GPC_VK_SELECT */
            arm920t_set_reg(cpu, 0, ~keys);
            return 1;
        }
        arm920t_set_reg(cpu, 0, 0xffffffffu);
        return 1;
    }
    case 0x15: /* Application argument query. */
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 1, 0u);
        return 1;
    case 0x12: /* Firmware exit.  Keep direct-loaded homebrew in a benign idle loop. */
        arm920t_set_reg(cpu, 15, pc);
        return 1;
    case 0x16: { /* GPSDK graphics mode/palette helper. */
        uint32_t selector = arm920t_get_reg(cpu, 0);
        uint32_t arg1 = arm920t_get_reg(cpu, 1);
        uint32_t arg2 = arm920t_get_reg(cpu, 2);
        uint32_t arg3 = arm920t_get_reg(cpu, 3);
        switch (selector) {
        case 1u: /* palette realize / vblank-safe update */
        case 2u: /* fast palette update */
            direct_realize_software_palette_from(g, arg2, 1);
            arm920t_set_reg(cpu, 0, 1u);
            return 1;
        case 3u: /* palette operation accepted; keep current addresses stable. */
        case 4u:
            arm920t_set_reg(cpu, 0, 1u);
            return 1;
        case 5u: { /* swi_pal_addr_get(unsigned int **reg, unsigned int **log) */
            uint32_t sw = direct_palette_sw_addr(g);
            uint32_t hw = direct_palette_hw_mirror_addr(g);
            if (!g->direct_fxe_palette_initialized) direct_fill_default_palette(g);
            if (direct_ram_range(g, arg1, 4u)) direct_write32_if_ram(g, arg1, 0x14a00400u);
            if (direct_ram_range(g, arg2, 4u)) direct_write32_if_ram(g, arg2, sw);
            direct_copy_lcd_palette_to_ram(g, hw);
            g->direct_fxe_palette_addr = sw;
            arm920t_set_reg(cpu, 0, 1u);
            return 1;
        }
        case 6u:
            direct_set_lcd_8bpp(g, g->direct_fxe_fb_addr ? g->direct_fxe_fb_addr : direct_default_surface_addr(0), arg1);
            arm920t_set_reg(cpu, 0, 1u);
            return 1;
        default:
            GP32_UNUSED(arg3);
            arm920t_set_reg(cpu, 0, 1u);
            return 1;
        }
    }
    case 0x05: { /* Execute a decrunched GXB image when a packer passes one; otherwise firmware display service no-op. */
        uint32_t target = arm920t_get_reg(cpu, 0);
        uint32_t stack = arm920t_get_reg(cpu, 1);
        if (direct_ram_range(g, target, 8u)) {
            uint32_t w0 = s3c2400_debug_read32(g->soc, target);
            uint32_t w1 = s3c2400_debug_read32(g->soc, target + 4u);
            int w0_branch = (w0 & 0x0f000000u) == 0x0a000000u;
            int w1_branch = (w1 & 0x0f000000u) == 0x0a000000u;
            if (w0_branch || w1_branch) {
                if (!w0_branch) target += 4u;
                if (direct_ram_range(g, stack, 4u)) arm920t_set_reg(cpu, 13, stack);
                arm920t_set_reg(cpu, 15, target);
                return 1;
            }
        }
        arm920t_set_reg(cpu, 0, 0u);
        return 1;
    }
    default:
        if (imm == 0x02u || imm == 0x04u || imm == 0x07u || imm == 0x09u || imm == 0x0au ||
            imm == 0x0du || imm == 0x0eu ||
            (imm >= 0x100u && imm <= 0x120u)) {
            arm920t_set_reg(cpu, 0, 0u);
            return 1;
        }
        return 0;
    }
}

gp32_t *gp32_create(const gp32_options_t *opt) {
    gp32_t *g = (gp32_t *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    if (opt) { g->log = opt->log; g->log_user = opt->log_user; }
    g->soc = s3c2400_create(opt ? opt->ram_size : 0);
    if (!g->soc) { gp32_destroy(g); return NULL; }
    s3c2400_set_log(g->soc, bridge_log, g);
    arm_bus_t bus = s3c2400_get_bus(g->soc);
    g->cpu = arm920t_create(&bus);
    if (!g->cpu) { gp32_destroy(g); return NULL; }
    s3c2400_set_irq_sink(g->soc, g->cpu);
    arm920t_set_swi_handler(g->cpu, direct_fxe_swi, g);
    if (opt && opt->enable_trace) arm920t_set_trace(g->cpu, 1, bridge_log, g);
#if defined(GP32EMU_WASM)
    int direct_smc = 0;
    (void)direct_smc;
    /* WASM hosts load BIOS/media from JavaScript-provided memory buffers.
     * Avoid pulling filesystem/ZIP path loaders into the freestanding build. */
    gp32_reset(g);
#else
    int direct_smc = opt && opt->smartmedia_path && !opt->bios_path;
    if (opt && opt->bios_path && gp32_load_bios(g, opt->bios_path) != GP32_OK) { gp32_destroy(g); return NULL; }
    if (opt && opt->smartmedia_path && gp32_load_smartmedia(g, opt->smartmedia_path) != GP32_OK) { gp32_destroy(g); return NULL; }
    gp32_reset(g);
    if (direct_smc && gp32_load_smartmedia_direct(g, opt->smartmedia_path) != GP32_OK) { gp32_destroy(g); return NULL; }
#endif
    return g;
}

void gp32_destroy(gp32_t *g) {
    if (!g) return;
    direct_clear_fpk_assets(g);
    direct_clear_reset_image(g);
    arm920t_destroy(g->cpu);
    s3c2400_destroy(g->soc);
    free(g);
}

gp32_status_t gp32_load_bios(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_load_bios(g->soc, path, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "BIOS load failed"); return GP32_ERR_IO; }
    return GP32_OK;
}

gp32_status_t gp32_load_smartmedia(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_load_smartmedia(g->soc, path, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "SmartMedia load failed"); return GP32_ERR_BAD_IMAGE; }
    return GP32_OK;
}

gp32_status_t gp32_load_bios_data(gp32_t *g, const void *data, size_t size) {
    if (!g || !data || !size) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_load_bios_buffer(g->soc, (const uint8_t *)data, size, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "BIOS buffer load failed"); return GP32_ERR_IO; }
    return GP32_OK;
}

gp32_status_t gp32_load_smartmedia_data(gp32_t *g, const void *data, size_t size) {
    if (!g || !data || !size) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_load_smartmedia_buffer(g->soc, (const uint8_t *)data, size, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "SmartMedia buffer load failed"); return GP32_ERR_BAD_IMAGE; }
    return GP32_OK;
}

static gp32_status_t gp32_load_fxe_image(gp32_t *g, fxe_image_t *img);

static void direct_smc_set_launch_paths(gp32_t *g, const char *exe_path) {
    if (!g) return;
    g->direct_smc_executable_path[0] = '\0';
    g->direct_smc_game_dir[0] = '\0';
    if (!exe_path || !exe_path[0]) return;

    char tmp[260];
    size_t j = 0u;
    const char *prefix = "gp:\\";
    const char *last_sep = strrchr(exe_path, '/');
    const char *last_sep2 = strrchr(exe_path, '\\');
    if (last_sep2 && (!last_sep || last_sep2 > last_sep)) last_sep = last_sep2;

    for (size_t i = 0u; prefix[i] && j + 1u < sizeof(tmp); ++i) tmp[j++] = prefix[i];
    for (const char *p = exe_path; *p && j + 1u < sizeof(tmp); ++p) {
        char c = *p;
        if (c == '/') c = '\\';
        /* The BIOS launcher's app-argument string is not fully normalised to
         * lower-case.  It uses a lower-case path prefix (gp:\game\...) but
         * preserves the 8.3 executable basename as stored on the SMC.  Hany's
         * SDK startup explicitly strcmp()s this value against
         * "gp:\game\STAR"; returning "gp:\game\star" makes it take the
         * failure-exit path and eventually return through a zero PC in HLE.
         * Keep directories case-insensitive/lowercase for older direct-FPK
         * users, but preserve the final filename stem/extension. */
        if (!last_sep || p <= last_sep) c = (char)tolower((unsigned char)c);
        tmp[j++] = c;
    }
    tmp[j] = '\0';
    snprintf(g->direct_smc_executable_path, sizeof(g->direct_smc_executable_path), "%s", tmp);
    snprintf(g->direct_smc_game_dir, sizeof(g->direct_smc_game_dir), "%s", tmp);
    char *dot = strrchr(g->direct_smc_game_dir, '.');
    char *slash = strrchr(g->direct_smc_game_dir, '\\');
    if (dot && (!slash || dot > slash)) *dot = '\0';
    else if (slash && slash[1]) slash[1] = '\0';
}

gp32_status_t gp32_load_smartmedia_direct(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    smc_direct_package_t pkg;
    if (!smc_direct_load_file(path, &pkg, e, sizeof(e))) {
        seterr(g, "%s", e[0] ? e : "SmartMedia direct-load failed");
        return GP32_ERR_BAD_IMAGE;
    }
    direct_smc_set_launch_paths(g, pkg.executable_path);
    direct_clear_fpk_assets(g);
    direct_clear_reset_image(g);
    g->direct_fpk_assets = pkg.assets;
    g->direct_fpk_asset_count = pkg.asset_count;
    pkg.assets = NULL;
    pkg.asset_count = 0;
    gp32_status_t st = gp32_load_fxe_image_internal(g, &pkg.image, 1, 1, 1, 0);
    if (st == GP32_OK) {
        direct_init_smc_gpio(g);
        direct_scan_file_hle(g);
    }
    smc_direct_package_free(&pkg);
    return st;
}

gp32_status_t gp32_load_smartmedia_direct_data(gp32_t *g, const void *data, size_t size, const char *label) {
    if (!g || !data || !size) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    smc_direct_package_t pkg;
    if (!smc_direct_load_buffer((const uint8_t *)data, size, label ? label : "smc", &pkg, e, sizeof(e))) {
        seterr(g, "%s", e[0] ? e : "SmartMedia direct buffer load failed");
        return GP32_ERR_BAD_IMAGE;
    }
    direct_smc_set_launch_paths(g, pkg.executable_path);
    direct_clear_fpk_assets(g);
    direct_clear_reset_image(g);
    g->direct_fpk_assets = pkg.assets;
    g->direct_fpk_asset_count = pkg.asset_count;
    pkg.assets = NULL;
    pkg.asset_count = 0;
    gp32_status_t st = gp32_load_fxe_image_internal(g, &pkg.image, 1, 1, 1, 0);
    if (st == GP32_OK) {
        direct_init_smc_gpio(g);
        direct_scan_file_hle(g);
    }
    smc_direct_package_free(&pkg);
    return st;
}

static gp32_status_t gp32_load_fxe_image_internal(gp32_t *g, fxe_image_t *img, int update_reset_image, int scan_file_hle, int init_smc_gpio, int preserve_hle_options) {
    char e[256] = {0};
    direct_reset_hle_runtime(g, preserve_hle_options);
    s3c2400_reset(g->soc);
    s3c2400_install_hle_bios(g->soc);
    if (!s3c2400_load_ram_image(g->soc, img->load_addr, img->payload, img->payload_size, e, sizeof(e))) {
        seterr(g, "%s", e[0] ? e : "FXE RAM load failed");
        return GP32_ERR_BAD_IMAGE;
    }
    g->direct_fxe_mode = 1;
    g->direct_fxe_entry = img->entry_addr;
    g->direct_fxe_stack = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    g->direct_fxe_fb_addr = 0u;
    g->direct_fxe_image_end = img->load_addr + (uint32_t)img->payload_size;
    g->direct_fxe_bpp = 0u;
    g->direct_fxe_palette_addr = 0u;
    g->direct_fxe_palette_initialized = 0u;
    g->direct_fxe_lcd_explicit = 0u;
    g->direct_fxe_lcd_enabled = 1u;
    g->direct_vblank_next_cycle = 0u;
    g->direct_vblank_wait_cycles = 0u;
    g->direct_vblank_wait_requested = 0;
    direct_apply_gxb_scatterload(g, img);
    for (unsigned i = 0; i < 4u; ++i) {
        g->direct_fxe_lcd_surface[i] = direct_default_surface_addr(i);
        g->direct_fxe_surface_checksum[i] = 0u;
    }
    direct_install_stubs(g);
    direct_sync_fwinfo(g);
    direct_fill_default_palette(g);
    snprintf(g->direct_fxe_title, sizeof(g->direct_fxe_title), "%s", img->title[0] ? img->title : "FXE");
    arm920t_reset(g->cpu, img->entry_addr);
    arm920t_set_cpsr(g->cpu, ARM_MODE_SVC | ARM_I_FLAG | ARM_F_FLAG);
    arm920t_set_reg(g->cpu, 13, g->direct_fxe_stack - 16u);
    arm920t_set_reg(g->cpu, 0, img->load_addr);
    arm920t_set_reg(g->cpu, 1, g->direct_fxe_stack - 16u);
    if (update_reset_image && !direct_store_reset_image(g, img, scan_file_hle, init_smc_gpio)) {
        seterr(g, "out of memory storing direct-HLE reset image");
        return GP32_ERR_IO;
    }
    return GP32_OK;
}

static gp32_status_t gp32_load_fxe_image(gp32_t *g, fxe_image_t *img) {
    return gp32_load_fxe_image_internal(g, img, 1, 0, 0, 0);
}

gp32_status_t gp32_load_fxe(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    fxe_image_t img;
    char e[256] = {0};
    if (!fxe_load_file(path, &img, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "FXE load failed"); return GP32_ERR_BAD_IMAGE; }
    direct_clear_fpk_assets(g);
    direct_clear_reset_image(g);
    gp32_status_t st = gp32_load_fxe_image(g, &img);
    fxe_image_free(&img);
    return st;
}

gp32_status_t gp32_load_fxe_data(gp32_t *g, const void *data, size_t size, const char *label) {
    if (!g || !data || !size) return GP32_ERR_INVALID_ARGUMENT;
    fxe_image_t img;
    char e[256] = {0};
    if (!fxe_load_buffer((const uint8_t *)data, size, label ? label : "buffer", &img, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "FXE buffer load failed"); return GP32_ERR_BAD_IMAGE; }
    direct_clear_fpk_assets(g);
    direct_clear_reset_image(g);
    gp32_status_t st = gp32_load_fxe_image(g, &img);
    fxe_image_free(&img);
    return st;
}

gp32_status_t gp32_load_fpk(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    fpk_package_t pkg;
    if (!fpk_load_package_file(path, &pkg, e, sizeof(e))) {
        seterr(g, "%s", e[0] ? e : "FPK load failed");
        return GP32_ERR_BAD_IMAGE;
    }
    fxe_image_t img;
    if (!fxe_load_buffer(pkg.fxe_data, pkg.fxe_size, pkg.title[0] ? pkg.title : path, &img, e, sizeof(e))) {
        fpk_package_free(&pkg);
        seterr(g, "%s", e[0] ? e : "FPK embedded FXE load failed");
        return GP32_ERR_BAD_IMAGE;
    }
    if (!img.title[0] && pkg.title[0]) {
        size_t n = strlen(pkg.title);
        if (n >= sizeof(img.title)) n = sizeof(img.title) - 1u;
        memcpy(img.title, pkg.title, n);
        img.title[n] = '\0';
    }
    direct_clear_fpk_assets(g);
    direct_clear_reset_image(g);
    g->direct_fpk_assets = pkg.assets;
    g->direct_fpk_asset_count = pkg.asset_count;
    pkg.assets = NULL;
    pkg.asset_count = 0;
    gp32_status_t st = gp32_load_fxe_image_internal(g, &img, 1, 1, 0, 0);
    if (st == GP32_OK) direct_scan_file_hle(g);
    fxe_image_free(&img);
    fpk_package_free(&pkg);
    return st;
}

gp32_status_t gp32_load_fpk_data(gp32_t *g, const void *data, size_t size, const char *label) {
    if (!g || !data || !size) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    fpk_package_t pkg;
    if (!fpk_load_package_buffer((const uint8_t *)data, size, label ? label : "FPK", &pkg, e, sizeof(e))) {
        seterr(g, "%s", e[0] ? e : "FPK buffer load failed");
        return GP32_ERR_BAD_IMAGE;
    }
    fxe_image_t img;
    if (!fxe_load_buffer(pkg.fxe_data, pkg.fxe_size, pkg.title[0] ? pkg.title : (label ? label : "FPK"), &img, e, sizeof(e))) {
        fpk_package_free(&pkg);
        seterr(g, "%s", e[0] ? e : "FPK embedded FXE load failed");
        return GP32_ERR_BAD_IMAGE;
    }
    if (!img.title[0] && pkg.title[0]) {
        size_t n = strlen(pkg.title);
        if (n >= sizeof(img.title)) n = sizeof(img.title) - 1u;
        memcpy(img.title, pkg.title, n);
        img.title[n] = '\0';
    }
    direct_clear_fpk_assets(g);
    g->direct_fpk_assets = pkg.assets;
    g->direct_fpk_asset_count = pkg.asset_count;
    pkg.assets = NULL;
    pkg.asset_count = 0;
    gp32_status_t st = gp32_load_fxe_image_internal(g, &img, 1, 1, 0, 0);
    if (st == GP32_OK) direct_scan_file_hle(g);
    fxe_image_free(&img);
    fpk_package_free(&pkg);
    return st;
}

gp32_status_t gp32_save_smartmedia(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_save_smartmedia(g->soc, path, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "SmartMedia save failed"); return GP32_ERR_IO; }
    return GP32_OK;
}

gp32_status_t gp32_reset(gp32_t *g) {
    if (!g) return GP32_ERR_INVALID_ARGUMENT;
    if (g->direct_reset_image_valid && g->direct_reset_image.payload && g->direct_reset_image.payload_size) {
        gp32_status_t st = gp32_load_fxe_image_internal(g, &g->direct_reset_image, 0, g->direct_reset_scan_file_hle, g->direct_reset_init_smc_gpio, 1);
        if (st == GP32_OK) {
            if (g->direct_reset_init_smc_gpio) direct_init_smc_gpio(g);
            if (g->direct_reset_scan_file_hle) direct_scan_file_hle(g);
        }
        return st;
    }
    g->direct_fxe_mode = 0;
    g->direct_fxe_fb_addr = 0;
    g->direct_fxe_image_end = 0;
    g->direct_fxe_bpp = 0;
    g->direct_fxe_palette_addr = 0;
    g->direct_fxe_palette_initialized = 0;
    g->direct_fxe_lcd_explicit = 0;
    g->direct_fxe_lcd_enabled = 0;
    g->direct_vblank_next_cycle = 0u;
    g->direct_vblank_wait_cycles = 0u;
    g->direct_vblank_wait_requested = 0;
    for (unsigned i = 0; i < 4u; ++i) {
        g->direct_fxe_lcd_surface[i] = 0;
        g->direct_fxe_surface_checksum[i] = 0;
    }
    direct_reset_hle_runtime(g, 1);
    s3c2400_reset(g->soc);
    arm920t_reset(g->cpu, 0x00000000u);
    return GP32_OK;
}


static uint32_t direct_task_saved_pc(gp32_t *g, uint32_t task_addr) {
    if (!g || !direct_ram_range(g, task_addr, 0x34u)) return 0u;
    uint32_t saved_sp = s3c2400_debug_read32(g->soc, task_addr + 0u);
    if (!direct_ram_range(g, saved_sp, 64u)) return 0u;
    return s3c2400_debug_read32(g->soc, saved_sp + 60u);
}

static int direct_task_record_plausible(gp32_t *g, uint32_t task_addr) {
    if (!g || !direct_ram_range(g, task_addr, 0x34u)) return 0;
    uint32_t state = s3c2400_debug_read32(g->soc, task_addr + 0x14u);
    if (state != 1u && state != 2u && state != 4u && state != 8u) return 0;
    uint32_t entry = s3c2400_debug_read32(g->soc, task_addr + 0x30u);
    return direct_ram_range(g, entry & ~1u, 4u);
}

static int direct_task_state_can_run(gp32_t *g, uint32_t task_addr) {
    if (!direct_task_record_plausible(g, task_addr)) return 0;
    if (direct_gpos_task_is_internal_timer(g, task_addr)) return 0;
    uint32_t state = s3c2400_debug_read32(g->soc, task_addr + 0x14u);
    if (state == 1u || state == 2u) return 1;
    if (state == 4u) {
        uint32_t elapsed = s3c2400_debug_read32(g->soc, task_addr + 0x20u);
        uint32_t limit = s3c2400_debug_read32(g->soc, task_addr + 0x24u);
        return limit == 0u || elapsed >= limit;
    }
    return 0;
}

static void direct_set_task_scheduler_current(gp32_t *g, uint32_t task_addr, uint32_t idle_pc) {
    if (!g || !direct_task_record_plausible(g, task_addr)) return;
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    for (uint32_t a = GP32_RAM_BASE; a + 8u < ram_end; a += 4u) {
        uint32_t cur = s3c2400_debug_read32(g->soc, a + 0u);
        uint32_t sel = s3c2400_debug_read32(g->soc, a + 4u);
        if (cur != sel || cur == task_addr) continue;
        if (!direct_task_record_plausible(g, cur)) continue;
        uint32_t saved_pc = direct_task_saved_pc(g, cur);
        if (saved_pc && (saved_pc == idle_pc || saved_pc + 4u == idle_pc || saved_pc == idle_pc - 4u || saved_pc + 8u == idle_pc)) {
            direct_write32_if_ram(g, a + 0u, task_addr);
            direct_write32_if_ram(g, a + 4u, task_addr);
            return;
        }
    }
}

static void direct_task_scan_bounds(gp32_t *g, uint32_t first_task, uint32_t last_task, uint32_t *start, uint32_t *end, uint32_t *step) {
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    *start = GP32_RAM_BASE;
    *end = ram_end;
    *step = 4u;
    if (!g) return;
    if (direct_ram_range(g, first_task, 0x34u) && direct_ram_range(g, last_task, 0x34u) &&
        last_task >= first_task && last_task - first_task <= 32u * 0x34u) {
        *start = first_task;
        *end = last_task + 0x34u;
        *step = 0x34u;
        return;
    }
    uint32_t image_end = g->direct_fxe_image_end;
    if (image_end > GP32_RAM_BASE && image_end < ram_end) *end = image_end;
}

static void direct_tick_sdk_task_sleepers(gp32_t *g, uint32_t first_task, uint32_t last_task) {
    if (!g || !g->direct_fxe_mode) return;
    uint32_t start = GP32_RAM_BASE, end = GP32_RAM_BASE, step = 4u;
    direct_task_scan_bounds(g, first_task, last_task, &start, &end, &step);
    for (uint32_t t = start; t + 0x34u <= end; t += step) {
        if (!direct_task_record_plausible(g, t)) continue;
        if (direct_gpos_task_is_internal_timer(g, t)) continue;
        if (s3c2400_debug_read32(g->soc, t + 0x14u) != 4u) continue;
        uint32_t saved_sp = s3c2400_debug_read32(g->soc, t + 0u);
        if (!direct_ram_range(g, saved_sp, 64u)) continue;
        uint32_t saved_pc = s3c2400_debug_read32(g->soc, saved_sp + 60u);
        if (!direct_ram_range(g, saved_pc & ~1u, 4u)) continue;
        uint32_t elapsed = s3c2400_debug_read32(g->soc, t + 0x20u);
        uint32_t limit = s3c2400_debug_read32(g->soc, t + 0x24u);
        if (limit == 0u || elapsed + 1u >= limit) {
            if (direct_trace_enabled()) fprintf(stderr, "[direct-hle] task wake t=%08x saved_pc=%08x limit=%u\n", t, saved_pc, limit);
            direct_write32_if_ram(g, t + 0x20u, 0u);
            direct_write32_if_ram(g, t + 0x14u, 2u);
        } else {
            direct_write32_if_ram(g, t + 0x20u, elapsed + 1u);
        }
    }
}

static int direct_resume_ready_sdk_task(gp32_t *g, uint32_t pc, uint32_t first_task, uint32_t last_task) {
    if (!g || !g->direct_fxe_mode || !g->cpu) return 0;
    uint32_t start = GP32_RAM_BASE, end = GP32_RAM_BASE, step = 4u;
    direct_task_scan_bounds(g, first_task, last_task, &start, &end, &step);
    for (uint32_t t = start; t + 0x34u <= end; t += step) {
        if (!direct_task_record_plausible(g, t)) continue;
        if (direct_gpos_task_is_internal_timer(g, t)) continue;
        uint32_t state = s3c2400_debug_read32(g->soc, t + 0x14u);
        if (state != 2u) continue;
        uint32_t saved_sp = s3c2400_debug_read32(g->soc, t + 0u);
        if (!direct_ram_range(g, saved_sp, 64u)) continue;
        uint32_t saved_cpsr = s3c2400_debug_read32(g->soc, saved_sp + 0u);
        uint32_t saved_pc = s3c2400_debug_read32(g->soc, saved_sp + 60u);
        if ((saved_cpsr & 0x1fu) != ARM_MODE_SVC && (saved_cpsr & 0x1fu) != 0x10u) continue;
        if (!direct_ram_range(g, saved_pc & ~1u, 4u)) continue;
        uint32_t saved_insn = s3c2400_debug_read32(g->soc, saved_pc & ~1u);
        if (saved_insn == 0xeafffffeu || saved_insn == 0xeaffffffu) continue;
        if (direct_trace_enabled()) fprintf(stderr, "[direct-hle] resume ready task t=%08x saved_pc=%08x from=%08x\n", t, saved_pc, pc);
        direct_set_task_scheduler_current(g, t, pc);
        direct_write32_if_ram(g, t + 0x14u, 1u);
        if (direct_restore_saved_context(g, t)) return 1;
    }
    return 0;
}

static int direct_restore_saved_context(gp32_t *g, uint32_t task_addr) {
    if (!g || !direct_ram_range(g, task_addr, 0x34u)) return 0;
    uint32_t saved_sp = s3c2400_debug_read32(g->soc, task_addr + 0u);
    if (!direct_ram_range(g, saved_sp, 16u * 4u)) return 0;
    uint32_t saved_cpsr = s3c2400_debug_read32(g->soc, saved_sp + 0u);
    uint32_t new_pc = s3c2400_debug_read32(g->soc, saved_sp + 60u);
    if (!direct_ram_range(g, new_pc & ~1u, 4u)) return 0;
    arm920t_set_cpsr(g->cpu, saved_cpsr);
    for (unsigned i = 0; i < 13u; ++i) {
        arm920t_set_reg(g->cpu, i, s3c2400_debug_read32(g->soc, saved_sp + 4u + i * 4u));
    }
    arm920t_set_reg(g->cpu, 14, s3c2400_debug_read32(g->soc, saved_sp + 56u));
    arm920t_set_reg(g->cpu, 13, saved_sp + 64u);
    arm920t_set_reg(g->cpu, 15, new_pc);
    direct_write32_if_ram(g, task_addr + 0u, saved_sp + 64u);
    direct_write32_if_ram(g, task_addr + 0x14u, 1u);
    return 1;
}

static int direct_try_resume_sdk_task(gp32_t *g) {
    if (!g || !g->direct_fxe_mode || !g->cpu) return 0;
    uint32_t pc = arm920t_get_pc(g->cpu);
    if (!direct_ram_range(g, pc, 4u) || s3c2400_debug_read32(g->soc, pc) != 0xeafffffeu) return 0;

    /* Several devkitPro/official-GPSDK CRTs fall back into a resident SDK idle
       task after their firmware scheduler setup SWI is stubbed.  The idle loop
       is followed by the SDK's context-builder helper and preceded by a small
       literal pool containing the task globals.  Use that literal pool to
       perform the same saved-context restore the SDK scheduler would have done,
       rather than hard-coding an AKA NOID address. */
    uint32_t current_slot = s3c2400_debug_read32(g->soc, pc - 0x70u);
    uint32_t ready_slot = s3c2400_debug_read32(g->soc, pc - 0x6cu);
    uint32_t task_base = s3c2400_debug_read32(g->soc, pc - 0x60u);
    if (direct_ram_range(g, current_slot, 4u) && direct_ram_range(g, ready_slot, 4u) &&
        direct_ram_range(g, task_base, 8u * 0x34u)) {
        uint32_t current = s3c2400_debug_read32(g->soc, current_slot);
        for (unsigned i = 0; i < 8u; ++i) {
            uint32_t t = task_base + i * 0x34u;
            if (t == current || direct_gpos_task_is_internal_timer(g, t)) continue;
            uint32_t saved_sp = s3c2400_debug_read32(g->soc, t + 0u);
            if (!direct_task_state_can_run(g, t) || !direct_ram_range(g, saved_sp, 64u)) continue;
            uint32_t new_pc = s3c2400_debug_read32(g->soc, saved_sp + 60u);
            if (new_pc == pc || !direct_ram_range(g, new_pc & ~1u, 4u)) continue;
            direct_write32_if_ram(g, current_slot, t);
            direct_write32_if_ram(g, ready_slot, t);
            direct_write32_if_ram(g, t + 0x14u, 1u);
            if (direct_restore_saved_context(g, t)) return 1;
        }
    }
    /*
     * Fallback for SDK task loops whose literal-pool layout differs from the
     * pattern above.  Some GPSDK programs park in a permanent branch while
     * another task context is already marked runnable in the firmware task
     * table.  Scan for the same task-record shape used by the normal restore
     * path: saved SP at +0, runnable state at +0x14, and a saved ARM context
     * whose PC points back into RAM.
     */
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    for (uint32_t t = GP32_RAM_BASE; t + 0x34u < ram_end; t += 4u) {
        uint32_t state = s3c2400_debug_read32(g->soc, t + 0x14u);
        if (direct_gpos_task_is_internal_timer(g, t)) continue;
        if (state == 4u && direct_task_record_plausible(g, t)) {
            /*
             * Firmware-resident GPSDK schedulers sleep tasks by marking their
             * task control block state as 4 and counting scheduler ticks at
             * +0x20 until the delay at +0x24 expires.  In direct-HLE mode
             * these timer IRQ callbacks do not run once the resident idle task
             * has fallen into its permanent branch, so titles such as
             * W.B.W. can park forever with a runnable game task still asleep.
             * When the CPU is demonstrably executing that idle self-branch,
             * advance one scheduler tick while scanning the task table and let
             * the normal context-restore path below resume the awakened task.
             */
            uint32_t elapsed = s3c2400_debug_read32(g->soc, t + 0x20u);
            uint32_t limit = s3c2400_debug_read32(g->soc, t + 0x24u);
            if (limit == 0u || elapsed + 1u >= limit) {
                if (direct_trace_enabled()) fprintf(stderr, "[direct-hle] idle task wake t=%08x limit=%u\n", t, limit);
                direct_write32_if_ram(g, t + 0x20u, 0u);
                direct_write32_if_ram(g, t + 0x14u, 2u);
                state = 2u;
            } else {
                direct_write32_if_ram(g, t + 0x20u, elapsed + 1u);
            }
        }
        if (!direct_task_state_can_run(g, t)) continue;
        uint32_t saved_sp = s3c2400_debug_read32(g->soc, t + 0u);
        if (state == 8u) {
            uint32_t link_or_stack = s3c2400_debug_read32(g->soc, t + 4u);
            uint32_t stack_size = s3c2400_debug_read32(g->soc, t + 8u);
            if (!direct_ram_range(g, link_or_stack, 4u) || stack_size == 0u || stack_size > 0x20000u) continue;
        }
        if (!direct_ram_range(g, saved_sp, 64u)) continue;
        uint32_t saved_cpsr = s3c2400_debug_read32(g->soc, saved_sp + 0u);
        uint32_t saved_pc = s3c2400_debug_read32(g->soc, saved_sp + 60u);
        if ((saved_cpsr & 0x1fu) != ARM_MODE_SVC && (saved_cpsr & 0x1fu) != 0x10u) continue;
        if (!direct_ram_range(g, saved_pc & ~1u, 4u)) continue;
        if ((saved_pc & ~1u) == pc || ((saved_pc + 4u) & ~1u) == pc) continue;
        uint32_t saved_insn = s3c2400_debug_read32(g->soc, saved_pc & ~1u);
        if (saved_insn == 0xeafffffeu || saved_insn == 0xeaffffffu) continue;
        direct_set_task_scheduler_current(g, t, pc);
        direct_write32_if_ram(g, t + 0x14u, 1u);
        if (direct_restore_saved_context(g, t)) return 1;
    }
    return 0;
}

gp32_status_t gp32_run_cycles(gp32_t *g, uint32_t cycles) {
    if (!g) return GP32_ERR_INVALID_ARGUMENT;
    direct_fix_gp32_additive_blend_shadow_endpoint(g);
    uint32_t remaining = cycles;
    while (remaining) {
        uint32_t idle = direct_consume_idle_wait(g, remaining);
        if (idle) {
            remaining -= idle;
            continue;
        }
        uint32_t slice = remaining > 32768u ? 32768u : remaining;
        direct_update_fw_tick(g);
        uint32_t done = arm920t_run(g->cpu, slice);
        s3c2400_tick(g->soc, done);
        direct_hle_gpos_timer_tick(g, done);
        direct_hle_audio_tick(g, done);
        direct_update_fw_tick(g);
        direct_process_asset_autoload(g);
        direct_schedule_vblank_wait(g);
        if (done == 0) {
            if (direct_try_file_hle(g)) continue;
            if (direct_try_resume_sdk_task(g)) continue;
            break;
        }
        if (direct_try_file_hle(g)) {
            remaining -= done;
            continue;
        }
        if (direct_try_resume_sdk_task(g)) {
            remaining -= done;
            continue;
        }
        remaining -= done;
    }
    direct_update_fw_tick(g);
    direct_process_asset_autoload(g);
    direct_fix_gp32_additive_blend_shadow_endpoint(g);
    direct_select_visible_surface(g);
    return GP32_OK;
}

gp32_status_t gp32_set_jit(gp32_t *g, int enabled) {
    if (!g || !g->cpu) return GP32_ERR_INVALID_ARGUMENT;
    arm920t_set_jit(g->cpu, enabled);
    return GP32_OK;
}

gp32_status_t gp32_set_hle_sef_rate(gp32_t *g, uint32_t sample_rate_hz) {
    if (!g) return GP32_ERR_INVALID_ARGUMENT;
    if (sample_rate_hz && (sample_rate_hz < 4000u || sample_rate_hz > 192000u)) {
        seterr(g, "HLE SEF sample rate must be 0 or 4000..192000 Hz");
        return GP32_ERR_INVALID_ARGUMENT;
    }
    g->direct_hle_audio_rate_override = sample_rate_hz;
    if (sample_rate_hz && g->direct_hle_audio_asset) {
        g->direct_hle_audio_rate = sample_rate_hz;
        g->direct_hle_audio_accum = 0;
    }
    return GP32_OK;
}

gp32_status_t gp32_set_buttons(gp32_t *g, uint32_t mask) {
    if (!g) return GP32_ERR_INVALID_ARGUMENT;
    s3c2400_set_buttons(g->soc, mask);
    return GP32_OK;
}

gp32_status_t gp32_get_framebuffer(gp32_t *g, gp32_framebuffer_desc_t *out) {
    if (!g || !out) return GP32_ERR_INVALID_ARGUMENT;
    direct_fix_gp32_additive_blend_shadow_endpoint(g);
    direct_fix_gp32_additive_blend_shadow_pixels(g);
    uint32_t w,h,stride; uint64_t frames;
    const uint32_t *pix = s3c2400_framebuffer(g->soc, &w, &h, &stride, &frames);
    out->pixels_rgba8888 = pix;
    out->width = w;
    out->height = h;
    out->stride_pixels = stride;
    out->frame_counter = frames;
    return pix ? GP32_OK : GP32_ERR_INVALID_ARGUMENT;
}

gp32_status_t gp32_get_audio(gp32_t *g, gp32_audio_desc_t *out) {
    if (!g || !out) return GP32_ERR_INVALID_ARGUMENT;
    uint64_t frames = 0;
    uint32_t rate = 0;
    const int16_t *samples = s3c2400_audio_samples(g->soc, &frames, &rate);
    out->samples_s16_interleaved = samples;
    out->frame_count = frames;
    out->sample_rate_hz = rate;
    return GP32_OK;
}

gp32_status_t gp32_clear_audio(gp32_t *g) {
    if (!g) return GP32_ERR_INVALID_ARGUMENT;
    s3c2400_audio_clear(g->soc);
    return GP32_OK;
}

uint32_t gp32_get_pc(const gp32_t *g) { return g ? arm920t_get_pc(g->cpu) : 0; }
uint32_t gp32_get_cpu_reg(const gp32_t *g, unsigned reg) { return g ? arm920t_get_reg(g->cpu, reg) : 0; }
uint32_t gp32_get_cpsr(const gp32_t *g) { return g ? arm920t_get_cpsr(g->cpu) : 0; }
uint32_t gp32_get_cp15(const gp32_t *g, unsigned reg) { return g ? arm920t_get_cp15(g->cpu, reg) : 0; }
uint32_t gp32_debug_read32(gp32_t *g, uint32_t addr) { return g ? s3c2400_debug_read32(g->soc, addr) : 0xffffffffu; }
uint64_t gp32_get_cycles(const gp32_t *g) { return g ? arm920t_get_cycles(g->cpu) : 0; }
uint32_t gp32_get_fclk_hz(const gp32_t *g) { return g ? s3c2400_fclk_hz(g->soc) : 0u; }
uint32_t gp32_get_run_clock_hz(const gp32_t *g) { return g ? s3c2400_run_clock_hz(g->soc) : 0u; }
uint64_t gp32_get_jit_hits(const gp32_t *g) { return g ? arm920t_get_jit_hits(g->cpu) : 0; }
uint64_t gp32_get_jit_misses(const gp32_t *g) { return g ? arm920t_get_jit_misses(g->cpu) : 0; }
uint64_t gp32_get_jit_fallbacks(const gp32_t *g) { return g ? arm920t_get_jit_fallbacks(g->cpu) : 0; }
const char *gp32_get_error(const gp32_t *g) { return g ? g->error : "invalid gp32 handle"; }

typedef struct gp32_fpk_handle_state_image {
    int32_t asset_index;
    size_t pos;
    int used;
} gp32_fpk_handle_state_image_t;

typedef struct gp32_state_image {
    int direct_fxe_mode;
    uint32_t direct_fxe_entry;
    uint32_t direct_fxe_stack;
    uint32_t direct_fxe_fb_addr;
    uint32_t direct_fxe_image_end;
    uint32_t direct_fxe_lcd_surface[4];
    uint32_t direct_fxe_surface_checksum[4];
    uint32_t direct_fxe_bpp;
    uint32_t direct_fxe_palette_addr;
    uint32_t direct_fxe_palette_initialized;
    uint32_t direct_fxe_lcd_explicit;
    uint32_t direct_fxe_lcd_enabled;
    char direct_fxe_title[33];
    char direct_smc_executable_path[260];
    char direct_smc_game_dir[260];
    gp32_fpk_handle_state_image_t direct_fpk_handles[32];
    uint32_t direct_hle_file_open_addr;
    uint32_t direct_hle_file_read_addr[2];
    uint32_t direct_hle_file_close_addr;
    uint32_t direct_hle_file_size_addr;
    uint32_t direct_hle_file_seek_addr;
    uint32_t direct_hle_pathbuf_addr;
    uint32_t direct_hle_sound_dispatch_addr;
    uint32_t direct_hle_sound_table_addr;
    uint32_t direct_hle_sound_play_addr;
    uint32_t direct_hle_sound_state_addr;
    uint32_t direct_hle_pcm_env_addr;
    uint32_t direct_hle_pcm_init_addr;
    uint32_t direct_hle_pcm_play_addr;
    uint32_t direct_hle_pcm_stop_addr;
    uint32_t direct_hle_pcm_remove_addr;
    uint32_t direct_hle_pcm_lock_addr;
    uint32_t direct_hle_pcm_only_kill_addr;
    uint32_t direct_hle_pcm_initialized;
    uint32_t direct_hle_pcm_sr;
    uint32_t direct_hle_pcm_bit_count;
    uint32_t direct_hle_pcm_rate;
    uint32_t direct_hle_pcm_stereo;
    uint32_t direct_hle_pcm_bits;
    uint32_t direct_hle_pcm_active;
    uint32_t direct_hle_pcm_src_addr;
    uint32_t direct_hle_pcm_size_bytes;
    uint32_t direct_hle_pcm_pos_bytes;
    uint32_t direct_hle_pcm_repeat;
    uint64_t direct_hle_pcm_accum;
    uint32_t direct_hle_sdk_sndmixedbuf_addr;
    uint32_t direct_hle_sdk_sndsrcexist_addr;
    uint32_t direct_hle_sdk_pcm_workidx_addr;
    uint32_t direct_hle_sdk_sndmixer_addr;
    uint32_t direct_hle_sdk_mixbuf0_addr;
    uint32_t direct_hle_sdk_mixbuf1_addr;
    uint32_t direct_hle_sdk_mixbuf_bytes;
    uint32_t direct_hle_sdk_rate;
    uint64_t direct_hle_sdk_accum;
    uint64_t direct_hle_sdk_last_submit_cycle;
    uint64_t direct_hle_sdk_timer_accum;
    uint32_t direct_hle_sdk_submitted_frames;
    uint32_t direct_hle_sdk_timer_table_addr;
    struct {
        uint32_t configured;
        uint32_t enabled;
        uint32_t callback;
        uint32_t tps;
        uint32_t max_exec_tick;
        uint64_t accum;
    } direct_hle_gpos_timer[GP32_DIRECT_GPOS_TIMER_COUNT];
    uint32_t direct_hle_gpos_timers_enabled;
    uint32_t direct_hle_gpos_task_first;
    uint32_t direct_hle_gpos_task_last;
    uint32_t direct_hle_gpos_scheduler_callback;
    uint32_t direct_hle_callback_returned;
    uint32_t direct_hle_callback_running;
    uint32_t direct_hle_audio_rate_override;
    uint32_t direct_hle_audio_last_auto_rate;
    int32_t direct_hle_audio_asset_index;
    uint32_t direct_hle_audio_pos;
    uint32_t direct_hle_audio_size;
    uint32_t direct_hle_audio_rate;
    uint64_t direct_hle_audio_accum;
    struct {
        const fpk_asset_t *asset;
        uint32_t copied;
        uint32_t tries;
    } direct_hle_asset_autoload[16];
    uint64_t direct_vblank_next_cycle;
    uint64_t direct_vblank_wait_cycles;
    int direct_vblank_wait_requested;
} gp32_state_image_t;

static int32_t direct_asset_index(const gp32_t *g, const fpk_asset_t *asset) {
    if (!g || !asset || !g->direct_fpk_assets || g->direct_fpk_asset_count == 0) return -1;
    for (size_t i = 0; i < g->direct_fpk_asset_count; ++i) {
        if (&g->direct_fpk_assets[i] == asset) return (int32_t)i;
    }
    return -1;
}

static const fpk_asset_t *direct_asset_from_index(const gp32_t *g, int32_t index) {
    if (!g || index < 0 || !g->direct_fpk_assets) return NULL;
    if ((size_t)index >= g->direct_fpk_asset_count) return NULL;
    return &g->direct_fpk_assets[index];
}

static void gp32_direct_state_capture(const gp32_t *g, gp32_state_image_t *st) {
    memset(st, 0, sizeof(*st));
    st->direct_fxe_mode = g->direct_fxe_mode;
    st->direct_fxe_entry = g->direct_fxe_entry;
    st->direct_fxe_stack = g->direct_fxe_stack;
    st->direct_fxe_fb_addr = g->direct_fxe_fb_addr;
    st->direct_fxe_image_end = g->direct_fxe_image_end;
    memcpy(st->direct_fxe_lcd_surface, g->direct_fxe_lcd_surface, sizeof(st->direct_fxe_lcd_surface));
    memcpy(st->direct_fxe_surface_checksum, g->direct_fxe_surface_checksum, sizeof(st->direct_fxe_surface_checksum));
    st->direct_fxe_bpp = g->direct_fxe_bpp;
    st->direct_fxe_palette_addr = g->direct_fxe_palette_addr;
    st->direct_fxe_palette_initialized = g->direct_fxe_palette_initialized;
    st->direct_fxe_lcd_explicit = g->direct_fxe_lcd_explicit;
    st->direct_fxe_lcd_enabled = g->direct_fxe_lcd_enabled;
    memcpy(st->direct_fxe_title, g->direct_fxe_title, sizeof(st->direct_fxe_title));
    memcpy(st->direct_smc_executable_path, g->direct_smc_executable_path, sizeof(st->direct_smc_executable_path));
    memcpy(st->direct_smc_game_dir, g->direct_smc_game_dir, sizeof(st->direct_smc_game_dir));
    for (size_t i = 0; i < GP32_ARRAY_COUNT(st->direct_fpk_handles); ++i) {
        st->direct_fpk_handles[i].asset_index = direct_asset_index(g, g->direct_fpk_handles[i].asset);
        st->direct_fpk_handles[i].pos = g->direct_fpk_handles[i].pos;
        st->direct_fpk_handles[i].used = g->direct_fpk_handles[i].used;
    }
    st->direct_hle_file_open_addr = g->direct_hle_file_open_addr;
    memcpy(st->direct_hle_file_read_addr, g->direct_hle_file_read_addr, sizeof(st->direct_hle_file_read_addr));
    st->direct_hle_file_close_addr = g->direct_hle_file_close_addr;
    st->direct_hle_file_size_addr = g->direct_hle_file_size_addr;
    st->direct_hle_file_seek_addr = g->direct_hle_file_seek_addr;
    st->direct_hle_pathbuf_addr = g->direct_hle_pathbuf_addr;
    st->direct_hle_sound_dispatch_addr = g->direct_hle_sound_dispatch_addr;
    st->direct_hle_sound_table_addr = g->direct_hle_sound_table_addr;
    st->direct_hle_sound_play_addr = g->direct_hle_sound_play_addr;
    st->direct_hle_sound_state_addr = g->direct_hle_sound_state_addr;
    st->direct_hle_pcm_env_addr = g->direct_hle_pcm_env_addr;
    st->direct_hle_pcm_init_addr = g->direct_hle_pcm_init_addr;
    st->direct_hle_pcm_play_addr = g->direct_hle_pcm_play_addr;
    st->direct_hle_pcm_stop_addr = g->direct_hle_pcm_stop_addr;
    st->direct_hle_pcm_remove_addr = g->direct_hle_pcm_remove_addr;
    st->direct_hle_pcm_lock_addr = g->direct_hle_pcm_lock_addr;
    st->direct_hle_pcm_only_kill_addr = g->direct_hle_pcm_only_kill_addr;
    st->direct_hle_pcm_initialized = g->direct_hle_pcm_initialized;
    st->direct_hle_pcm_sr = g->direct_hle_pcm_sr;
    st->direct_hle_pcm_bit_count = g->direct_hle_pcm_bit_count;
    st->direct_hle_pcm_rate = g->direct_hle_pcm_rate;
    st->direct_hle_pcm_stereo = g->direct_hle_pcm_stereo;
    st->direct_hle_pcm_bits = g->direct_hle_pcm_bits;
    st->direct_hle_pcm_active = g->direct_hle_pcm_active;
    st->direct_hle_pcm_src_addr = g->direct_hle_pcm_src_addr;
    st->direct_hle_pcm_size_bytes = g->direct_hle_pcm_size_bytes;
    st->direct_hle_pcm_pos_bytes = g->direct_hle_pcm_pos_bytes;
    st->direct_hle_pcm_repeat = g->direct_hle_pcm_repeat;
    st->direct_hle_pcm_accum = g->direct_hle_pcm_accum;
    st->direct_hle_sdk_sndmixedbuf_addr = g->direct_hle_sdk_sndmixedbuf_addr;
    st->direct_hle_sdk_sndsrcexist_addr = g->direct_hle_sdk_sndsrcexist_addr;
    st->direct_hle_sdk_pcm_workidx_addr = g->direct_hle_sdk_pcm_workidx_addr;
    st->direct_hle_sdk_sndmixer_addr = g->direct_hle_sdk_sndmixer_addr;
    st->direct_hle_sdk_mixbuf0_addr = g->direct_hle_sdk_mixbuf0_addr;
    st->direct_hle_sdk_mixbuf1_addr = g->direct_hle_sdk_mixbuf1_addr;
    st->direct_hle_sdk_mixbuf_bytes = g->direct_hle_sdk_mixbuf_bytes;
    st->direct_hle_sdk_rate = g->direct_hle_sdk_rate;
    st->direct_hle_sdk_accum = g->direct_hle_sdk_accum;
    st->direct_hle_sdk_last_submit_cycle = g->direct_hle_sdk_last_submit_cycle;
    st->direct_hle_sdk_timer_accum = g->direct_hle_sdk_timer_accum;
    st->direct_hle_sdk_submitted_frames = g->direct_hle_sdk_submitted_frames;
    st->direct_hle_sdk_timer_table_addr = g->direct_hle_sdk_timer_table_addr;
    memcpy(st->direct_hle_gpos_timer, g->direct_hle_gpos_timer, sizeof(st->direct_hle_gpos_timer));
    st->direct_hle_gpos_timers_enabled = g->direct_hle_gpos_timers_enabled;
    st->direct_hle_gpos_task_first = g->direct_hle_gpos_task_first;
    st->direct_hle_gpos_task_last = g->direct_hle_gpos_task_last;
    st->direct_hle_gpos_scheduler_callback = g->direct_hle_gpos_scheduler_callback;
    st->direct_hle_callback_returned = g->direct_hle_callback_returned;
    st->direct_hle_callback_running = g->direct_hle_callback_running;
    st->direct_hle_audio_rate_override = g->direct_hle_audio_rate_override;
    st->direct_hle_audio_last_auto_rate = g->direct_hle_audio_last_auto_rate;
    st->direct_hle_audio_asset_index = direct_asset_index(g, g->direct_hle_audio_asset);
    st->direct_hle_audio_pos = g->direct_hle_audio_pos;
    st->direct_hle_audio_size = g->direct_hle_audio_size;
    st->direct_hle_audio_rate = g->direct_hle_audio_rate;
    st->direct_hle_audio_accum = g->direct_hle_audio_accum;
    st->direct_vblank_next_cycle = g->direct_vblank_next_cycle;
    st->direct_vblank_wait_cycles = g->direct_vblank_wait_cycles;
    st->direct_vblank_wait_requested = g->direct_vblank_wait_requested;
}

static void gp32_direct_state_apply(gp32_t *g, const gp32_state_image_t *st) {
    g->direct_fxe_mode = st->direct_fxe_mode;
    g->direct_fxe_entry = st->direct_fxe_entry;
    g->direct_fxe_stack = st->direct_fxe_stack;
    g->direct_fxe_fb_addr = st->direct_fxe_fb_addr;
    g->direct_fxe_image_end = st->direct_fxe_image_end;
    memcpy(g->direct_fxe_lcd_surface, st->direct_fxe_lcd_surface, sizeof(g->direct_fxe_lcd_surface));
    memcpy(g->direct_fxe_surface_checksum, st->direct_fxe_surface_checksum, sizeof(g->direct_fxe_surface_checksum));
    g->direct_fxe_bpp = st->direct_fxe_bpp;
    g->direct_fxe_palette_addr = st->direct_fxe_palette_addr;
    g->direct_fxe_palette_initialized = st->direct_fxe_palette_initialized;
    g->direct_fxe_lcd_explicit = st->direct_fxe_lcd_explicit;
    g->direct_fxe_lcd_enabled = st->direct_fxe_lcd_enabled;
    memcpy(g->direct_fxe_title, st->direct_fxe_title, sizeof(g->direct_fxe_title));
    g->direct_fxe_title[sizeof(g->direct_fxe_title) - 1u] = '\0';
    memcpy(g->direct_smc_executable_path, st->direct_smc_executable_path, sizeof(g->direct_smc_executable_path));
    g->direct_smc_executable_path[sizeof(g->direct_smc_executable_path) - 1u] = '\0';
    memcpy(g->direct_smc_game_dir, st->direct_smc_game_dir, sizeof(g->direct_smc_game_dir));
    g->direct_smc_game_dir[sizeof(g->direct_smc_game_dir) - 1u] = '\0';
    for (size_t i = 0; i < GP32_ARRAY_COUNT(st->direct_fpk_handles); ++i) {
        g->direct_fpk_handles[i].asset = direct_asset_from_index(g, st->direct_fpk_handles[i].asset_index);
        g->direct_fpk_handles[i].pos = st->direct_fpk_handles[i].pos;
        g->direct_fpk_handles[i].used = st->direct_fpk_handles[i].used && g->direct_fpk_handles[i].asset;
    }
    g->direct_hle_file_open_addr = st->direct_hle_file_open_addr;
    memcpy(g->direct_hle_file_read_addr, st->direct_hle_file_read_addr, sizeof(g->direct_hle_file_read_addr));
    g->direct_hle_file_close_addr = st->direct_hle_file_close_addr;
    g->direct_hle_file_size_addr = st->direct_hle_file_size_addr;
    g->direct_hle_file_seek_addr = st->direct_hle_file_seek_addr;
    g->direct_hle_pathbuf_addr = st->direct_hle_pathbuf_addr;
    g->direct_hle_sound_dispatch_addr = st->direct_hle_sound_dispatch_addr;
    g->direct_hle_sound_table_addr = st->direct_hle_sound_table_addr;
    g->direct_hle_sound_play_addr = st->direct_hle_sound_play_addr;
    g->direct_hle_sound_state_addr = st->direct_hle_sound_state_addr;
    g->direct_hle_pcm_env_addr = st->direct_hle_pcm_env_addr;
    g->direct_hle_pcm_init_addr = st->direct_hle_pcm_init_addr;
    g->direct_hle_pcm_play_addr = st->direct_hle_pcm_play_addr;
    g->direct_hle_pcm_stop_addr = st->direct_hle_pcm_stop_addr;
    g->direct_hle_pcm_remove_addr = st->direct_hle_pcm_remove_addr;
    g->direct_hle_pcm_lock_addr = st->direct_hle_pcm_lock_addr;
    g->direct_hle_pcm_only_kill_addr = st->direct_hle_pcm_only_kill_addr;
    g->direct_hle_pcm_initialized = st->direct_hle_pcm_initialized;
    g->direct_hle_pcm_sr = st->direct_hle_pcm_sr;
    g->direct_hle_pcm_bit_count = st->direct_hle_pcm_bit_count;
    g->direct_hle_pcm_rate = st->direct_hle_pcm_rate;
    g->direct_hle_pcm_stereo = st->direct_hle_pcm_stereo;
    g->direct_hle_pcm_bits = st->direct_hle_pcm_bits;
    g->direct_hle_pcm_active = st->direct_hle_pcm_active;
    g->direct_hle_pcm_src_addr = st->direct_hle_pcm_src_addr;
    g->direct_hle_pcm_size_bytes = st->direct_hle_pcm_size_bytes;
    g->direct_hle_pcm_pos_bytes = st->direct_hle_pcm_pos_bytes;
    g->direct_hle_pcm_repeat = st->direct_hle_pcm_repeat;
    g->direct_hle_pcm_accum = st->direct_hle_pcm_accum;
    memset(g->direct_hle_pcm_ch, 0, sizeof(g->direct_hle_pcm_ch));
    if (g->direct_hle_pcm_active) {
        g->direct_hle_pcm_ch[0].active = g->direct_hle_pcm_active;
        g->direct_hle_pcm_ch[0].src_addr = g->direct_hle_pcm_src_addr;
        g->direct_hle_pcm_ch[0].size_bytes = g->direct_hle_pcm_size_bytes;
        g->direct_hle_pcm_ch[0].pos_bytes = g->direct_hle_pcm_pos_bytes;
        g->direct_hle_pcm_ch[0].repeat = g->direct_hle_pcm_repeat;
        g->direct_hle_pcm_ch[0].rate = g->direct_hle_pcm_rate;
        g->direct_hle_pcm_ch[0].stereo = g->direct_hle_pcm_stereo;
        g->direct_hle_pcm_ch[0].bits = g->direct_hle_pcm_bits;
    }
    g->direct_hle_sdk_sndmixedbuf_addr = st->direct_hle_sdk_sndmixedbuf_addr;
    g->direct_hle_sdk_sndsrcexist_addr = st->direct_hle_sdk_sndsrcexist_addr;
    g->direct_hle_sdk_pcm_workidx_addr = st->direct_hle_sdk_pcm_workidx_addr;
    g->direct_hle_sdk_sndmixer_addr = st->direct_hle_sdk_sndmixer_addr;
    g->direct_hle_sdk_mixbuf0_addr = st->direct_hle_sdk_mixbuf0_addr;
    g->direct_hle_sdk_mixbuf1_addr = st->direct_hle_sdk_mixbuf1_addr;
    g->direct_hle_sdk_mixbuf_bytes = st->direct_hle_sdk_mixbuf_bytes;
    g->direct_hle_sdk_rate = st->direct_hle_sdk_rate;
    g->direct_hle_sdk_accum = st->direct_hle_sdk_accum;
    g->direct_hle_sdk_last_submit_cycle = st->direct_hle_sdk_last_submit_cycle;
    g->direct_hle_sdk_timer_accum = st->direct_hle_sdk_timer_accum;
    g->direct_hle_sdk_submitted_frames = st->direct_hle_sdk_submitted_frames;
    g->direct_hle_sdk_timer_table_addr = st->direct_hle_sdk_timer_table_addr;
    memcpy(g->direct_hle_gpos_timer, st->direct_hle_gpos_timer, sizeof(g->direct_hle_gpos_timer));
    g->direct_hle_gpos_timers_enabled = st->direct_hle_gpos_timers_enabled;
    g->direct_hle_gpos_task_first = st->direct_hle_gpos_task_first;
    g->direct_hle_gpos_task_last = st->direct_hle_gpos_task_last;
    g->direct_hle_gpos_scheduler_callback = st->direct_hle_gpos_scheduler_callback;
    g->direct_hle_callback_returned = st->direct_hle_callback_returned;
    g->direct_hle_callback_running = st->direct_hle_callback_running;
    g->direct_hle_audio_rate_override = st->direct_hle_audio_rate_override;
    g->direct_hle_audio_last_auto_rate = st->direct_hle_audio_last_auto_rate;
    g->direct_hle_audio_asset = direct_asset_from_index(g, st->direct_hle_audio_asset_index);
    g->direct_hle_audio_pos = st->direct_hle_audio_pos;
    g->direct_hle_audio_size = st->direct_hle_audio_size;
    g->direct_hle_audio_rate = st->direct_hle_audio_rate;
    g->direct_hle_audio_accum = st->direct_hle_audio_accum;
    g->direct_vblank_next_cycle = st->direct_vblank_next_cycle;
    g->direct_vblank_wait_cycles = st->direct_vblank_wait_cycles;
    g->direct_vblank_wait_requested = st->direct_vblank_wait_requested;
}

gp32_status_t gp32_save_state(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "wb");
    if (!f) { seterr(g, "open savestate %s: %s", path, strerror(errno)); return GP32_ERR_IO; }
    const uint8_t magic[16] = { 'G','P','3','2','S','T','A','T','E','v','0','0','0','2',0,0 };
    gp32_state_image_t direct;
    gp32_direct_state_capture(g, &direct);
    int ok = fwrite(magic, 1, sizeof(magic), f) == sizeof(magic) &&
             fwrite(&direct, 1, sizeof(direct), f) == sizeof(direct) &&
             arm920t_state_save(g->cpu, f) &&
             s3c2400_state_save(g->soc, f);
    if (fclose(f) != 0) ok = 0;
    if (!ok) { seterr(g, "write savestate %s failed", path); return GP32_ERR_IO; }
    return GP32_OK;
}

gp32_status_t gp32_load_state(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "rb");
    if (!f) { seterr(g, "open savestate %s: %s", path, strerror(errno)); return GP32_ERR_IO; }
    const uint8_t magic[16] = { 'G','P','3','2','S','T','A','T','E','v','0','0','0','2',0,0 };
    uint8_t got[16];
    gp32_state_image_t direct;
    int ok = fread(got, 1, sizeof(got), f) == sizeof(got) && memcmp(got, magic, sizeof(magic)) == 0 &&
             fread(&direct, 1, sizeof(direct), f) == sizeof(direct) &&
             arm920t_state_load(g->cpu, f) &&
             s3c2400_state_load(g->soc, f);
    if (fclose(f) != 0) ok = 0;
    if (!ok) { seterr(g, "load savestate %s failed or unsupported version", path); return GP32_ERR_IO; }
    gp32_direct_state_apply(g, &direct);
    s3c2400_set_irq_sink(g->soc, g->cpu);
    arm920t_set_swi_handler(g->cpu, direct_fxe_swi, g);
    gp32_clear_audio(g);
    direct_fix_gp32_additive_blend_shadow_endpoint(g);
    return GP32_OK;
}
