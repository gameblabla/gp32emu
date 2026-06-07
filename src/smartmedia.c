/*
 * SmartMedia/NAND model ported to C11 from MAME's smartmed/nandflash devices.
 * Original files: src/devices/machine/smartmed.cpp, nandflash.cpp
 * License: BSD-3-Clause, copyright Raphael Nabet.
 */
#include "smartmedia.h"
#include "zip.h"

typedef enum sm_mode {
    SM_M_INIT,
    SM_M_READ,
    SM_M_PROGRAM,
    SM_M_ERASE,
    SM_M_READSTATUS,
    SM_M_READID,
    SM_M_30,
    SM_M_RANDOM_DATA_INPUT,
    SM_M_RANDOM_DATA_OUTPUT
} sm_mode_t;

typedef enum sm_pointer_mode {
    SM_PM_A,
    SM_PM_B,
    SM_PM_C
} sm_pointer_mode_t;

struct smc {
    uint8_t *data;
    size_t data_size;
    uint8_t header[1024];
    size_t header_size;
    int dirty;
    uint32_t page_data_size;
    uint32_t page_total_size;
    uint32_t num_pages;
    uint32_t log2_pages_per_block;
    uint32_t col_address_cycles;
    uint32_t row_address_cycles;
    uint32_t sequential_row_read;
    uint8_t id[5];
    uint32_t id_len;
    uint8_t *page_reg;
    uint8_t data_uid[256 + 16];
    int data_uid_present;
    sm_mode_t mode;
    sm_pointer_mode_t pointer_mode;
    uint32_t page_addr;
    uint32_t byte_addr;
    uint32_t addr_load_ptr;
    uint8_t status;
    uint8_t accumulated_status;
    bool mode_3065;
    uint32_t program_byte_count;
};

static unsigned log2_u32(uint32_t v) {
    unsigned n = 0;
    while ((UINT32_C(1) << n) < v) n++;
    return n;
}

smc_t *smc_create(void) {
    smc_t *s = (smc_t *)calloc(1, sizeof(*s));
    if (s) smc_reset(s);
    return s;
}

void smc_destroy(smc_t *s) {
    if (!s) return;
    free(s->data);
    free(s->page_reg);
    free(s);
}

static int smc_detect_small_geometry(uint8_t id1, uint8_t id2, uint32_t *page_data, uint32_t *page_total, uint32_t *pages, uint32_t *log2_ppb) {
    if (id1 == 0xec) {
        switch (id2) {
        case 0xa4: *page_data=0x100; *page_total=0x108; *pages=0x00800; *log2_ppb=0; return 1;
        case 0x6e: *page_data=0x100; *page_total=0x108; *pages=0x01000; *log2_ppb=0; return 1;
        case 0xea: *page_data=0x100; *page_total=0x108; *pages=0x02000; *log2_ppb=4; return 1;
        case 0xe3: *page_data=0x200; *page_total=0x210; *pages=0x02000; *log2_ppb=4; return 1;
        case 0xe6: *page_data=0x200; *page_total=0x210; *pages=0x04000; *log2_ppb=4; return 1;
        case 0x73: *page_data=0x200; *page_total=0x210; *pages=0x08000; *log2_ppb=5; return 1;
        case 0x75: *page_data=0x200; *page_total=0x210; *pages=0x10000; *log2_ppb=5; return 1;
        case 0x76: *page_data=0x200; *page_total=0x210; *pages=0x20000; *log2_ppb=5; return 1;
        case 0x79: *page_data=0x200; *page_total=0x210; *pages=0x40000; *log2_ppb=5; return 1;
        }
    } else if (id1 == 0x98) {
        switch (id2) {
        case 0x73: *page_data=0x200; *page_total=0x210; *pages=0x08000; *log2_ppb=5; return 1;
        case 0x75: *page_data=0x200; *page_total=0x210; *pages=0x10000; *log2_ppb=5; return 1;
        }
    }
    return 0;
}

