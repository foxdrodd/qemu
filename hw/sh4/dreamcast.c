/*
 * Sega Dreamcast (SH7091 / Holly / PowerVR2 CLX2) emulation
 *
 * Copyright (c) 2026 Florian Fuchs
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "target/sh4/cpu.h"
#include "hw/core/sysbus.h"
#include "hw/sh4/sh.h"
#include "system/reset.h"
#include "system/system.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/irq.h"
#include "sh7750_regs.h"
#include "elf.h"

/*
 * Dreamcast physical memory map (SH-4 Area layout).
 * System RAM lives in Area 3 at 0x0c000000 -- the same base the r2d board
 * uses -- so the -kernel load path below is identical to r2d's.
 */
#define SDRAM_BASE     0x0c000000        /* Area 3: 16 MB main RAM           */
#define SDRAM_SIZE     (16 * MiB)
#define VRAM_BASE      0x05000000        /* PowerVR2 video RAM: 8 MB         */
#define VRAM_SIZE      (8  * MiB)
#define AICA_RAM_BASE  0x00800000        /* AICA sound RAM: 2 MB             */
#define AICA_RAM_SIZE  (2  * MiB)

#define HOLLY_INTC_BASE 0x005f6900       /* Holly ASIC interrupt regs (ESR/EMR) */
#define HOLLY_INTC_SIZE 0x40
#define PVR_BASE        0x005f8000       /* PowerVR2 / TA registers     [M2] */

/* CONFIG_BOOT_LINK_OFFSET of the Dreamcast Linux kernel. */
#define LINUX_LOAD_OFFSET  0x00800000

typedef struct ResetData {
    SuperHCPU *cpu;
    uint32_t vector;
} ResetData;

static void main_cpu_reset(void *opaque)
{
    ResetData *s = (ResetData *)opaque;
    CPUSH4State *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));
    env->pc = s->vector;
}

/*
 * Holly (System ASIC) interrupt controller.
 *
 * Peripherals raise "hardware events"; each event maps to one of three SH-4
 * interrupt levels (IRQ 13 / 11 / 9) via the Event Mask Registers.  This
 * mirrors Linux's arch/sh/boards/mach-dreamcast/irq.c:
 *
 *   ESR (status, write-1-to-ack):  0x6900 NRM, 0x6904 EXT, 0x6908 ERR
 *   EMR (mask): diagonal 0x6910 (->IRQ13), 0x6924 (->IRQ11), 0x6938 (->IRQ9)
 *
 * A level fires when (ESR & EMR) != 0.  We drive the SH-4 IRL lines through
 * sh7750_irl(); its encoded value is (chain_index ^ 15), where chain index N
 * selects SH-4 interrupt level N+1 (see vectors_irl[] in sh7750.c):
 *   IRQ13 -> level 13 -> index 12 -> 12 ^ 15 = 3
 *   IRQ11 -> level 11 -> index 10 -> 10 ^ 15 = 5
 *   IRQ9  -> level  9 -> index  8 ->  8 ^ 15 = 7
 * Passing 0 (index 15, unused) deasserts all IRL sources.
 */
#define HOLLY_NR_EVENTS 96

typedef struct HollyState {
    uint32_t esr[3];        /* 0x00/0x04/0x08 : ISTNRM / ISTEXT / ISTERR   */
    uint32_t emr[12];       /* 0x10..0x3c     : event mask registers       */
    qemu_irq irl;           /* encoded IRL line into the SH-4 INTC         */
    IRQState event[HOLLY_NR_EVENTS];
    MemoryRegion iomem;
} HollyState;

static void holly_update_irl(HollyState *s)
{
    /* diagonal EMR index and IRL-encoded value for IRQ13, IRQ11, IRQ9.
     * irl_enc = index ^ 15:  IRQ13->12^15=3, IRQ11->10^15=5, IRQ9->8^15=7 */
    static const int emr_idx[3] = { 0, 5, 10 };
    static const int irl_enc[3] = { 3, 5, 7 };
    int i;

    for (i = 0; i < 3; i++) {          /* i == 0 (IRQ13) is highest priority */
        if (s->esr[i] & s->emr[emr_idx[i]]) {
            qemu_set_irq(s->irl, irl_enc[i]);
            return;
        }
    }
    qemu_set_irq(s->irl, 0);           /* nothing pending: deassert all */
}

/* A peripheral pulses event line n (0..95) to latch its ESR bit. */
static void holly_event_set(void *opaque, int n, int level)
{
    HollyState *s = opaque;

    if (level) {
        s->esr[n >> 5] |= 1u << (n & 31);
        holly_update_irl(s);
    }
}

static uint64_t holly_read(void *opaque, hwaddr addr, unsigned int size)
{
    HollyState *s = opaque;

    if (addr < 0x0c) {
        return s->esr[addr >> 2];
    }
    if (addr >= 0x10 && addr < 0x40) {
        return s->emr[(addr - 0x10) >> 2];
    }
    return 0;
}

