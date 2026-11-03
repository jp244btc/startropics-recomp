/*
 * sys.c - system glue: power/reset, frame loop, controllers, save states.
 */
#include "nes.h"
#include <stdio.h>
#include <stdlib.h>

int nes_load(const char *rom_path) {
    if (cart_load(rom_path) != 0) return -1;
    nes_power();
    return 0;
}

void nes_power(void) {
    memset(ram, 0, sizeof ram);
    sys.pad_shift[0] = sys.pad_shift[1] = 0;
    sys.strobe = false;
    mapper_init();
    ppu_reset();
    apu_reset();
    cpu_power();
    sys_sync();
}

void nes_reset(void) {
    /* RESET: mapper and PPU registers restart; APU keeps its timers, CPU keeps RAM */
    mapper_reset();
    ppu_reset();
    apu_write(0x4015, 0);
    apu_write(0x4017, apu.frame_reset_value);
    cpu_reset();
    sys_sync();
}

void nes_set_pad(int pad, uint8_t buttons) {
    sys.pad[pad & 1] = buttons;
}

void nes_run_frame(void) {
    ppu.frame_done = false;
    while (!ppu.frame_done) {
        if (cpu.int_pending) cpu_take_interrupt();
        if (sys.use_recomp) {
            if (!recomp_run()) { cpu_step(); sys.fallback_steps++; }
        } else {
            cpu_step();
        }
    }
}

/* ---- save states ---- */
#define STATE_MAGIC 0x53545331u  /* "STS1" */

typedef struct {
    uint32_t magic;
    uint32_t version;
    cpu_t    cpu;
    uint8_t  ram[2048];
    ppu_t    ppu;
    apu_t    apu;
    mapper_t mapper;
    uint8_t  wram[8192];
    uint8_t  chr_ram[8192];
    uint8_t  pad[2];
    uint8_t  pad_shift[2];
    uint8_t  strobe;
} state_t;

size_t nes_state_size(void) { return sizeof(state_t); }

int nes_state_save(void *buf, size_t size) {
    if (size < sizeof(state_t)) return -1;
    state_t *s = (state_t *)buf;
    memset(s, 0, sizeof *s);
    s->magic = STATE_MAGIC; s->version = 1;
    s->cpu = cpu; memcpy(s->ram, ram, sizeof ram);
    s->ppu = ppu; s->apu = apu; s->mapper = mapper;
    memcpy(s->wram, cart.wram, sizeof s->wram);
    memcpy(s->chr_ram, cart.chr_ram, sizeof s->chr_ram);
    s->pad[0] = sys.pad[0]; s->pad[1] = sys.pad[1];
    s->pad_shift[0] = sys.pad_shift[0]; s->pad_shift[1] = sys.pad_shift[1];
    s->strobe = sys.strobe;
    return 0;
}

int nes_state_load(const void *buf, size_t size) {
    if (size < sizeof(state_t)) return -1;
    const state_t *s = (const state_t *)buf;
    if (s->magic != STATE_MAGIC || s->version != 1) return -1;
    cpu = s->cpu; memcpy(ram, s->ram, sizeof ram);
    ppu = s->ppu; apu = s->apu; mapper = s->mapper;
    memcpy(cart.wram, s->wram, sizeof cart.wram);
    memcpy(cart.chr_ram, s->chr_ram, sizeof cart.chr_ram);
    sys.pad[0] = s->pad[0]; sys.pad[1] = s->pad[1];
    sys.pad_shift[0] = s->pad_shift[0]; sys.pad_shift[1] = s->pad_shift[1];
    sys.strobe = s->strobe != 0;
    apu.ring_read = apu.ring_write = 0;
    mapper_update_pointers();
    return 0;
}