static int smc_set_geometry_from_size(smc_t *s, size_t size, char *err, size_t err_len) {
    /* MAME GP32 software list uses raw 528-byte pages when geometry is supplied by the softlist. */
    if ((size % 528u) == 0) {
        s->page_data_size = 512;
        s->page_total_size = 528;
        s->num_pages = (uint32_t)(size / 528u);
        s->col_address_cycles = 1;
        s->row_address_cycles = (s->num_pages > 0x10000u) ? 3u : 2u;
        s->log2_pages_per_block = log2_u32(32);
        s->sequential_row_read = 1;
        s->id_len = 2;
        s->id[0] = 0xec;
        if (s->num_pages <= 8192u) s->id[1] = 0xe6;
        else if (s->num_pages <= 32768u) s->id[1] = 0x73;
        else if (s->num_pages <= 65536u) s->id[1] = 0x75;
        else if (s->num_pages <= 131072u) s->id[1] = 0x76;
        else s->id[1] = 0x79;
        return 1;
    }
    if ((size % 2112u) == 0) {
        s->page_data_size = 2048;
        s->page_total_size = 2112;
        s->num_pages = (uint32_t)(size / 2112u);
        s->col_address_cycles = 2;
        s->row_address_cycles = (s->num_pages > 0x10000u) ? 3u : 2u;
        s->log2_pages_per_block = log2_u32(64);
        s->sequential_row_read = 0;
        s->id_len = 4;
        s->id[0] = 0xec; s->id[1] = 0xf1; s->id[2] = 0x00; s->id[3] = 0x15;
        return 1;
    }
    if (err && err_len) snprintf(err, err_len, "unsupported SmartMedia size %zu; expected MAME format-2 or raw 528-/2112-byte pages", size);
    return 0;
}

int smc_load_buffer(smc_t *s, const uint8_t *src, size_t len, char *err, size_t err_len) {
    if (!s || !src || len == 0) return 0;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return 0;
    memcpy(buf, src, len);
    free(s->data);
    free(s->page_reg);
    memset(s, 0, sizeof(*s));

    size_t payload_off = 0;
    uint32_t pd = 0, pt = 0, np = 0, ppb = 0;
    if (len > 1024u && smc_detect_small_geometry(buf[0], buf[1], &pd, &pt, &np, &ppb) &&
        (len - 1024u) == (size_t)pt * np) {
        payload_off = 1024u;
        memcpy(s->header, buf, 1024u);
        s->header_size = 1024u;
        s->page_data_size = pd;
        s->page_total_size = pt;
        s->num_pages = np;
        s->log2_pages_per_block = ppb;
        s->col_address_cycles = 1;
        s->row_address_cycles = (np > 0x10000u) ? 3u : 2u;
        s->sequential_row_read = 1;
        s->id_len = 3;
        s->id[0] = buf[0];
        s->id[1] = buf[1];
        s->id[2] = buf[2];
        for (int i = 0; i < 8; ++i) {
            memcpy(s->data_uid + i * 32, buf + 256, 16);
            for (int j = 0; j < 16; ++j) s->data_uid[i * 32 + 16 + j] = (uint8_t)(buf[256 + j] ^ 0xffu);
        }
        memcpy(s->data_uid + 256, buf + 272, 16);
        s->data_uid_present = 1;
    }

    if (payload_off) {
        s->data_size = len - payload_off;
        s->data = (uint8_t *)malloc(s->data_size);
        if (!s->data) { free(buf); return 0; }
        memcpy(s->data, buf + payload_off, s->data_size);
        free(buf);
    } else {
        s->data = buf;
        s->data_size = len;
        if (!smc_set_geometry_from_size(s, s->data_size, err, err_len)) {
            free(s->data);
            s->data = NULL;
            s->data_size = 0;
            return 0;
        }
    }
    s->page_reg = (uint8_t *)malloc(s->page_total_size);
    if (!s->page_reg) {
        free(s->data);
        memset(s, 0, sizeof(*s));
        return 0;
    }
    smc_reset(s);
    return 1;
}

