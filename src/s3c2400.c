/*
 * Samsung S3C2400X GP32 SoC model. Register layout, GPIO/SmartMedia wiring,
 * LCD DMA palette format and memory map are derived from MAME gp32.cpp.
 * Original license: BSD-3-Clause, copyright Tim Schuerewegen.
 */
#include "s3c2400.h"
#include "gp32emu/gp32.h"
#include "zip.h"

#define BIOS_SIZE 0x80000u
#define RAM_BASE  0x0c000000u
#define RAM_DEFAULT_SIZE 0x00800000u
#define MPLLCON 1

#define INT_ADC       31
#define INT_RTC       30
#define INT_UTXD1     29
#define INT_UTXD0     28
#define INT_IIC       27
#define INT_USBH      26
#define INT_USBD      25
#define INT_URXD1     24
#define INT_URXD0     23
#define INT_SPI       22
#define INT_MMC       21
#define INT_DMA3      20
#define INT_DMA2      19
#define INT_DMA1      18
#define INT_DMA0      17
#define INT_TIMER4    14
#define INT_TIMER3    13
#define INT_TIMER2    12
#define INT_TIMER1    11
#define INT_TIMER0    10

#define BPPMODE_TFT_01 0x08u
#define BPPMODE_TFT_02 0x09u
#define BPPMODE_TFT_04 0x0au
#define BPPMODE_TFT_08 0x0bu
#define BPPMODE_TFT_16 0x0cu

typedef struct gp32_smc_lines {
    int add_latch;
    int chip;
    int cmd_latch;
    int do_read;
    int do_write;
    int read;
    int wp;
    int busy;
    uint8_t datarx;
    uint8_t datatx;
} gp32_smc_lines_t;

typedef struct lcd_state {
    uint32_t vramaddr_cur, vramaddr_max, offsize, pagewidth_cur, pagewidth_max;
    uint32_t bppmode, bswp, hwswp, hozval, lineval;
    uint32_t width, height;
} lcd_state_t;

struct s3c2400 {
    uint8_t bios[BIOS_SIZE];
    uint8_t *ram;
    size_t ram_size;
    smc_t *smc;
    arm920t_t *cpu_irq_sink;
    uint32_t buttons;
    uint32_t fb[320 * 240];
    uint32_t fb_w, fb_h;
    uint64_t frame_counter;
    uint32_t lcd_vpos;
    uint64_t lcd_line_accum;
    uint8_t eeprom[0x2000];
    uint8_t iic_data[4];
    int iic_data_index;
    uint16_t iic_address;

    uint32_t lcd_regs[0x400/4];
    uint16_t lcd_palette[0x400/2];
    uint32_t memcon[0x34/4];
    uint32_t usb_host[0x5c/4];
    uint32_t irq[0x18/4];
    uint32_t dma[0x7c/4];
    uint32_t clkpow[0x18/4];
    uint32_t uart0[0x2c/4];
    uint32_t uart1[0x2c/4];
    uint32_t pwm[0x44/4];
    uint64_t pwm_accum[5];
    uint64_t pwm_dec_cycles[5];
    uint64_t pwm_period_cycles[5];
    uint8_t pwm_clock_dirty;
    uint32_t usb_dev[0xbc/4];
    uint32_t watchdog[0x0c/4];
    uint32_t iic[0x10/4];
    uint32_t iis[0x14/4];
    uint16_t iis_fifo[2];
    unsigned iis_fifo_index;
    uint64_t iis_accum;
    int16_t *audio;
    uint64_t audio_frames;
    uint64_t audio_cap_frames;
    uint32_t audio_sample_rate_hz;
    uint32_t iis_cached_rate_hz;
    uint64_t iis_cached_period_cycles;
    uint8_t iis_clock_dirty;
    uint32_t gpio[0x60/4];
    uint32_t rtc[0x4c/4];
    uint32_t adc[0x08/4];
    uint32_t spi[0x18/4];
    uint32_t mmc[0x40/4];
    lcd_state_t lcd;
    gp32_smc_lines_t smc_lines;
    s3c2400_log_fn log;
    void *log_user;
};

static uint8_t *s3c2400_fastmem(void *user, uint32_t addr, size_t bytes, int write);
static uint32_t s3c2400_read32_io(void *user, uint32_t addr);
static uint32_t lcd_current_line_count(const s3c2400_t *s);
static uint32_t clk_fclk(const s3c2400_t *s, int reg);
static uint32_t clk_hclk(const s3c2400_t *s, int reg);
static uint32_t clk_run(const s3c2400_t *s, int reg);
static uint32_t clk_pclk(const s3c2400_t *s, int reg);

static void slog(s3c2400_t *s, const char *fmt, ...) {
    if (!s || !s->log) return;
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    s->log(s->log_user, buf);
}

