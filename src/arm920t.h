#ifndef GP32EMU_ARM920T_H
#define GP32EMU_ARM920T_H

#include "common.h"

typedef struct arm920t arm920t_t;

typedef uint8_t  (*arm_read8_fn)(void *user, uint32_t addr);
typedef uint16_t (*arm_read16_fn)(void *user, uint32_t addr);
typedef uint32_t (*arm_read32_fn)(void *user, uint32_t addr);
typedef void (*arm_write8_fn)(void *user, uint32_t addr, uint8_t value);
typedef void (*arm_write16_fn)(void *user, uint32_t addr, uint16_t value);
typedef void (*arm_write32_fn)(void *user, uint32_t addr, uint32_t value);
typedef uint8_t *(*arm_fastmem_fn)(void *user, uint32_t addr, size_t bytes, int write);
typedef void (*arm_log_fn)(void *user, const char *line);
typedef int (*arm_swi_fn)(void *user, arm920t_t *cpu, uint32_t imm, uint32_t pc, int thumb);

typedef struct arm_bus {
    arm_read8_fn read8;
    arm_read16_fn read16;
    arm_read32_fn read32;
    /* Optional direct physical-I/O word read used by the x64 JIT for identity-mapped S3C2400 MMIO. */
    arm_read32_fn read32_io;
    arm_write8_fn write8;
    arm_write16_fn write16;
    arm_write32_fn write32;
    arm_fastmem_fn fastmem;
    void *user;
} arm_bus_t;

arm920t_t *arm920t_create(const arm_bus_t *bus);
void arm920t_destroy(arm920t_t *cpu);
void arm920t_reset(arm920t_t *cpu, uint32_t vector);
uint32_t arm920t_run(arm920t_t *cpu, uint32_t cycles);
void arm920t_add_idle_cycles(arm920t_t *cpu, uint32_t cycles);
void arm920t_set_jit(arm920t_t *cpu, int enabled);
void arm920t_flush_jit(arm920t_t *cpu);
void arm920t_set_irq(arm920t_t *cpu, int state);
void arm920t_set_fiq(arm920t_t *cpu, int state);
void arm920t_set_trace(arm920t_t *cpu, int enabled, arm_log_fn log, void *user);
void arm920t_set_swi_handler(arm920t_t *cpu, arm_swi_fn fn, void *user);
void arm920t_set_reg(arm920t_t *cpu, unsigned reg, uint32_t value);
void arm920t_set_cpsr(arm920t_t *cpu, uint32_t value);
uint32_t arm920t_get_pc(const arm920t_t *cpu);
uint64_t arm920t_get_cycles(const arm920t_t *cpu);
uint32_t arm920t_get_reg(const arm920t_t *cpu, unsigned reg);
uint32_t arm920t_get_cpsr(const arm920t_t *cpu);
uint32_t arm920t_get_cp15(const arm920t_t *cpu, unsigned reg);
uint64_t arm920t_get_jit_hits(const arm920t_t *cpu);
uint64_t arm920t_get_jit_misses(const arm920t_t *cpu);
uint64_t arm920t_get_jit_fallbacks(const arm920t_t *cpu);
int arm920t_state_save(const arm920t_t *cpu, FILE *f);
int arm920t_state_load(arm920t_t *cpu, FILE *f);

#endif