int smc_load_file(smc_t *s, const char *path, char *err, size_t err_len) {
    if (!s || !path) return 0;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (gp32_zip_path_maybe(path)) {
        char entry_name[260] = {0};
        static const char * const exts[] = { ".smc" };
        if (!gp32_zip_read_first_matching(path, exts, GP32_ARRAY_COUNT(exts), &buf, &len, entry_name, sizeof(entry_name), err, err_len)) return 0;
        GP32_UNUSED(entry_name);
    } else {
        FILE *f = fopen(path, "rb");
        if (!f) {
            if (err && err_len) snprintf(err, err_len, "open %s: %s", path, strerror(errno));
            return 0;
        }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
        long n = ftell(f);
        if (n <= 0) { fclose(f); return 0; }
        rewind(f);
        buf = (uint8_t *)malloc((size_t)n);
        if (!buf) { fclose(f); return 0; }
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
            if (err && err_len) snprintf(err, err_len, "read %s failed", path);
            free(buf);
            fclose(f);
            return 0;
        }
        fclose(f);
        len = (size_t)n;
    }
    int ok = smc_load_buffer(s, buf, len, err, err_len);
    free(buf);
    return ok;
}

int smc_save_file(smc_t *s, const char *path, char *err, size_t err_len) {
    if (!s || !path || !s->data) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) {
        if (err && err_len) snprintf(err, err_len, "open %s: %s", path, strerror(errno));
        return 0;
    }
    if (s->header_size) {
        if (fwrite(s->header, 1, s->header_size, f) != s->header_size) {
            if (err && err_len) snprintf(err, err_len, "write %s header failed", path);
            fclose(f);
            return 0;
        }
    }
    if (fwrite(s->data, 1, s->data_size, f) != s->data_size) {
        if (err && err_len) snprintf(err, err_len, "write %s payload failed", path);
        fclose(f);
        return 0;
    }
    if (fclose(f) != 0) {
        if (err && err_len) snprintf(err, err_len, "close %s: %s", path, strerror(errno));
        return 0;
    }
    s->dirty = 0;
    return 1;
}

int smc_is_dirty(const smc_t *s) { return s ? s->dirty : 0; }

void smc_reset(smc_t *s) {
    if (!s) return;
    s->mode = SM_M_INIT;
    s->pointer_mode = SM_PM_A;
    s->page_addr = 0;
    s->byte_addr = 0;
    s->addr_load_ptr = 0;
    s->accumulated_status = 0;
    s->mode_3065 = false;
    s->program_byte_count = 0;
    s->status = 0xc0; /* ready, not protected */
    if (s->page_reg && s->page_total_size) memset(s->page_reg, 0xff, s->page_total_size);
}

int smc_is_present(const smc_t *s) { return s && s->data && s->num_pages != 0; }
int smc_is_protected(const smc_t *s) { return s ? ((s->status & 0x80) == 0) : 1; }
int smc_is_busy(const smc_t *s) { return s ? ((s->status & 0x40) == 0) : 0; }
size_t smc_image_size(const smc_t *s) { return s ? s->data_size : 0; }