s3c2400_t *s3c2400_create(size_t ram_size) {
    s3c2400_t *s = (s3c2400_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->ram_size = ram_size ? ram_size : RAM_DEFAULT_SIZE;
    s->ram = (uint8_t *)calloc(1, s->ram_size);
    s->smc = smc_create();
    if (!s->ram || !s->smc) { s3c2400_destroy(s); return NULL; }
    memset(s->bios, 0xff, sizeof(s->bios));
    memset(s->eeprom, 0xff, sizeof(s->eeprom));
    s->fb_w = 240; s->fb_h = 320;
    s3c2400_reset(s);
    return s;
}

void s3c2400_destroy(s3c2400_t *s) {
    if (!s) return;
    smc_destroy(s->smc);
    free(s->ram);
    free(s->audio);
    free(s);
}

void s3c2400_set_irq_sink(s3c2400_t *s, arm920t_t *cpu) { if (s) s->cpu_irq_sink = cpu; }
void s3c2400_set_log(s3c2400_t *s, s3c2400_log_fn fn, void *user) { if (s) { s->log = fn; s->log_user = user; } }

void s3c2400_reset(s3c2400_t *s) {
    if (!s) return;
    memset(s->ram, 0, s->ram_size);
    memset(s->lcd_regs, 0, sizeof(s->lcd_regs));
    memset(s->lcd_palette, 0, sizeof(s->lcd_palette));
    memset(s->memcon, 0, sizeof(s->memcon));
    memset(s->usb_host, 0, sizeof(s->usb_host));
    memset(s->irq, 0, sizeof(s->irq));
    memset(s->dma, 0, sizeof(s->dma));
    memset(s->clkpow, 0, sizeof(s->clkpow));
    memset(s->uart0, 0, sizeof(s->uart0));
    memset(s->uart1, 0, sizeof(s->uart1));
    memset(s->pwm, 0, sizeof(s->pwm));
    memset(s->pwm_accum, 0, sizeof(s->pwm_accum));
    memset(s->usb_dev, 0, sizeof(s->usb_dev));
    memset(s->watchdog, 0, sizeof(s->watchdog));
    memset(s->iic, 0, sizeof(s->iic));
    memset(s->iic_data, 0, sizeof(s->iic_data));
    s->iic_data_index = 0;
    s->iic_address = 0;
    memset(s->iis, 0, sizeof(s->iis));
    memset(s->iis_fifo, 0, sizeof(s->iis_fifo));
    s->iis_fifo_index = 0;
    s->iis_accum = 0;
    s->audio_frames = 0;
    s->audio_sample_rate_hz = 44100u;
    s->iis_cached_rate_hz = 44100u;
    s->iis_cached_period_cycles = 0;
    s->iis_clock_dirty = 1;
    s->pwm_clock_dirty = 1;
    memset(s->gpio, 0, sizeof(s->gpio));
    memset(s->rtc, 0, sizeof(s->rtc));
    memset(s->adc, 0, sizeof(s->adc));
    memset(s->spi, 0, sizeof(s->spi));
    memset(s->mmc, 0, sizeof(s->mmc));
    memset(&s->smc_lines, 0, sizeof(s->smc_lines));
    smc_reset(s->smc);
    s->fb_w = 240; s->fb_h = 320;
    memset(s->fb, 0, sizeof(s->fb));
    s->lcd_vpos = 0;
    s->lcd_line_accum = 0;
}

arm_bus_t s3c2400_get_bus(s3c2400_t *s) {
    arm_bus_t b;
    b.read8 = s3c2400_read8;
    b.read16 = s3c2400_read16;
    b.read32 = s3c2400_read32;
    b.read32_io = s3c2400_read32_io;
    b.write8 = s3c2400_write8;
    b.write16 = s3c2400_write16;
    b.write32 = s3c2400_write32;
    b.fastmem = s3c2400_fastmem;
    b.user = s;
    return b;
}

static int load_file_exact_or_less(uint8_t *dst, size_t cap, const char *path, char *err, size_t err_len) {
    if (gp32_zip_path_maybe(path)) {
        uint8_t *buf = NULL;
        size_t n = 0;
        char entry_name[260] = {0};
        static const char * const exts[] = { ".bin", ".rom", ".bios" };
        if (!gp32_zip_read_first_matching(path, exts, GP32_ARRAY_COUNT(exts), &buf, &n, entry_name, sizeof(entry_name), err, err_len)) return 0;
        if (n > cap) { if (err && err_len) snprintf(err, err_len, "%s:%s is too large", path, entry_name); free(buf); return 0; }
        memset(dst, 0xff, cap);
        memcpy(dst, buf, n);
        free(buf);
        return 1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { if (err && err_len) snprintf(err, err_len, "open %s: %s", path, strerror(errno)); return 0; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long n = ftell(f); rewind(f);
    if (n < 0 || (size_t)n > cap) { if (err && err_len) snprintf(err, err_len, "%s is too large", path); fclose(f); return 0; }
    memset(dst, 0xff, cap);
    if (fread(dst, 1, (size_t)n, f) != (size_t)n) { if (err && err_len) snprintf(err, err_len, "read %s failed", path); fclose(f); return 0; }
    fclose(f); return 1;
}
int s3c2400_load_bios(s3c2400_t *s, const char *path, char *err, size_t err_len) { return s && path && load_file_exact_or_less(s->bios, BIOS_SIZE, path, err, err_len); }
int s3c2400_load_bios_buffer(s3c2400_t *s, const uint8_t *data, size_t size, char *err, size_t err_len) {
    if (!s || !data || !size) { if (err && err_len) snprintf(err, err_len, "invalid BIOS buffer"); return 0; }
    if (size > BIOS_SIZE) { if (err && err_len) snprintf(err, err_len, "BIOS buffer is too large"); return 0; }
    memset(s->bios, 0xff, BIOS_SIZE);
    memcpy(s->bios, data, size);
    return 1;
}
int s3c2400_load_smartmedia(s3c2400_t *s, const char *path, char *err, size_t err_len) { return s && smc_load_file(s->smc, path, err, err_len); }
int s3c2400_load_smartmedia_buffer(s3c2400_t *s, const uint8_t *data, size_t size, char *err, size_t err_len) { return s && smc_load_buffer(s->smc, data, size, err, err_len); }
int s3c2400_save_smartmedia(s3c2400_t *s, const char *path, char *err, size_t err_len) { return s && smc_save_file(s->smc, path, err, err_len); }

int s3c2400_load_ram_image(s3c2400_t *s, uint32_t addr, const uint8_t *data, size_t size, char *err, size_t err_len) {
    if (!s || !data) return 0;
    if (addr < RAM_BASE) { if (err && err_len) snprintf(err, err_len, "RAM image address below SDRAM"); return 0; }
    uint32_t off = addr - RAM_BASE;
    if ((size_t)off > s->ram_size || size > s->ram_size - (size_t)off) {
        if (err && err_len) snprintf(err, err_len, "RAM image too large for SDRAM");
        return 0;
    }
    memcpy(&s->ram[off], data, size);
    return 1;
}

size_t s3c2400_ram_size(const s3c2400_t *s) { return s ? s->ram_size : 0u; }

void s3c2400_set_buttons(s3c2400_t *s, uint32_t mask) { if (s) s->buttons = mask; }

static uint8_t expand6(uint32_t v) {
    v &= 0x3fu;
    return (uint8_t)((v << 2) | (v >> 4));
}

static uint32_t color_lcd5551(uint16_t data) {
    /* S3C2400 TFT palette entries and the GP32 16-bpp SDK color value use
       5:5:5:I: bits 15..11 red, 10..6 green, 5..1 blue, bit 0 common
       intensity.  The intensity bit is the shared LSB of each channel. */
    uint32_t i = data & 1u;
    uint8_t r = expand6((((uint32_t)data >> 11) & 0x1fu) << 1 | i);
    uint8_t g = expand6((((uint32_t)data >> 6) & 0x1fu) << 1 | i);
    uint8_t b = expand6((((uint32_t)data >> 1) & 0x1fu) << 1 | i);
    return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint8_t *ram_ptr(s3c2400_t *s, uint32_t addr, size_t bytes) {
    if (addr < RAM_BASE) return NULL;
    uint32_t off = addr - RAM_BASE;
    if ((size_t)off + bytes > s->ram_size) return NULL;
    return &s->ram[off];
}

static uint8_t *s3c2400_fastmem(void *user, uint32_t addr, size_t bytes, int write) {
    s3c2400_t *s = (s3c2400_t *)user;
    if (!s) return NULL;
    if (!write && (uint64_t)addr + bytes <= BIOS_SIZE) return &s->bios[addr];
    return ram_ptr(s, addr, bytes);
}

static uint32_t reg_array_read(uint32_t *a, size_t bytes, uint32_t off) {
    if ((size_t)off + 4 > bytes) return 0xffffffffu;
    return a[off >> 2];
}
static void reg_array_write(uint32_t *a, size_t bytes, uint32_t off, uint32_t value, uint32_t mask) {
    if ((size_t)off + 4 > bytes) return;
    uint32_t *r = &a[off >> 2];
    *r = (*r & ~mask) | (value & mask);
}

static uint32_t lcd_palette_read32(const s3c2400_t *s, uint32_t off) {
    if (!s || off >= 0x400u) return 0xffffffffu;
    uint32_t i = off >> 2;
    if (i >= 256u) return 0xffffffffu;
    return (uint32_t)s->lcd_palette[i];
}

static void lcd_palette_write32(s3c2400_t *s, uint32_t off, uint32_t value, uint32_t mask) {
    if (!s || off >= 0x400u) return;
    uint32_t i = off >> 2;
    if (i >= 256u) return;
    uint16_t cur = s->lcd_palette[i];
    if ((off & 2u) == 0u) {
        if (mask & 0x000000ffu) cur = (uint16_t)((cur & 0xff00u) | (value & 0x00ffu));
        if (mask & 0x0000ff00u) cur = (uint16_t)((cur & 0x00ffu) | (value & 0xff00u));
    } else {
        /* DATA[31:16] is invalid for S3C2400 palette entries. */
    }
    s->lcd_palette[i] = cur;
}

static uint32_t button_in0(const s3c2400_t *s) {
    uint32_t v = 0xffffu;
    if (s->buttons & GP32_BUTTON_LEFT)  v &= ~0x0100u;
    if (s->buttons & GP32_BUTTON_DOWN)  v &= ~0x0200u;
    if (s->buttons & GP32_BUTTON_RIGHT) v &= ~0x0400u;
    if (s->buttons & GP32_BUTTON_UP)    v &= ~0x0800u;
    if (s->buttons & GP32_BUTTON_L)     v &= ~0x1000u;
    if (s->buttons & GP32_BUTTON_B)     v &= ~0x2000u;
    if (s->buttons & GP32_BUTTON_A)     v &= ~0x4000u;
    if (s->buttons & GP32_BUTTON_R)     v &= ~0x8000u;
    return v;
}
static uint32_t button_in1(const s3c2400_t *s) {
    uint32_t v = 0xffffu;
    if (s->buttons & GP32_BUTTON_START)  v &= ~0x0040u;
    if (s->buttons & GP32_BUTTON_SELECT) v &= ~0x0080u;
    return v;
}

static void smc_lines_reset(gp32_smc_lines_t *m) { memset(m, 0, sizeof(*m)); }
static void gp32_smc_write(s3c2400_t *s, uint8_t data) {
    gp32_smc_lines_t *m = &s->smc_lines;
    if (m->chip && !m->read) {
        if (m->cmd_latch) smc_command_w(s->smc, data);
        else if (m->add_latch) smc_address_w(s->smc, data);
        else smc_data_w(s->smc, data);
    }
}
static uint8_t gp32_smc_read(s3c2400_t *s) { return smc_data_r(s->smc); }
static void gp32_smc_update(s3c2400_t *s) {
    gp32_smc_lines_t *m = &s->smc_lines;
    if (!m->chip) { smc_lines_reset(m); return; }
    if (m->do_write && !m->read) gp32_smc_write(s, m->datatx);
    else if (!m->do_write && m->do_read && m->read && !m->cmd_latch && !m->add_latch) m->datarx = gp32_smc_read(s);
}

static void check_irq(s3c2400_t *s) {
    if (!s->cpu_irq_sink) return;
    uint32_t pending = s->irq[0] & ~s->irq[2]; /* SRCPND masked by INTMSK */
    if (pending) {
        uint32_t t = pending, n = 0;
        while (!(t & 1u)) { n++; t >>= 1; }
        s->irq[4] |= (1u << n);
        s->irq[5] = n;
        arm920t_set_irq(s->cpu_irq_sink, 1);
    } else arm920t_set_irq(s->cpu_irq_sink, 0);
}
static void request_irq(s3c2400_t *s, unsigned n) {
    if (n >= 32) return;
    s->irq[0] |= (1u << n);
    check_irq(s);
}

static void iis_fifo_write16(s3c2400_t *s, uint16_t sample);
static uint32_t iis_frame_rate_hz(const s3c2400_t *s);
static uint64_t iis_period_cpu_cycles(const s3c2400_t *s);
static uint32_t iis_dma_transfers_per_frame(const s3c2400_t *s);

static void dma_reload(s3c2400_t *s, int ch) {
    uint32_t *r = &s->dma[ch << 3];
    r[3] = (r[3] & ~0x000fffffu) | (r[2] & 0x000fffffu);
    r[4] = (r[4] & ~0x1fffffffu) | (r[0] & 0x1fffffffu);
    r[5] = (r[5] & ~0x1fffffffu) | (r[1] & 0x1fffffffu);
}

static uint32_t dma_iis_fast_trigger_count(s3c2400_t *s, uint32_t *r, uint32_t requests) {
    uint32_t tc = r[3] & 0x000fffffu;
    uint32_t src = r[4] & 0x1fffffffu;
    uint32_t dst = r[5] & 0x1fffffffu;
    unsigned dsz = GP32_BITS(r[2],21,20);
    int inc_src = GP32_BIT(r[0],29) == 0;
    int inc_dst = GP32_BIT(r[1],29) == 0;
    int service = GP32_BIT(r[2],26);
    if (!tc || !requests || inc_dst || dst != 0x15508010u || dsz == 0u) return 0;
    uint32_t completed = 0;
    while (tc && completed < requests) {
        tc--;
        if (dsz == 1u) {
            uint8_t *rp = ram_ptr(s, src, 2);
            uint16_t v = rp ? (uint16_t)(rp[0] | ((uint16_t)rp[1] << 8)) : s3c2400_read16(s, src);
            iis_fifo_write16(s, v);
            if (inc_src) src += 2u;
        } else {
            uint8_t *rp = ram_ptr(s, src, 4);
            uint32_t v = rp ? gp32_ld32le(rp) : s3c2400_read32(s, src);
            iis_fifo_write16(s, (uint16_t)v);
            iis_fifo_write16(s, (uint16_t)(v >> 16));
            if (inc_src) src += 4u;
        }
        completed++;
        if (!service) {
            /* Single-service mode performs one transfer per hardware request. */
        } else {
            while (tc) {
                tc--;
                if (dsz == 1u) {
                    uint8_t *rp = ram_ptr(s, src, 2);
                    uint16_t v = rp ? (uint16_t)(rp[0] | ((uint16_t)rp[1] << 8)) : s3c2400_read16(s, src);
                    iis_fifo_write16(s, v);
                    if (inc_src) src += 2u;
                } else {
                    uint8_t *rp = ram_ptr(s, src, 4);
                    uint32_t v = rp ? gp32_ld32le(rp) : s3c2400_read32(s, src);
                    iis_fifo_write16(s, (uint16_t)v);
                    iis_fifo_write16(s, (uint16_t)(v >> 16));
                    if (inc_src) src += 4u;
                }
            }
            break;
        }
    }
    r[4] = (r[4] & ~0x1fffffffu) | src;
    r[5] = (r[5] & ~0x1fffffffu) | dst;
    r[3] = (r[3] & ~0x000fffffu) | tc;
    if (!tc) {
        if (GP32_BIT(r[2],22)) r[6] &= ~(1u << 1);
        else dma_reload(s, 2);
        if (GP32_BIT(r[2],28)) request_irq(s, INT_DMA2);
    }
    return completed;
}

static int dma_iis_fast_trigger(s3c2400_t *s, uint32_t *r) {
    return dma_iis_fast_trigger_count(s, r, 1u) != 0u;
}

static uint32_t dma_request_iis_fast_count(s3c2400_t *s, uint32_t requests) {
    uint32_t *r = &s->dma[2 << 3];
    if (!((r[6] & 2u) && GP32_BIT(r[2], 23) && GP32_BITS(r[2], 25, 24) == 0u)) return 0;
    return dma_iis_fast_trigger_count(s, r, requests);
}

static void dma_trigger(s3c2400_t *s, int ch) {
    uint32_t *r = &s->dma[ch << 3];
    if (ch == 2 && dma_iis_fast_trigger(s, r)) return;
    uint32_t tc = r[3] & 0x000fffffu;
    uint32_t src = r[4] & 0x1fffffffu;
    uint32_t dst = r[5] & 0x1fffffffu;
    unsigned dsz = GP32_BITS(r[2],21,20);
    int inc_src = GP32_BIT(r[0],29) == 0;
    int inc_dst = GP32_BIT(r[1],29) == 0;
    int service = GP32_BIT(r[2],26);
    while (tc) {
        tc--;
        if (dsz == 0) { uint8_t v = s3c2400_read8(s, src); s3c2400_write8(s, dst, v); }
        else if (dsz == 1) { uint16_t v = s3c2400_read16(s, src); s3c2400_write16(s, dst, v); }
        else { uint32_t v = s3c2400_read32(s, src); s3c2400_write32(s, dst, v); }
        if (inc_src) src += 1u << dsz;
        if (inc_dst) dst += 1u << dsz;
        if (!service) break;
    }
    r[4] = (r[4] & ~0x1fffffffu) | src;
    r[5] = (r[5] & ~0x1fffffffu) | dst;
    r[3] = (r[3] & ~0x000fffffu) | tc;
    if (!tc) {
        if (GP32_BIT(r[2],22)) r[6] &= ~(1u << 1); /* reload bit set means no reload in MAME driver */
        else dma_reload(s, ch);
        if (GP32_BIT(r[2],28)) request_irq(s, INT_DMA0 + (unsigned)ch);
    }
}

static void dma_start(s3c2400_t *s, int ch) {
    uint32_t *r = &s->dma[ch << 3];
    dma_reload(s, ch);
    if (!GP32_BIT(r[2], 23)) dma_trigger(s, ch); /* software request only */
}


static void pwm_refresh_clock_cache(s3c2400_t *s) {
    static const uint32_t mux_table[4] = { 2u, 4u, 8u, 16u };
    static const unsigned prescaler_shift[5] = { 0, 0, 8, 8, 8 };
    static const unsigned mux_shift[5] = { 0, 4, 8, 12, 16 };
    if (!s || !s->pwm_clock_dirty) return;
    uint32_t runclk = clk_run(s, MPLLCON);
    uint32_t pclk = clk_pclk(s, MPLLCON);
    if (!runclk) runclk = 40000000u;
    if (!pclk) pclk = runclk;
    for (unsigned t = 0; t < 5u; ++t) {
        uint32_t prescaler = GP32_BITS(s->pwm[0], prescaler_shift[t] + 7u, prescaler_shift[t]);
        uint32_t mux = GP32_BITS(s->pwm[1], mux_shift[t] + 3u, mux_shift[t]);
        uint32_t div = mux < 4u ? mux_table[mux] : mux_table[3];
        uint32_t cnt = s->pwm[3u + t * 3u] & 0xffffu;
        uint32_t cmp = (t == 4u) ? 0u : (s->pwm[4u + t * 3u] & 0xffffu);
        if (cnt == 0u) cnt = 0x10000u;
        uint32_t interval = (cnt > cmp) ? (cnt - cmp + 1u) : 1u;
        uint64_t base_num = (uint64_t)runclk * (uint64_t)(prescaler + 1u) * (uint64_t)div;
        uint64_t dec_cycles = (base_num + (uint64_t)pclk - 1u) / (uint64_t)pclk;
        uint64_t period = (base_num * (uint64_t)interval) / (uint64_t)pclk;
        s->pwm_dec_cycles[t] = dec_cycles ? dec_cycles : 1u;
        s->pwm_period_cycles[t] = period ? period : 1u;
    }
    s->pwm_clock_dirty = 0;
}

static uint32_t pwm_current_count(s3c2400_t *s, unsigned t) {
    static const unsigned start_mask[5] = { 0x000001u, 0x000100u, 0x001000u, 0x010000u, 0x100000u };
    if (!s || t >= 5u) return 0u;
    uint32_t cnt = s->pwm[3u + t * 3u] & 0xffffu;
    if (cnt == 0u) cnt = 0x10000u;
    if (!(s->pwm[2] & start_mask[t])) return cnt;
    pwm_refresh_clock_cache(s);
    uint64_t dec_cycles = s->pwm_dec_cycles[t] ? s->pwm_dec_cycles[t] : 1u;
    uint64_t elapsed = s->pwm_accum[t] / dec_cycles;
    if (elapsed >= cnt) return 0u;
    return (uint32_t)(cnt - elapsed);
}

static uint32_t io_read32(s3c2400_t *s, uint32_t addr) {
    uint32_t off;
    if (addr >= 0x14000000u && addr <= 0x1400003bu) return reg_array_read(s->memcon, sizeof(s->memcon), addr - 0x14000000u);
    if (addr >= 0x14200000u && addr <= 0x1420005bu) return reg_array_read(s->usb_host, sizeof(s->usb_host), addr - 0x14200000u);
    if (addr >= 0x14400000u && addr <= 0x14400017u) return reg_array_read(s->irq, sizeof(s->irq), addr - 0x14400000u);
    if (addr >= 0x14600000u && addr <= 0x1460007bu) return reg_array_read(s->dma, sizeof(s->dma), addr - 0x14600000u);
    if (addr >= 0x14800000u && addr <= 0x14800017u) return reg_array_read(s->clkpow, sizeof(s->clkpow), addr - 0x14800000u);
    if (addr >= 0x14a00000u && addr <= 0x14a003ffu) {
        off = addr - 0x14a00000u;
        uint32_t data = reg_array_read(s->lcd_regs, sizeof(s->lcd_regs), off);
        if (off == 0) {
            uint32_t linecnt = (s->lcd_regs[0] & 1u) ? lcd_current_line_count(s) : 0u;
            data = (data & ~0xfffc0000u) | ((linecnt & 0x3ffu) << 18);
        }
        return data;
    }
    if (addr >= 0x14a00400u && addr <= 0x14a007ffu) return lcd_palette_read32(s, addr - 0x14a00400u);
    if (addr >= 0x15000000u && addr <= 0x1500002bu) {
        off = addr - 0x15000000u;
        uint32_t data = reg_array_read(s->uart0, sizeof(s->uart0), off);
        if (off == 0x10u) data = (data & ~0x06u) | 0x06u;
        return data;
    }
    if (addr >= 0x15004000u && addr <= 0x1500402bu) {
        off = addr - 0x15004000u;
        uint32_t data = reg_array_read(s->uart1, sizeof(s->uart1), off);
        if (off == 0x10u) data = (data & ~0x06u) | 0x06u;
        return data;
    }
    if (addr >= 0x15100000u && addr <= 0x15100043u) {
        off = addr - 0x15100000u;
        if (off == 0x14u) return pwm_current_count(s, 0u);
        if (off == 0x20u) return pwm_current_count(s, 1u);
        if (off == 0x2cu) return pwm_current_count(s, 2u);
        if (off == 0x38u) return pwm_current_count(s, 3u);
        if (off == 0x40u) return pwm_current_count(s, 4u);
        return reg_array_read(s->pwm, sizeof(s->pwm), off);
    }
    if (addr >= 0x15200140u && addr <= 0x152001fbu) return reg_array_read(s->usb_dev, sizeof(s->usb_dev), addr - 0x15200140u);
    if (addr >= 0x15300000u && addr <= 0x1530000bu) return reg_array_read(s->watchdog, sizeof(s->watchdog), addr - 0x15300000u);
    if (addr >= 0x15400000u && addr <= 0x1540000fu) { uint32_t data = reg_array_read(s->iic, sizeof(s->iic), addr - 0x15400000u); if ((addr & 0xff) == 0x04) data &= ~0xfu; return data; }
    if (addr >= 0x15508000u && addr <= 0x15508013u) return reg_array_read(s->iis, sizeof(s->iis), addr - 0x15508000u);
    if (addr >= 0x15600000u && addr <= 0x1560005bu) {
        off = addr - 0x15600000u;
        uint32_t data = reg_array_read(s->gpio, sizeof(s->gpio), off);
        switch (off) {
        case 0x08: data = (data & ~1u) | (!s->smc_lines.read ? 1u : 0u); break;
        case 0x0c: data = (data & ~0xffffu) | s->smc_lines.datarx | (button_in0(s) & 0xff00u); break;
        case 0x24:
            data &= ~0x3c0u;
            if (!s->smc_lines.busy) data |= 0x200u;
            if (!s->smc_lines.do_read) data |= 0x100u;
            if (!s->smc_lines.chip) data |= 0x080u;
            if (!smc_is_protected(s->smc)) data |= 0x040u;
            break;
        case 0x30:
            data &= ~0xfcu;
            if (s->smc_lines.cmd_latch) data |= 0x20u;
            if (s->smc_lines.add_latch) data |= 0x10u;
            if (!s->smc_lines.do_write) data |= 0x08u;
            if (!smc_is_present(s->smc)) data |= 0x04u;
            data |= button_in1(s) & 0xc0u;
            break;
        }
        return data;
    }
    if (addr >= 0x15700040u && addr <= 0x1570008bu) return reg_array_read(s->rtc, sizeof(s->rtc), addr - 0x15700040u);
    if (addr >= 0x15800000u && addr <= 0x15800007u) return reg_array_read(s->adc, sizeof(s->adc), addr - 0x15800000u);
    if (addr >= 0x15900000u && addr <= 0x15900017u) return reg_array_read(s->spi, sizeof(s->spi), addr - 0x15900000u);
    if (addr >= 0x15a00000u && addr <= 0x15a0003fu) return reg_array_read(s->mmc, sizeof(s->mmc), addr - 0x15a00000u);
    return 0xffffffffu;
}

static uint32_t s3c2400_read32_io(void *user, uint32_t addr) {
    s3c2400_t *s = (s3c2400_t *)user;
    switch (addr) {
    case 0x14a00000u: {
        uint32_t data = s->lcd_regs[0];
        uint32_t linecnt = (data & 1u) ? lcd_current_line_count(s) : 0u;
        return (data & ~0xfffc0000u) | ((linecnt & 0x3ffu) << 18);
    }
    case 0x15100014u: return pwm_current_count(s, 0u);
    case 0x15100020u: return pwm_current_count(s, 1u);
    case 0x1510002cu: return pwm_current_count(s, 2u);
    case 0x15100038u: return pwm_current_count(s, 3u);
    case 0x15100040u: return pwm_current_count(s, 4u);
    case 0x15400004u: return s->iic[1] & ~0x0fu;
    case 0x15600008u: return (s->gpio[0x08u >> 2] & ~1u) | (!s->smc_lines.read ? 1u : 0u);
    case 0x1560000cu: return (s->gpio[0x0cu >> 2] & ~0xffffu) | s->smc_lines.datarx | (button_in0(s) & 0xff00u);
    case 0x15600024u: {
        uint32_t data = s->gpio[0x24u >> 2] & ~0x3c0u;
        if (!s->smc_lines.busy) data |= 0x200u;
        if (!s->smc_lines.do_read) data |= 0x100u;
        if (!s->smc_lines.chip) data |= 0x080u;
        if (!smc_is_protected(s->smc)) data |= 0x040u;
        return data;
    }
    case 0x15600030u: {
        uint32_t data = s->gpio[0x30u >> 2] & ~0xfcu;
        if (s->smc_lines.cmd_latch) data |= 0x20u;
        if (s->smc_lines.add_latch) data |= 0x10u;
        if (!s->smc_lines.do_write) data |= 0x08u;
        if (!smc_is_present(s->smc)) data |= 0x04u;
        return data | (button_in1(s) & 0xc0u);
    }
    default:
        return io_read32(s, addr);
    }
}

static void iic_step(s3c2400_t *s) {
    unsigned mode_selection = GP32_BITS(s->iic[1], 7, 6);
    switch (mode_selection) {
    case 2: /* master receive */
        if (s->iic_data_index == 0) {
            /* first byte is the device address already in IICDS */
        } else {
            uint8_t data = s->eeprom[s->iic_address & 0x1fffu];
            s->iic[3] = (s->iic[3] & ~0xffu) | data;
            s->iic_address = (uint16_t)((s->iic_address + 1u) & 0x1fffu);
        }
        s->iic_data_index++;
        break;
    case 3: { /* master transmit */
        uint8_t data = (uint8_t)(s->iic[3] & 0xffu);
        if (s->iic_data_index < 4) s->iic_data[s->iic_data_index] = data;
        s->iic_data_index++;
        if (s->iic_data_index == 3) s->iic_address = (uint16_t)(((uint16_t)s->iic_data[1] << 8) | s->iic_data[2]);
        else if (s->iic_data_index >= 4 && s->iic_data[0] == 0xa0) {
            s->eeprom[s->iic_address & 0x1fffu] = data;
            s->iic_address = (uint16_t)((s->iic_address + 1u) & 0x1fffu);
        }
        break;
    }
    default:
        break;
    }
    s->iic[0] |= 0x10u; /* interrupt pending flag */
    if (s->iic[0] & 0x20u) request_irq(s, INT_IIC);
}

static void iic_start(s3c2400_t *s) {
    s->iic_data_index = 0;
    iic_step(s);
}

static void io_write32(s3c2400_t *s, uint32_t addr, uint32_t value, uint32_t mask) {
    uint32_t off;
    if (addr >= 0x14000000u && addr <= 0x1400003bu) { reg_array_write(s->memcon,sizeof(s->memcon),addr-0x14000000u,value,mask); return; }
    if (addr >= 0x14200000u && addr <= 0x1420005bu) { reg_array_write(s->usb_host,sizeof(s->usb_host),addr-0x14200000u,value,mask); return; }
    if (addr >= 0x14400000u && addr <= 0x14400017u) {
        off = addr - 0x14400000u; uint32_t old = reg_array_read(s->irq, sizeof(s->irq), off); reg_array_write(s->irq,sizeof(s->irq),off,value,mask);
        if (off == 0x00) s->irq[0] = old & ~value;
        else if (off == 0x10) s->irq[4] = old & ~value;
        check_irq(s);
        return;
    }
    if (addr >= 0x14600000u && addr <= 0x1460007bu) {
        off=addr-0x14600000u; uint32_t old=reg_array_read(s->dma,sizeof(s->dma),off); reg_array_write(s->dma,sizeof(s->dma),off,value,mask);
        /* DCON[22] is the reload-off option. It must not disable an
         * already-running channel when software writes DCON: hardware turns
         * the request off only when the current transfer count reaches zero.
         * Several BIOS-driven games rewrite DCON with reload-off while the
         * final audio DMA block is still draining. Clearing DMASKTRIG here
         * drops the terminal-count DMA interrupt and can stop streamed music. */
        if (off==0x18u||off==0x38u||off==0x58u||off==0x78u) {
            int ch = (int)(off >> 5);
            uint32_t *dr = &s->dma[ch << 3];
            uint32_t now = dr[6];
            if ((mask & 4u) && (now & 4u)) {
                /* DMASKTRIG[2] STOP forces the channel off after the current
                 * atomic transfer. DMA transfers are modeled atomically here,
                 * so complete the stop immediately and clear the current
                 * transfer registers as the hardware documents. */
                dr[3] = 0;
                dr[4] = 0;
                dr[5] = 0;
                dr[6] &= ~2u;
            } else if (((old ^ now) & 2u) && (now & 2u)) {
                if (!GP32_BIT(dr[2], 23)) dr[6] &= ~1u; /* SW_TRIG is self-clearing when accepted. */
                dma_start(s, ch);
            }
        }
        return;
    }
    if (addr >= 0x14800000u && addr <= 0x14800017u) { reg_array_write(s->clkpow,sizeof(s->clkpow),addr-0x14800000u,value,mask); s->iis_clock_dirty = 1; s->pwm_clock_dirty = 1; return; }
    if (addr >= 0x14a00000u && addr <= 0x14a003ffu) {
        off = addr - 0x14a00000u;
        uint32_t old = reg_array_read(s->lcd_regs, sizeof(s->lcd_regs), off);
        reg_array_write(s->lcd_regs, sizeof(s->lcd_regs), off, value, mask);
        uint32_t now = reg_array_read(s->lcd_regs, sizeof(s->lcd_regs), off);
        (void)old;
        (void)now;
        if (off == 0u) s3c2400_render_lcd(s);
        return;
    }
    if (addr >= 0x14a00400u && addr <= 0x14a007ffu) { lcd_palette_write32(s, addr - 0x14a00400u, value, mask); return; }
    if (addr >= 0x15000000u && addr <= 0x1500002bu) { reg_array_write(s->uart0,sizeof(s->uart0),addr-0x15000000u,value,mask); return; }
    if (addr >= 0x15004000u && addr <= 0x1500402bu) { reg_array_write(s->uart1,sizeof(s->uart1),addr-0x15004000u,value,mask); return; }
    if (addr >= 0x15100000u && addr <= 0x15100043u) { reg_array_write(s->pwm,sizeof(s->pwm),addr-0x15100000u,value,mask); s->pwm_clock_dirty = 1; return; }
    if (addr >= 0x15200140u && addr <= 0x152001fbu) { reg_array_write(s->usb_dev,sizeof(s->usb_dev),addr-0x15200140u,value,mask); return; }
    if (addr >= 0x15300000u && addr <= 0x1530000bu) { reg_array_write(s->watchdog,sizeof(s->watchdog),addr-0x15300000u,value,mask); return; }
    if (addr >= 0x15400000u && addr <= 0x1540000fu) {
        off = addr - 0x15400000u;
        uint32_t old = reg_array_read(s->iic, sizeof(s->iic), off);
        reg_array_write(s->iic,sizeof(s->iic),off,value,mask);
        uint32_t now = reg_array_read(s->iic, sizeof(s->iic), off);
        if (off == 0x00u) {
            /* IICCON bit 4 is the interrupt-pending/ack bit.  Only advance
             * the EEPROM transaction when that bit is actually acknowledged.
             * Byte/halfword writes to other lanes used to look like bit 4 was
             * clear and could trigger millions of bogus iic_step() calls in
             * BIOS-driven games such as Astonishia Story R. */
            if ((mask & 0x10u) && (old & 0x10u) && !(now & 0x10u) && (s->iic[1] & 0x20u)) iic_step(s);
        } else if (off == 0x04u) {
            if (mask & 0x20u) {
                if (!(old & 0x20u) && (now & 0x20u)) iic_start(s);
                else if (!(now & 0x20u)) s->iic_data_index = 0;
            }
        }
        return;
    }
    if (addr >= 0x15508000u && addr <= 0x15508013u) {
        off = addr - 0x15508000u;
        uint32_t old = reg_array_read(s->iis, sizeof(s->iis), off);
        reg_array_write(s->iis, sizeof(s->iis), off, value, mask);
        if (off == 0x00u && ((old ^ s->iis[0]) & 1u)) s->iis_accum = 0;
        if (off == 0x04u || off == 0x08u) {
            s->iis_clock_dirty = 1;
            s->audio_sample_rate_hz = iis_frame_rate_hz(s);
        }
        if (off == 0x10u) {
            if (mask & 0xffff0000u) iis_fifo_write16(s, (uint16_t)(value >> 16));
            if (mask & 0x0000ffffu) iis_fifo_write16(s, (uint16_t)value);
        }
        return;
    }
    if (addr >= 0x15600000u && addr <= 0x1560005bu) {
        off=addr-0x15600000u; reg_array_write(s->gpio,sizeof(s->gpio),off,value,mask);
        switch(off){
        case 0x08: s->smc_lines.read = ((s->gpio[off>>2] & 1u) == 0); gp32_smc_update(s); break;
        case 0x0c: s->smc_lines.datatx = (uint8_t)(s->gpio[off>>2] & 0xffu); break;
        case 0x24: s->smc_lines.do_read=((s->gpio[off>>2]&0x100u)==0); s->smc_lines.chip=((s->gpio[off>>2]&0x80u)==0); s->smc_lines.wp=((s->gpio[off>>2]&0x40u)==0); gp32_smc_update(s); break;
        case 0x30: s->smc_lines.cmd_latch=((s->gpio[off>>2]&0x20u)!=0); s->smc_lines.add_latch=((s->gpio[off>>2]&0x10u)!=0); s->smc_lines.do_write=((s->gpio[off>>2]&0x08u)==0); gp32_smc_update(s); break;
        }
        return;
    }
    if (addr >= 0x15700040u && addr <= 0x1570008bu) { reg_array_write(s->rtc,sizeof(s->rtc),addr-0x15700040u,value,mask); return; }
    if (addr >= 0x15800000u && addr <= 0x15800007u) { reg_array_write(s->adc,sizeof(s->adc),addr-0x15800000u,value,mask); return; }
    if (addr >= 0x15900000u && addr <= 0x15900017u) { reg_array_write(s->spi,sizeof(s->spi),addr-0x15900000u,value,mask); return; }
    if (addr >= 0x15a00000u && addr <= 0x15a0003fu) { reg_array_write(s->mmc,sizeof(s->mmc),addr-0x15a00000u,value,mask); return; }
    slog(s, "unmapped write32 %08" PRIx32 "=%08" PRIx32, addr, value);
}

uint8_t s3c2400_read8(void *user, uint32_t addr) {
    s3c2400_t *s=(s3c2400_t*)user;
    if (addr < BIOS_SIZE) return s->bios[addr];
    uint8_t *p=ram_ptr(s,addr,1); if(p) return *p;
    uint32_t a=addr&~3u, v=io_read32(s,a); return (uint8_t)(v >> ((addr&3u)*8u));
}
uint16_t s3c2400_read16(void *user, uint32_t addr) { uint16_t v=(uint16_t)s3c2400_read8(user,addr); v|=(uint16_t)s3c2400_read8(user,addr+1u)<<8; return v; }
uint32_t s3c2400_read32(void *user, uint32_t addr) {
    s3c2400_t *s=(s3c2400_t*)user;
    if (addr+3u < BIOS_SIZE) return gp32_ld32le(&s->bios[addr]);
    uint8_t *p=ram_ptr(s,addr,4); if(p) return gp32_ld32le(p);
    if ((addr & 3u)==0) {
        if (addr == 0x14a00000u) { uint32_t data = s->lcd_regs[0]; uint32_t linecnt = (data & 1u) ? lcd_current_line_count(s) : 0u; return (data & ~0xfffc0000u) | ((linecnt & 0x3ffu) << 18); }
        return io_read32(s,addr);
    }
    return (uint32_t)s3c2400_read8(user,addr) | ((uint32_t)s3c2400_read8(user,addr+1)<<8) | ((uint32_t)s3c2400_read8(user,addr+2)<<16) | ((uint32_t)s3c2400_read8(user,addr+3)<<24);
}
void s3c2400_write8(void *user, uint32_t addr, uint8_t value) {
    s3c2400_t *s=(s3c2400_t*)user;
    uint8_t *p=ram_ptr(s,addr,1); if(p){*p=value;return;}
    uint32_t lane=(addr&3u)*8u, mask=0xffu<<lane, v=(uint32_t)value<<lane; io_write32(s,addr&~3u,v,mask);
}
void s3c2400_write16(void *user, uint32_t addr, uint16_t value) {
    s3c2400_t *s = (s3c2400_t *)user;
    uint8_t *p = ram_ptr(s, addr, 2);
    if (p) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); return; }
    if ((addr & 1u) == 0u) {
        uint32_t lane = (addr & 3u) * 8u;
        uint32_t mask = 0xffffu << lane;
        uint32_t v = (uint32_t)value << lane;
        io_write32(s, addr & ~3u, v, mask);
        return;
    }
    s3c2400_write8(user, addr, (uint8_t)value);
    s3c2400_write8(user, addr + 1u, (uint8_t)(value >> 8));
}
void s3c2400_write32(void *user, uint32_t addr, uint32_t value) {
    s3c2400_t *s=(s3c2400_t*)user; uint8_t *p=ram_ptr(s,addr,4); if(p){gp32_st32le(p,value);return;} if((addr&3u)==0) io_write32(s,addr,value,0xffffffffu); else { s3c2400_write8(user,addr,(uint8_t)value); s3c2400_write8(user,addr+1,(uint8_t)(value>>8)); s3c2400_write8(user,addr+2,(uint8_t)(value>>16)); s3c2400_write8(user,addr+3,(uint8_t)(value>>24)); } }

static void lcd_dma_reload(s3c2400_t *s) {
    s->lcd.vramaddr_cur = s->lcd_regs[5] << 1;
    s->lcd.vramaddr_max = ((s->lcd_regs[5] & 0xffe00000u) | s->lcd_regs[6]) << 1;
    s->lcd.offsize = GP32_BITS(s->lcd_regs[7],21,11);
    s->lcd.pagewidth_cur = 0;
    s->lcd.pagewidth_max = GP32_BITS(s->lcd_regs[7],10,0);
}
static void lcd_dma_init(s3c2400_t *s) {
    lcd_dma_reload(s);
    s->lcd.bppmode = GP32_BITS(s->lcd_regs[0],4,1);
    s->lcd.bswp = GP32_BIT(s->lcd_regs[4],1);
    s->lcd.hwswp = GP32_BIT(s->lcd_regs[4],0);
    s->lcd.lineval = GP32_BITS(s->lcd_regs[1],23,14);
    s->lcd.hozval = GP32_BITS(s->lcd_regs[2],18,8);
    s->lcd.width = s->lcd.hozval + 1u;
    s->lcd.height = s->lcd.lineval + 1u;
    if (s->lcd.width > 240) s->lcd.width = 240;
    if (s->lcd.height > 320) s->lcd.height = 320;
}
static uint32_t lcd_dma_read(s3c2400_t *s) {
    uint8_t data[4] = {0,0,0,0};
    for(int i=0;i<2;i++){
        uint8_t *v=ram_ptr(s,s->lcd.vramaddr_cur,2);
        if(v){data[i*2]=v[0];data[i*2+1]=v[1];}
        s->lcd.vramaddr_cur+=2; s->lcd.pagewidth_cur++;
        if(s->lcd.pagewidth_cur>=s->lcd.pagewidth_max && s->lcd.pagewidth_max){s->lcd.vramaddr_cur += s->lcd.offsize<<1; s->lcd.pagewidth_cur=0;}
    }
    if(!s->lcd.hwswp) return !s->lcd.bswp ? ((uint32_t)data[3]<<24)|((uint32_t)data[2]<<16)|((uint32_t)data[1]<<8)|data[0] : ((uint32_t)data[0]<<24)|((uint32_t)data[1]<<16)|((uint32_t)data[2]<<8)|data[3];
    return !s->lcd.bswp ? ((uint32_t)data[1]<<24)|((uint32_t)data[0]<<16)|((uint32_t)data[3]<<8)|data[2] : ((uint32_t)data[2]<<24)|((uint32_t)data[3]<<16)|((uint32_t)data[0]<<8)|data[1];
}
static uint32_t pal(s3c2400_t *s, uint32_t i){ return color_lcd5551(s->lcd_palette[i & 0xffu]); }

void s3c2400_render_lcd(s3c2400_t *s) {
    if (!s || !(s->lcd_regs[0] & 1u)) return;
    lcd_dma_init(s);
    uint32_t w=s->lcd.width?s->lcd.width:240, h=s->lcd.height?s->lcd.height:320;
    memset(s->fb,0,sizeof(s->fb));
    uint32_t x=0,y=0;
    while (s->lcd.vramaddr_cur < s->lcd.vramaddr_max && y < h) {
        uint32_t d = lcd_dma_read(s);
        int pixels = 0, bits = 0;
        switch(s->lcd.bppmode){case BPPMODE_TFT_01:pixels=32;bits=1;break;case BPPMODE_TFT_02:pixels=16;bits=2;break;case BPPMODE_TFT_04:pixels=8;bits=4;break;case BPPMODE_TFT_08:pixels=4;bits=8;break;case BPPMODE_TFT_16:pixels=2;bits=16;break;default:return;}
        for(int i=0;i<pixels && y<h;i++){
            uint32_t color;
            if(bits==16){ color=color_lcd5551((uint16_t)(d>>16)); d<<=16; }
            else { unsigned shift=32u-(unsigned)bits; uint32_t idx=(d>>shift)&((1u<<bits)-1u); color=pal(s,idx); d<<=bits; }
            if(x<w) s->fb[y*240u+x]=color;
            x++; if(x>=w){x=0;y++;}
        }
    }
    /*
     * GP32 panel aperture: the BIOS programs a 240x320 portrait DMA surface
     * and the handheld mounts it as a 320x240 landscape panel.  The first raw
     * DMA scanline is a panel-settle/prefetch line and is not part of the
     * visible glass area.  The official BIOS leaves that line stale while
     * fading the boot logo to black; exposing it after rotation produces a
     * spurious one-pixel white line at the left edge.  Present the visible
     * aperture by replacing that hidden raw scanline with the first displayed
     * one.  This is a panel-timing/aperture rule, not a BIOS-title filter.
     */
    if (w == 240u && h == 320u) {
        memcpy(&s->fb[0], &s->fb[240u], 240u * sizeof(s->fb[0]));
    }
    s->fb_w = w; s->fb_h = h; s->frame_counter++;
}

static uint32_t clk_fclk(const s3c2400_t *s, int reg) {
    uint32_t data = s->clkpow[reg];
    uint32_t mdiv = GP32_BITS(data, 19, 12);
    uint32_t pdiv = GP32_BITS(data, 9, 4);
    uint32_t sdiv = GP32_BITS(data, 1, 0);
    uint32_t den = (pdiv + 2u) << sdiv;
    if (!den) return 12000000u;
    return (uint32_t)(((uint64_t)(mdiv + 8u) * 12000000ull) / den);
}

static uint32_t clk_hclk(const s3c2400_t *s, int reg) {
    uint32_t f = clk_fclk(s, reg);
    switch (s->clkpow[5] & 3u) {
    case 0: return f;
    case 1: return f;
    case 2: return f / 2u;
    default: return f / 2u;
    }
}

static uint32_t clk_run(const s3c2400_t *s, int reg) {
    /* The ARM core presently accounts one emulator cycle per decoded/executed
     * instruction group, not per ARM920T FCLK pipeline cycle. For ordinary
     * undivided clocks this maps to the firmware clock directly. In the common
     * GP32 high-PLL / half-HCLK mode, real code is bus/wait-state limited while
     * this interpreter/JIT does not yet charge those stalls per instruction, so
     * use an effective bus-side throughput for frontend pacing and for all
     * peripheral-period-to-core-cycle conversions. This is clock-topology based,
     * not title-specific. */
    uint32_t f = clk_fclk(s, reg);
    uint32_t h = clk_hclk(s, reg);
    if (!h) return f;
    if (f >= 100000000u && h <= f / 2u && h >= 60000000u) {
        uint64_t scaled = ((uint64_t)h * 8u + 5u) / 11u;
        return scaled ? (uint32_t)scaled : h;
    }
    return h ? h : f;
}

static uint32_t clk_pclk(const s3c2400_t *s, int reg) {
    uint32_t f = clk_fclk(s, reg);
    switch (s->clkpow[5] & 3u) {
    case 0: return f;
    case 1: return f / 2u;
    case 2: return f / 2u;
    default: return f / 4u;
    }
}

uint32_t s3c2400_fclk_hz(const s3c2400_t *s) {
    uint32_t f = s ? clk_fclk(s, MPLLCON) : 0u;
    return f ? f : 66000000u;
}

uint32_t s3c2400_hclk_hz(const s3c2400_t *s) {
    uint32_t h = s ? clk_hclk(s, MPLLCON) : 0u;
    return h ? h : 66000000u;
}

uint32_t s3c2400_run_clock_hz(const s3c2400_t *s) {
    uint32_t h = s ? clk_run(s, MPLLCON) : 0u;
    return h ? h : 66000000u;
}

static void audio_append_stereo(s3c2400_t *s, int16_t left, int16_t right) {
    if (!s) return;
    if (s->audio_frames >= s->audio_cap_frames) {
        uint64_t new_cap = s->audio_cap_frames ? s->audio_cap_frames * 2u : 65536u;
        if (new_cap < s->audio_frames + 1u) new_cap = s->audio_frames + 1u;
        if (new_cap > (UINT64_MAX / (2u * sizeof(int16_t)))) return;
        int16_t *n = (int16_t *)realloc(s->audio, (size_t)new_cap * 2u * sizeof(int16_t));
        if (!n) return;
        s->audio = n;
        s->audio_cap_frames = new_cap;
    }
    s->audio[s->audio_frames * 2u + 0u] = left;
    s->audio[s->audio_frames * 2u + 1u] = right;
    s->audio_frames++;
}

void s3c2400_audio_append_u8_mono(s3c2400_t *s, const uint8_t *samples, uint32_t sample_count, uint32_t sample_rate_hz) {
    if (!s || !samples || !sample_count) return;
    s->audio_sample_rate_hz = sample_rate_hz ? sample_rate_hz : 11025u;
    for (uint32_t i = 0; i < sample_count; ++i) {
        int16_t v = (int16_t)(((int)samples[i] - 128) << 8);
        audio_append_stereo(s, v, v);
    }
}

void s3c2400_audio_append_s16_stereo(s3c2400_t *s, int16_t left, int16_t right, uint32_t sample_rate_hz) {
    if (!s) return;
    s->audio_sample_rate_hz = sample_rate_hz ? sample_rate_hz : 11025u;
    audio_append_stereo(s, left, right);
}


static uint32_t lcd_visible_lines(const s3c2400_t *s) {
    if (!s) return 320u;
    uint32_t lineval = GP32_BITS(s->lcd_regs[1], 23, 14);
    uint32_t visible = lineval + 1u;
    if (visible == 0u || visible > 1024u) visible = 320u;
    return visible;
}

static uint32_t lcd_total_lines_for_visible(uint32_t visible) {
    uint32_t vblank_lines = (visible / 16u) + 8u;
    if (vblank_lines < 8u) vblank_lines = 8u;
    uint32_t total_lines = visible + vblank_lines;
    if (total_lines <= visible) total_lines = visible + 1u;
    return total_lines;
}

static uint64_t lcd_panel_frame_cycles(const s3c2400_t *s) {
    uint32_t runclk = clk_run(s, MPLLCON);
    if (!runclk) runclk = 66000000u;
    uint64_t frame_cycles = ((uint64_t)runclk + 30u) / 60u;
    return frame_cycles ? frame_cycles : 1u;
}

static uint32_t lcd_current_line_count(const s3c2400_t *s) {
    if (!s) return 0u;
    uint32_t visible = lcd_visible_lines(s);

    /*
     * LCDCON1[27:18] is consumed by BIOS code as a live LCD scan/vblank
     * status field.  The emulator's elapsed value is in effective ARM run
     * cycles, so convert the GP32 panel frame cadence into the same unit
     * before deriving the current line.  This keeps polling loops tied to the
     * emulated LCD frame rather than to host presentation phase or raw VCLK
     * register values that commercial GP32 software does not use literally.
     */
    uint64_t frame_cycles = lcd_panel_frame_cycles(s);
    uint32_t total_lines = lcd_total_lines_for_visible(visible);
    uint64_t line_cycles = frame_cycles / total_lines;
    if (!line_cycles) line_cycles = 1u;

    uint64_t line64 = (s->lcd_line_accum % frame_cycles) / line_cycles;
    uint32_t line = (line64 >= total_lines) ? (total_lines - 1u) : (uint32_t)line64;
    if (line >= visible) return 0u;
    return (visible - 1u) - line;
}

static void iis_fifo_write16(s3c2400_t *s, uint16_t sample) {
    if (!s) return;
    s->iis_fifo[s->iis_fifo_index++] = sample;
    if (s->iis_fifo_index >= 2u) {
        s->iis_fifo_index = 0;
        audio_append_stereo(s, (int16_t)s->iis_fifo[0], (int16_t)s->iis_fifo[1]);
    }
}

static uint32_t iis_frame_rate_hz(const s3c2400_t *s) {
    static const uint32_t codeclk_table[2] = { 256u, 384u };
    uint32_t pclk = clk_pclk(s, MPLLCON);
    if (!pclk) pclk = 12000000u;
    uint32_t prescaler_a = GP32_BITS(s->iis[2], 9, 5);
    uint32_t codeclk = codeclk_table[GP32_BIT(s->iis[1], 2)];
    /*
     * The GP32 BIOS "nobody" sample is a useful sanity check here. v13
     * matched its approximate duration by tagging the emitted stream as an
     * 8 kHz MCLK-derived stream, but that lowered the pitch by roughly a
     * third versus a hardware recording. The S3C2400 IIS block is driven by
     * the current peripheral clock; for the BIOS registers this gives about
     * 11.025 kHz, which aligns the dominant spectral peaks with the hardware
     * capture.
     *
     * The timer below advances once per complete stereo output frame. 16-bit
     * DMA still performs two FIFO halfword writes during that frame.
     */
    uint64_t rate = (uint64_t)pclk / ((uint64_t)(prescaler_a + 1u) * (uint64_t)codeclk);
    if (rate < 4000u) rate = 4000u;
    if (rate > 96000u) rate = 96000u;
    return (uint32_t)rate;
}

static uint64_t iis_period_cpu_cycles(const s3c2400_t *s) {
    uint32_t runclk = clk_run(s, MPLLCON);
    uint32_t rate = iis_frame_rate_hz(s);
    if (!runclk) runclk = 40000000u;
    if (!rate) return 1u;
    uint64_t period = ((uint64_t)runclk + (uint64_t)rate - 1u) / (uint64_t)rate;
    if (period < 256u) period = 256u;
    return period;
}

static uint32_t iis_dma_transfers_per_frame(const s3c2400_t *s) {
    if (!s) return 1u;
    const uint32_t *r = &s->dma[2u << 3];
    unsigned dsz = GP32_BITS(r[2], 21, 20);
    /*
     * One tick of the simplified IIS scheduler represents one complete stereo
     * audio frame. 16-bit DMA therefore needs two FIFO writes (left/right) in
     * that frame period, while a 32-bit DMA write carries both halfwords.
     */
    if (dsz == 0u) return 4u;
    if (dsz == 1u) return 2u;
    return 1u;
}

static void iis_refresh_clock_cache(s3c2400_t *s) {
    if (!s || !s->iis_clock_dirty) return;
    s->iis_cached_rate_hz = iis_frame_rate_hz(s);
    s->iis_cached_period_cycles = iis_period_cpu_cycles(s);
    if (!s->iis_cached_period_cycles) s->iis_cached_period_cycles = 1u;
    s->audio_sample_rate_hz = s->iis_cached_rate_hz;
    s->iis_clock_dirty = 0;
}

static uint64_t pwm_period_cpu_cycles(s3c2400_t *s, unsigned t) {
    if (!s || t >= 5u) return 1u;
    pwm_refresh_clock_cache(s);
    return s->pwm_period_cycles[t] ? s->pwm_period_cycles[t] : 1u;
}

static void dma_request_iis(s3c2400_t *s) {
    uint32_t *r = &s->dma[2 << 3];
    if ((r[6] & 2u) && GP32_BIT(r[2], 23) && GP32_BITS(r[2], 25, 24) == 0u) dma_trigger(s, 2);
}

static void dma_request_pwm(s3c2400_t *s) {
    for (int ch = 0; ch < 4; ++ch) {
        if (ch == 1) continue;
        uint32_t *r = &s->dma[ch << 3];
        if ((r[6] & 2u) && GP32_BIT(r[2], 23) && GP32_BITS(r[2], 25, 24) == 3u) dma_trigger(s, ch);
    }
}

void s3c2400_tick(s3c2400_t *s, uint32_t cpu_cycles) {
    if (!s || !cpu_cycles) return;
    uint64_t old_lcd_accum = s->lcd_line_accum;
    uint64_t lcd_frame_cycles = lcd_panel_frame_cycles(s);
    s->lcd_line_accum += (uint64_t)cpu_cycles;
    if ((s->lcd_regs[0] & 1u) && lcd_frame_cycles &&
        (old_lcd_accum / lcd_frame_cycles) != (s->lcd_line_accum / lcd_frame_cycles)) {
        /*
         * The LCD output should be a completed scanout, not a fresh full-frame
         * decode of VRAM/palette at whatever cycle the host frontend asks for
         * pixels.  BIOS boot effects rewrite the framebuffer and palette
         * between vblank waits; rendering only when the emulated panel reaches
         * the frame boundary prevents random one-line snapshots of those
         * in-progress updates while preserving ordinary game rendering.
         */
        s3c2400_render_lcd(s);
    }
    if (s->iis[0] & 1u) {
        iis_refresh_clock_cache(s);
        uint64_t period = s->iis_cached_period_cycles ? s->iis_cached_period_cycles : 1u;
        s->iis_accum += cpu_cycles;
        if (s->iis_accum >= period) {
            uint64_t periods64 = s->iis_accum / period;
            uint32_t transfers_per_frame = iis_dma_transfers_per_frame(s);
            s->iis_accum -= periods64 * period;
            while (periods64) {
                uint32_t frame_batch = periods64 > 2048u ? 2048u : (uint32_t)periods64;
                uint32_t transfer_count = frame_batch * transfers_per_frame;
                uint32_t done_fast = dma_request_iis_fast_count(s, transfer_count);
                if (done_fast < transfer_count) {
                    for (uint32_t i = done_fast; i < transfer_count; ++i) dma_request_iis(s);
                }
                periods64 -= frame_batch;
            }
        }
    } else {
        s->iis_accum = 0;
    }
    static const unsigned start_mask[5] = { 0x000001u, 0x000100u, 0x001000u, 0x010000u, 0x100000u };
    static const unsigned auto_mask[5]  = { 0x000008u, 0x000800u, 0x008000u, 0x080000u, 0x400000u };
    static const unsigned irq_no[5] = { INT_TIMER0, INT_TIMER1, INT_TIMER2, INT_TIMER3, INT_TIMER4 };
    unsigned pwm_dma_timer = GP32_BITS(s->pwm[1], 23, 20);
    for (unsigned t = 0; t < 5; ++t) {
        if (!(s->pwm[2] & start_mask[t])) { s->pwm_accum[t] = 0; continue; }
        uint64_t period = pwm_period_cpu_cycles(s, t);
        s->pwm_accum[t] += cpu_cycles;
        while (s->pwm_accum[t] >= period) {
            s->pwm_accum[t] -= period;
            if (pwm_dma_timer == t + 1u) dma_request_pwm(s);
            else request_irq(s, irq_no[t]);
            if (!(s->pwm[2] & auto_mask[t])) { s->pwm[2] &= ~start_mask[t]; s->pwm_accum[t] = 0; break; }
        }
    }
}

uint32_t s3c2400_debug_read32(s3c2400_t *s, uint32_t addr) {
    return s ? s3c2400_read32(s, addr) : 0xffffffffu;
}

const uint32_t *s3c2400_framebuffer(s3c2400_t *s, uint32_t *w, uint32_t *h, uint32_t *stride, uint64_t *frames) {
    if (!s) return NULL;
    if (s->frame_counter == 0u && (s->lcd_regs[0] & 1u)) s3c2400_render_lcd(s);
    if (w) *w = s->fb_w;
    if (h) *h = s->fb_h;
    if (stride) *stride = 240;
    if (frames) *frames = s->frame_counter;
    return s->fb;
}

const int16_t *s3c2400_audio_samples(s3c2400_t *s, uint64_t *frames, uint32_t *sample_rate_hz) {
    if (!s) return NULL;
    if (frames) *frames = s->audio_frames;
    if (sample_rate_hz) *sample_rate_hz = s->audio_sample_rate_hz ? s->audio_sample_rate_hz : 44100u;
    return s->audio;
}

void s3c2400_audio_clear(s3c2400_t *s) {
    if (!s) return;
    /*
     * This clears the host-facing captured PCM buffer only. Do not reset
     * iis_fifo_index here: it is emulated IIS FIFO state, and SDL calls this
     * once per video frame after consuming audio. Resetting it at arbitrary
     * frontend chunk boundaries drops a pending left/right halfword and causes
     * audible clicks/chopping.
     */
    s->audio_frames = 0;
}

typedef struct s3c2400_state_image {
    uint8_t bios[BIOS_SIZE];
    size_t ram_size;
    uint32_t buttons;
    uint32_t fb[320 * 240];
    uint32_t fb_w, fb_h;
    uint64_t frame_counter;
    uint32_t lcd_vpos;
    uint64_t lcd_line_accum;
    uint8_t eeprom[0x2000];
    uint8_t iic_data[4];
    int iic_data_index;
    uint16_t iic_address;
    uint32_t lcd_regs[0x400/4];
    uint16_t lcd_palette[0x400/2];
    uint32_t memcon[0x34/4];
    uint32_t usb_host[0x5c/4];
    uint32_t irq[0x18/4];
    uint32_t dma[0x7c/4];
    uint32_t clkpow[0x18/4];
    uint32_t uart0[0x2c/4];
    uint32_t uart1[0x2c/4];
    uint32_t pwm[0x44/4];
    uint64_t pwm_accum[5];
    uint32_t usb_dev[0xbc/4];
    uint32_t watchdog[0x0c/4];
    uint32_t iic[0x10/4];
    uint32_t iis[0x14/4];
    uint16_t iis_fifo[2];
    unsigned iis_fifo_index;
    uint64_t iis_accum;
    uint64_t audio_frames;
    uint32_t audio_sample_rate_hz;
    uint32_t iis_cached_rate_hz;
    uint64_t iis_cached_period_cycles;
    uint8_t iis_clock_dirty;
    uint32_t gpio[0x60/4];
    uint32_t rtc[0x4c/4];
    uint32_t adc[0x08/4];
    uint32_t spi[0x18/4];
    uint32_t mmc[0x40/4];
    lcd_state_t lcd;
    gp32_smc_lines_t smc_lines;
} s3c2400_state_image_t;

int s3c2400_state_save(const s3c2400_t *s, FILE *f) {
    if (!s || !f) return 0;
#ifdef GP32EMU_WASM
    static s3c2400_state_image_t st_storage;
    s3c2400_state_image_t *st = &st_storage;
#else
    s3c2400_state_image_t st_storage;
    s3c2400_state_image_t *st = &st_storage;
#endif
    memset(st, 0, sizeof(*st));
    memcpy(st->bios, s->bios, sizeof(st->bios));
    st->ram_size = s->ram_size;
    st->buttons = s->buttons;
    memcpy(st->fb, s->fb, sizeof(st->fb));
    st->fb_w = s->fb_w; st->fb_h = s->fb_h;
    st->frame_counter = s->frame_counter;
    st->lcd_vpos = s->lcd_vpos;
    st->lcd_line_accum = s->lcd_line_accum;
    memcpy(st->eeprom, s->eeprom, sizeof(st->eeprom));
    memcpy(st->iic_data, s->iic_data, sizeof(st->iic_data));
    st->iic_data_index = s->iic_data_index;
    st->iic_address = s->iic_address;
    memcpy(st->lcd_regs, s->lcd_regs, sizeof(st->lcd_regs));
    memcpy(st->lcd_palette, s->lcd_palette, sizeof(st->lcd_palette));
    memcpy(st->memcon, s->memcon, sizeof(st->memcon));
    memcpy(st->usb_host, s->usb_host, sizeof(st->usb_host));
    memcpy(st->irq, s->irq, sizeof(st->irq));
    memcpy(st->dma, s->dma, sizeof(st->dma));
    memcpy(st->clkpow, s->clkpow, sizeof(st->clkpow));
    memcpy(st->uart0, s->uart0, sizeof(st->uart0));
    memcpy(st->uart1, s->uart1, sizeof(st->uart1));
    memcpy(st->pwm, s->pwm, sizeof(st->pwm));
    memcpy(st->pwm_accum, s->pwm_accum, sizeof(st->pwm_accum));
    memcpy(st->usb_dev, s->usb_dev, sizeof(st->usb_dev));
    memcpy(st->watchdog, s->watchdog, sizeof(st->watchdog));
    memcpy(st->iic, s->iic, sizeof(st->iic));
    memcpy(st->iis, s->iis, sizeof(st->iis));
    memcpy(st->iis_fifo, s->iis_fifo, sizeof(st->iis_fifo));
    st->iis_fifo_index = s->iis_fifo_index;
    st->iis_accum = s->iis_accum;
    st->audio_frames = s->audio_frames;
    st->audio_sample_rate_hz = s->audio_sample_rate_hz;
    st->iis_cached_rate_hz = s->iis_cached_rate_hz;
    st->iis_cached_period_cycles = s->iis_cached_period_cycles;
    st->iis_clock_dirty = s->iis_clock_dirty;
    memcpy(st->gpio, s->gpio, sizeof(st->gpio));
    memcpy(st->rtc, s->rtc, sizeof(st->rtc));
    memcpy(st->adc, s->adc, sizeof(st->adc));
    memcpy(st->spi, s->spi, sizeof(st->spi));
    memcpy(st->mmc, s->mmc, sizeof(st->mmc));
    st->lcd = s->lcd;
    st->smc_lines = s->smc_lines;
    if (fwrite(st, 1, sizeof(*st), f) != sizeof(*st)) return 0;
    if (st->ram_size && s->ram && fwrite(s->ram, 1, st->ram_size, f) != st->ram_size) return 0;
    if (!smc_state_save(s->smc, f)) return 0;
    if (st->audio_frames && s->audio && fwrite(s->audio, (size_t)2u * sizeof(int16_t), (size_t)st->audio_frames, f) != (size_t)st->audio_frames) return 0;
    return 1;
}

int s3c2400_state_load(s3c2400_t *s, FILE *f) {
    if (!s || !f) return 0;
#ifdef GP32EMU_WASM
    static s3c2400_state_image_t st_storage;
    s3c2400_state_image_t *st = &st_storage;
#else
    s3c2400_state_image_t st_storage;
    s3c2400_state_image_t *st = &st_storage;
#endif
    if (fread(st, 1, sizeof(*st), f) != sizeof(*st)) return 0;
    if (st->ram_size == 0u || st->ram_size > (size_t)64u * 1024u * 1024u || st->audio_frames > (uint64_t)10u * 60u * 44100u) return 0;
    uint8_t *new_ram = (uint8_t *)malloc(st->ram_size);
    if (!new_ram) return 0;
    if (fread(new_ram, 1, st->ram_size, f) != st->ram_size) { free(new_ram); return 0; }
    if (!smc_state_load(s->smc, f)) { free(new_ram); return 0; }
    int16_t *new_audio = NULL;
    uint64_t new_audio_cap = 0;
    if (st->audio_frames) {
        new_audio = (int16_t *)malloc((size_t)st->audio_frames * 2u * sizeof(int16_t));
        if (!new_audio) { free(new_ram); return 0; }
        if (fread(new_audio, (size_t)2u * sizeof(int16_t), (size_t)st->audio_frames, f) != (size_t)st->audio_frames) { free(new_audio); free(new_ram); return 0; }
        new_audio_cap = st->audio_frames;
    }
    free(s->ram);
    free(s->audio);
    s->ram = new_ram;
    s->ram_size = st->ram_size;
    s->audio = new_audio;
    s->audio_cap_frames = new_audio_cap;
    memcpy(s->bios, st->bios, sizeof(s->bios));
    s->buttons = st->buttons;
    memcpy(s->fb, st->fb, sizeof(s->fb));
    s->fb_w = st->fb_w; s->fb_h = st->fb_h;
    s->frame_counter = st->frame_counter;
    s->lcd_vpos = st->lcd_vpos;
    s->lcd_line_accum = st->lcd_line_accum;
    memcpy(s->eeprom, st->eeprom, sizeof(s->eeprom));
    memcpy(s->iic_data, st->iic_data, sizeof(s->iic_data));
    s->iic_data_index = st->iic_data_index;
    s->iic_address = st->iic_address;
    memcpy(s->lcd_regs, st->lcd_regs, sizeof(s->lcd_regs));
    memcpy(s->lcd_palette, st->lcd_palette, sizeof(s->lcd_palette));
    memcpy(s->memcon, st->memcon, sizeof(s->memcon));
    memcpy(s->usb_host, st->usb_host, sizeof(s->usb_host));
    memcpy(s->irq, st->irq, sizeof(s->irq));
    memcpy(s->dma, st->dma, sizeof(s->dma));
    memcpy(s->clkpow, st->clkpow, sizeof(s->clkpow));
    memcpy(s->uart0, st->uart0, sizeof(s->uart0));
    memcpy(s->uart1, st->uart1, sizeof(s->uart1));
    memcpy(s->pwm, st->pwm, sizeof(s->pwm));
    memcpy(s->pwm_accum, st->pwm_accum, sizeof(s->pwm_accum));
    s->pwm_clock_dirty = 1;
    memcpy(s->usb_dev, st->usb_dev, sizeof(s->usb_dev));
    memcpy(s->watchdog, st->watchdog, sizeof(s->watchdog));
    memcpy(s->iic, st->iic, sizeof(s->iic));
    memcpy(s->iis, st->iis, sizeof(s->iis));
    memcpy(s->iis_fifo, st->iis_fifo, sizeof(s->iis_fifo));
    s->iis_fifo_index = st->iis_fifo_index;
    s->iis_accum = st->iis_accum;
    s->audio_frames = st->audio_frames;
    s->audio_sample_rate_hz = st->audio_sample_rate_hz;
    s->iis_cached_rate_hz = st->iis_cached_rate_hz;
    s->iis_cached_period_cycles = st->iis_cached_period_cycles;
    s->iis_clock_dirty = st->iis_clock_dirty;
    memcpy(s->gpio, st->gpio, sizeof(s->gpio));
    memcpy(s->rtc, st->rtc, sizeof(s->rtc));
    memcpy(s->adc, st->adc, sizeof(s->adc));
    memcpy(s->spi, st->spi, sizeof(s->spi));
    memcpy(s->mmc, st->mmc, sizeof(s->mmc));
    s->lcd = st->lcd;
    s->smc_lines = st->smc_lines;
    check_irq(s);
    return 1;
}