static void holly_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned int size)
{
    HollyState *s = opaque;

    if (addr < 0x0c) {
        s->esr[addr >> 2] &= ~(uint32_t)val;   /* write-1-to-acknowledge */
    } else if (addr >= 0x10 && addr < 0x40) {
        s->emr[(addr - 0x10) >> 2] = val;
    }
    holly_update_irl(s);
}

static const MemoryRegionOps holly_ops = {
    .read = holly_read,
    .write = holly_write,
    .impl.min_access_size = 4,       /* kernel uses 32-bit inl/outl */
    .impl.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static HollyState *holly_init(MemoryRegion *sysmem, qemu_irq irl)
{
    HollyState *s = g_new0(HollyState, 1);

    s->irl = irl;
    memory_region_init_io(&s->iomem, NULL, &holly_ops, s, "dc-holly-intc",
                          HOLLY_INTC_SIZE);
    memory_region_add_subregion(sysmem, HOLLY_INTC_BASE, &s->iomem);
    qemu_init_irqs(s->event, HOLLY_NR_EVENTS, holly_event_set, s);
    return s;
}

/* Map SH-4 P1/P2 kernel virtual addresses (0x8xxxxxxx / 0xAxxxxxxx) to RAM. */
static uint64_t dc_kernel_translate(void *opaque, uint64_t addr)
{
    return addr & 0x1fffffff;
}

static void dreamcast_init(MachineState *machine)
{
    const char *kernel_filename = machine->kernel_filename;
    SuperHCPU *cpu;
    CPUSH4State *env;
    ResetData *reset_info;
    struct SH7750State *s;
    MemoryRegion *address_space_mem = get_system_memory();
    MemoryRegion *sdram = g_new(MemoryRegion, 1);
    MemoryRegion *vram  = g_new(MemoryRegion, 1);
    MemoryRegion *aram  = g_new(MemoryRegion, 1);

    cpu = SUPERH_CPU(cpu_create(machine->cpu_type));
    env = &cpu->env;

    reset_info = g_new0(ResetData, 1);
    reset_info->cpu = cpu;
    reset_info->vector = env->pc;
    qemu_register_reset(main_cpu_reset, reset_info);

    /* Main memory, video RAM and audio RAM. */
    memory_region_init_ram(sdram, NULL, "dc.sdram", SDRAM_SIZE, &error_fatal);
    memory_region_add_subregion(address_space_mem, SDRAM_BASE, sdram);
    memory_region_init_ram(vram, NULL, "dc.vram", VRAM_SIZE, &error_fatal);
    memory_region_add_subregion(address_space_mem, VRAM_BASE, vram);
    memory_region_init_ram(aram, NULL, "dc.aram", AICA_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(address_space_mem, AICA_RAM_BASE, aram);

    /*
     * SH7091 on-chip peripherals: INTC, TMU timers, cache/TLB, and the two
     * serial ports.  SCIF is serial_hd(1) and is the Dreamcast Linux console.
     */
    s = sh7750_init(cpu, address_space_mem);

    /* Holly System ASIC interrupt controller, driving the SH-4 IRL lines. */
    holly_init(address_space_mem, sh7750_irl(s));

    (void)PVR_BASE;     /* [M2] PowerVR2 framebuffer scanout */

    /*
     * Load the kernel.  The Dreamcast Linux vmlinux is an SH ELF linked in the
     * P1 window (0x8c000000); dc_kernel_translate() maps that to physical RAM.
     * A raw binary is accepted as a fallback, run from the start of RAM.
     */
    if (kernel_filename) {
        uint64_t entry = 0;
        ssize_t kernel_size;

        kernel_size = load_elf(kernel_filename, NULL, dc_kernel_translate, NULL,
                               &entry, NULL, NULL, NULL,
                               ELFDATA2LSB, EM_SH, 0, 0);
        if (kernel_size < 0) {
            /* Not an ELF: load as a raw image at the base of RAM. */
            entry = (SDRAM_BASE + LINUX_LOAD_OFFSET) | 0xa0000000;
            kernel_size = load_image_targphys(kernel_filename,
                                              SDRAM_BASE + LINUX_LOAD_OFFSET,
                                              SDRAM_SIZE - LINUX_LOAD_OFFSET,
                                              NULL);
        }
        if (kernel_size < 0) {
            error_report("qemu: could not load kernel '%s'", kernel_filename);
            exit(1);
        }
        reset_info->vector = entry;

        /* Basic bus-state config the firmware would normally do (cs3 SDRAM). */
        address_space_stl(&address_space_memory, SH7750_BCR1, 1 << 3,
                          MEMTXATTRS_UNSPECIFIED, NULL);
        address_space_stw(&address_space_memory, SH7750_BCR2, 3 << (3 * 2),
                          MEMTXATTRS_UNSPECIFIED, NULL);
    }
}

static void dreamcast_machine_init(MachineClass *mc)
{
    mc->desc = "Sega Dreamcast";
    mc->init = dreamcast_init;
    /* The SH7091 is an SH7750-class core; reuse sh7750r until a dedicated
     * sh7091 CPU type is added.  MMU/TLB behaviour is identical. */
    mc->default_cpu_type = TYPE_SH7750R_CPU;
}

DEFINE_MACHINE("dreamcast", dreamcast_machine_init)