void smc_command_w(smc_t *s, uint8_t data) {
    if (!smc_is_present(s)) return;
    switch (data) {
    case 0xff:
        s->mode = SM_M_INIT;
        s->pointer_mode = SM_PM_A;
        s->status = (uint8_t)((s->status & 0x80) | 0x40);
        s->accumulated_status = 0;
        s->mode_3065 = false;
        break;
    case 0x00:
        s->mode = SM_M_READ;
        s->pointer_mode = SM_PM_A;
        s->addr_load_ptr = 0;
        break;
    case 0x01:
        s->mode = SM_M_READ;
        s->pointer_mode = SM_PM_B;
        s->addr_load_ptr = 0;
        break;
    case 0x50:
        s->mode = SM_M_READ;
        s->pointer_mode = SM_PM_C;
        s->addr_load_ptr = 0;
        break;
    case 0x80:
        s->mode = SM_M_PROGRAM;
        s->addr_load_ptr = 0;
        s->program_byte_count = 0;
        if (s->page_reg) memset(s->page_reg, 0xff, s->page_total_size);
        break;
    case 0x10:
    case 0x15:
        if (s->mode == SM_M_PROGRAM || s->mode == SM_M_RANDOM_DATA_INPUT) {
            s->status = (uint8_t)((s->status & 0x80) | s->accumulated_status);
            if (s->page_addr < s->num_pages) {
                uint8_t *dst = &s->data[(size_t)s->page_addr * s->page_total_size];
                for (uint32_t i = 0; i < s->page_total_size; ++i) dst[i] &= s->page_reg[i];
                s->dirty = 1;
            }
            s->status |= 0x40;
            s->accumulated_status = (data == 0x15) ? (uint8_t)(s->status & 0x1f) : 0;
            s->mode = SM_M_INIT;
        } else {
            s->mode = SM_M_INIT;
        }
        break;
    case 0x60:
        s->mode = SM_M_ERASE;
        s->page_addr = 0;
        s->addr_load_ptr = 0;
        break;
    case 0xd0:
        if (s->mode == SM_M_ERASE) {
            uint32_t first = s->page_addr & ~((UINT32_C(1) << s->log2_pages_per_block) - 1u);
            size_t off = (size_t)first * s->page_total_size;
            size_t len = ((size_t)1u << s->log2_pages_per_block) * s->page_total_size;
            if (off < s->data_size) {
                if (off + len > s->data_size) len = s->data_size - off;
                memset(s->data + off, 0xff, len);
                s->dirty = 1;
            }
            s->status |= 0x40;
            s->mode = SM_M_INIT;
            if (s->pointer_mode == SM_PM_B) s->pointer_mode = SM_PM_A;
        } else {
            s->mode = SM_M_INIT;
        }
        break;
    case 0x70:
        s->mode = SM_M_READSTATUS;
        break;
    case 0x90:
        s->mode = SM_M_READID;
        s->addr_load_ptr = 0;
        break;
    case 0x30:
        if (s->col_address_cycles == 1) s->mode = SM_M_30;
        else if (s->mode == SM_M_READ && s->addr_load_ptr >= s->col_address_cycles + s->row_address_cycles) { }
        else s->mode = SM_M_INIT;
        break;
    case 0x65:
        if (s->mode == SM_M_30) s->mode_3065 = true;
        else s->mode = SM_M_INIT;
        break;
    case 0x05:
        if (s->mode == SM_M_READ || s->mode == SM_M_RANDOM_DATA_OUTPUT) {
            s->mode = SM_M_RANDOM_DATA_OUTPUT;
            s->addr_load_ptr = 0;
        } else s->mode = SM_M_INIT;
        break;
    case 0xe0:
        if (s->mode != SM_M_RANDOM_DATA_OUTPUT) s->mode = SM_M_INIT;
        break;
    case 0x85:
        if (s->mode == SM_M_PROGRAM || s->mode == SM_M_RANDOM_DATA_INPUT) {
            s->mode = SM_M_RANDOM_DATA_INPUT;
            s->addr_load_ptr = 0;
            s->program_byte_count = 0;
        } else s->mode = SM_M_INIT;
        break;
    default:
        s->mode = SM_M_INIT;
        break;
    }
}

void smc_address_w(smc_t *s, uint8_t data) {
    if (!smc_is_present(s)) return;
    switch (s->mode) {
    case SM_M_READ:
    case SM_M_PROGRAM:
        if (s->addr_load_ptr == 0) s->page_addr = 0;
        if ((s->addr_load_ptr == 0) && (s->col_address_cycles == 1)) {
            switch (s->pointer_mode) {
            case SM_PM_A: s->byte_addr = data; break;
            case SM_PM_B: s->byte_addr = (uint32_t)data + 256u; s->pointer_mode = SM_PM_A; break;
            case SM_PM_C: s->byte_addr = (uint32_t)(data & 0x0f) + (s->mode_3065 ? 256u : s->page_data_size); break;
            }
        } else if (s->addr_load_ptr < s->col_address_cycles) {
            s->byte_addr &= ~(UINT32_C(0xff) << (s->addr_load_ptr * 8u));
            s->byte_addr |= (uint32_t)data << (s->addr_load_ptr * 8u);
        } else if (s->addr_load_ptr < s->col_address_cycles + s->row_address_cycles) {
            uint32_t sh = (s->addr_load_ptr - s->col_address_cycles) * 8u;
            s->page_addr &= ~(UINT32_C(0xff) << sh);
            s->page_addr |= (uint32_t)data << sh;
        }
        s->addr_load_ptr++;
        break;
    case SM_M_ERASE:
        if (s->addr_load_ptr < s->row_address_cycles) {
            s->page_addr &= ~(UINT32_C(0xff) << (s->addr_load_ptr * 8u));
            s->page_addr |= (uint32_t)data << (s->addr_load_ptr * 8u);
        }
        s->addr_load_ptr++;
        break;
    case SM_M_RANDOM_DATA_INPUT:
    case SM_M_RANDOM_DATA_OUTPUT:
        if (s->addr_load_ptr < s->col_address_cycles) {
            s->byte_addr &= ~(UINT32_C(0xff) << (s->addr_load_ptr * 8u));
            s->byte_addr |= (uint32_t)data << (s->addr_load_ptr * 8u);
        }
        s->addr_load_ptr++;
        break;
    case SM_M_READID:
        if (s->addr_load_ptr == 0) s->byte_addr = data;
        s->addr_load_ptr++;
        break;
    default:
        break;
    }
}

uint8_t smc_data_r(smc_t *s) {
    if (!smc_is_present(s)) return 0xff;
    uint8_t reply = 0xff;
    switch (s->mode) {
    case SM_M_READ:
    case SM_M_RANDOM_DATA_OUTPUT:
        if (!s->mode_3065 && s->byte_addr < s->page_total_size && s->page_addr < s->num_pages) {
            reply = s->data[(size_t)s->page_addr * s->page_total_size + s->byte_addr];
        } else if (s->mode_3065 && s->data_uid_present) {
            uint32_t uid_addr = s->page_addr * s->page_total_size + s->byte_addr;
            if (uid_addr < (uint32_t)sizeof(s->data_uid)) reply = s->data_uid[uid_addr];
        }
        s->byte_addr++;
        if ((s->byte_addr == s->page_total_size) && s->sequential_row_read) {
            s->byte_addr = (s->pointer_mode != SM_PM_C) ? 0 : s->page_data_size;
            s->page_addr++;
            if (s->page_addr == s->num_pages) s->page_addr = 0;
        }
        break;
    case SM_M_READSTATUS:
        reply = (uint8_t)(s->status & 0xc1);
        break;
    case SM_M_READID:
        reply = (s->byte_addr < s->id_len) ? s->id[s->byte_addr] : 0;
        s->byte_addr++;
        break;
    default:
        break;
    }
    return reply;
}

void smc_data_w(smc_t *s, uint8_t data) {
    if (!smc_is_present(s)) return;
    switch (s->mode) {
    case SM_M_PROGRAM:
    case SM_M_RANDOM_DATA_INPUT:
        if (s->program_byte_count++ < s->page_total_size && s->page_reg) {
            if (s->byte_addr < s->page_total_size) s->page_reg[s->byte_addr] = data;
        }
        s->byte_addr++;
        if (s->byte_addr == s->page_total_size) s->byte_addr = (s->pointer_mode != SM_PM_C) ? 0 : s->page_data_size;
        break;
    default:
        break;
    }
}

typedef struct smc_state_image {
    size_t data_size;
    uint8_t header[1024];
    size_t header_size;
    int dirty;
    uint32_t page_data_size;
    uint32_t page_total_size;
    uint32_t num_pages;
    uint32_t log2_pages_per_block;
    uint32_t col_address_cycles;
    uint32_t row_address_cycles;
    uint32_t sequential_row_read;
    uint8_t id[5];
    uint32_t id_len;
    uint8_t data_uid[256 + 16];
    int data_uid_present;
    sm_mode_t mode;
    sm_pointer_mode_t pointer_mode;
    uint32_t page_addr;
    uint32_t byte_addr;
    uint32_t addr_load_ptr;
    uint8_t status;
    uint8_t accumulated_status;
    bool mode_3065;
    uint32_t program_byte_count;
} smc_state_image_t;

int smc_state_save(const smc_t *s, FILE *f) {
    if (!s || !f) return 0;
    smc_state_image_t st;
    memset(&st, 0, sizeof(st));
    st.data_size = s->data_size;
    memcpy(st.header, s->header, sizeof(st.header));
    st.header_size = s->header_size;
    st.dirty = s->dirty;
    st.page_data_size = s->page_data_size;
    st.page_total_size = s->page_total_size;
    st.num_pages = s->num_pages;
    st.log2_pages_per_block = s->log2_pages_per_block;
    st.col_address_cycles = s->col_address_cycles;
    st.row_address_cycles = s->row_address_cycles;
    st.sequential_row_read = s->sequential_row_read;
    memcpy(st.id, s->id, sizeof(st.id));
    st.id_len = s->id_len;
    memcpy(st.data_uid, s->data_uid, sizeof(st.data_uid));
    st.data_uid_present = s->data_uid_present;
    st.mode = s->mode;
    st.pointer_mode = s->pointer_mode;
    st.page_addr = s->page_addr;
    st.byte_addr = s->byte_addr;
    st.addr_load_ptr = s->addr_load_ptr;
    st.status = s->status;
    st.accumulated_status = s->accumulated_status;
    st.mode_3065 = s->mode_3065;
    st.program_byte_count = s->program_byte_count;
    if (fwrite(&st, 1, sizeof(st), f) != sizeof(st)) return 0;
    if (s->data_size && s->data && fwrite(s->data, 1, s->data_size, f) != s->data_size) return 0;
    if (s->page_total_size && s->page_reg && fwrite(s->page_reg, 1, s->page_total_size, f) != s->page_total_size) return 0;
    return 1;
}

int smc_state_load(smc_t *s, FILE *f) {
    if (!s || !f) return 0;
    smc_state_image_t st;
    if (fread(&st, 1, sizeof(st), f) != sizeof(st)) return 0;
    if (st.data_size > (size_t)128u * 1024u * 1024u || st.page_total_size > 2112u) return 0;
    uint8_t *data = NULL;
    uint8_t *page = NULL;
    if (st.data_size) {
        data = (uint8_t *)malloc(st.data_size);
        if (!data) return 0;
        if (fread(data, 1, st.data_size, f) != st.data_size) { free(data); return 0; }
    }
    if (st.page_total_size) {
        page = (uint8_t *)malloc(st.page_total_size);
        if (!page) { free(data); return 0; }
        if (fread(page, 1, st.page_total_size, f) != st.page_total_size) { free(data); free(page); return 0; }
    }
    free(s->data);
    free(s->page_reg);
    memset(s, 0, sizeof(*s));
    s->data = data;
    s->data_size = st.data_size;
    memcpy(s->header, st.header, sizeof(s->header));
    s->header_size = st.header_size <= sizeof(s->header) ? st.header_size : 0u;
    s->dirty = st.dirty;
    s->page_data_size = st.page_data_size;
    s->page_total_size = st.page_total_size;
    s->num_pages = st.num_pages;
    s->log2_pages_per_block = st.log2_pages_per_block;
    s->col_address_cycles = st.col_address_cycles;
    s->row_address_cycles = st.row_address_cycles;
    s->sequential_row_read = st.sequential_row_read;
    memcpy(s->id, st.id, sizeof(s->id));
    s->id_len = st.id_len <= sizeof(s->id) ? st.id_len : 0u;
    s->page_reg = page;
    memcpy(s->data_uid, st.data_uid, sizeof(s->data_uid));
    s->data_uid_present = st.data_uid_present;
    s->mode = st.mode;
    s->pointer_mode = st.pointer_mode;
    s->page_addr = st.page_addr;
    s->byte_addr = st.byte_addr;
    s->addr_load_ptr = st.addr_load_ptr;
    s->status = st.status;
    s->accumulated_status = st.accumulated_status;
    s->mode_3065 = st.mode_3065;
    s->program_byte_count = st.program_byte_count;
    return 1;
}
