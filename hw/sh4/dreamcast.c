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
#include "qemu/timer.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/dma.h"
#include "net/net.h"

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
#define CH2DMA_BASE     0x005f6800       /* Holly CH2 ("PVR") DMA registers */
#define PVR_BASE        0x005f8000       /* PowerVR2 display registers */
#define TA_FIFO_BASE    0x10000000       /* PowerVR2 tile-accelerator input FIFO */

/* CONFIG_BOOT_LINK_OFFSET of the Dreamcast Linux kernel. */
#define LINUX_LOAD_OFFSET  0x00800000

/* initrd placement and the SH boot-parameter page (boot_params_page). */
#define INITRD_OFFSET      0x00c00000            /* MEMORY_START + 12 MB */
#define BOOT_PARAMS_PHYS   (SDRAM_BASE + 0x1000) /* boot_params_page @ 0x8c001000 */

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

/* Sega LAN Adapter (HIT-0300) G2 I/O base. */
#define LANADAPTER_BASE    0x00600400
/* Maple bus controller register base. */
#define MAPLE_BASE         0x005f6c00

/*
 * Holly (System ASIC) interrupt controller: hw/intc/dreamcast_holly.c
 * (TYPE_DC_HOLLY).  Event line numbers (HOLLY_EV_*) and dc_holly_init() are
 * declared in hw/sh4/sh.h; peripherals connect their IRQ outputs to
 * qdev_get_gpio_in(holly, HOLLY_EV_*).
 */

/*
 * Holly CH2 ("PVR") DMA: hw/dma/dreamcast_ch2dma.c (TYPE_DC_CH2DMA).
 * Cascades SH4 DMAC channel 2 into VRAM; dc_ch2dma_init() is in sh.h.
 */

/*
 * GD-ROM drive: hw/block/dreamcast_gdrom.c (TYPE_DC_GDROM).  dc_gdrom_init()
 * and the boot-firmware sector readers are declared in sh.h.
 */
#define GDROM_BASE  0x005f7000

/* Board boot-from-disc firmware constants (ISO9660 geometry). */
#define GDROM_SECTOR   2048
#define ISO_PVD_LBA    16           /* ISO9660 PVD at data-track sector 16 */

/*
 * 1ST_READ.BIN descrambler - the exact inverse of sh-boot's scramble.c, which
 * the DC BIOS/IP.BIN normally performs.  A 16-bit LCG drives a Fisher-Yates
 * shuffle of 32-byte slices, over windows shrinking from 2 MB down to 32 bytes.
 */
#define DC_SCRAMBLE_MAXCHUNK (2048 * 1024)

static uint32_t dc_scr_seed;

static uint32_t dc_scr_rand(void)
{
    dc_scr_seed = (dc_scr_seed * 2109 + 9273) & 0x7fff;
    return (dc_scr_seed + 0xc000) & 0xffff;
}

static void dc_descramble(const uint8_t *src, uint8_t *dst, uint32_t size)
{
    uint32_t filesz = size, chunksz;
    const uint8_t *sp = src;
    uint8_t *dp = dst;
    int *idx = g_new(int, DC_SCRAMBLE_MAXCHUNK / 32);

    dc_scr_seed = size & 0xffff;
    for (chunksz = DC_SCRAMBLE_MAXCHUNK; chunksz >= 32; chunksz >>= 1) {
        while (filesz >= chunksz) {
            int sz = chunksz / 32, i;

            for (i = 0; i < sz; i++) {
                idx[i] = i;
            }
            for (i = sz - 1; i >= 0; --i) {
                uint32_t x = (dc_scr_rand() * (uint32_t)i) >> 16;
                int tmp = idx[i];
                idx[i] = idx[x];
                idx[x] = tmp;
                memcpy(dp + 32 * idx[i], sp, 32);
                sp += 32;
            }
            filesz -= chunksz;
            dp += chunksz;
        }
    }
    if (filesz) {                       /* trailing partial slice, verbatim */
        memcpy(dp, sp, filesz);
    }
    g_free(idx);
}

/*
 * Boot a self-contained disc the way the BIOS would: find 1ST_READ.BIN in the
 * ISO9660 root directory, descramble it, and stage it at its 0x8c010000 load
 * address.  Returns the entry PC, or 0 if the disc is not bootable.  (Dreamcast
 * Linux's 1ST_READ.BIN carries the kernel + boot params and reads its rootfs
 * over the hardware GD-ROM registers, so no BIOS syscall HLE is required.)
 */
static uint64_t dc_boot_disc(DeviceState *gd)
{
    uint8_t sec[GDROM_SECTOR];
    uint8_t *dirbuf, *filebuf, *dst;
    uint32_t data_lba = dc_gdrom_data_lba(gd);
    uint32_t root_lba, root_size, file_lba = 0, file_size = 0, nsec, pos;

    /* Primary Volume Descriptor at data-track sector 16; the root directory
     * record is the 34-byte field at PVD offset 156.  A missing disc makes
     * the first sector read fail. */
    if (!dc_gdrom_read_logical(gd, data_lba + ISO_PVD_LBA, 1, sec) ||
        sec[0] != 0x01 || memcmp(&sec[1], "CD001", 5) != 0) {
        return 0;
    }
    root_lba  = ldl_le_p(&sec[156 + 2]);
    root_size = ldl_le_p(&sec[156 + 10]);
    if (!root_size) {
        return 0;
    }

    nsec = DIV_ROUND_UP(root_size, GDROM_SECTOR);
    dirbuf = g_malloc(nsec * GDROM_SECTOR);
    if (!dc_gdrom_read_logical(gd, root_lba, nsec, dirbuf)) {
        g_free(dirbuf);
        return 0;
    }
    for (pos = 0; pos < root_size; ) {
        uint8_t rlen = dirbuf[pos];
        uint8_t nlen;
        const char *nm;

        if (rlen < 34) {                /* zero-pad up to the next sector */
            pos = ROUND_UP(pos + 1, GDROM_SECTOR);
            continue;
        }
        nlen = dirbuf[pos + 32];
        nm = (const char *)&dirbuf[pos + 33];
        if (nlen >= 12 && !g_ascii_strncasecmp(nm, "1ST_READ.BIN", 12)) {
            file_lba  = ldl_le_p(&dirbuf[pos + 2]);
            file_size = ldl_le_p(&dirbuf[pos + 10]);
            break;
        }
        pos += rlen;
    }
    g_free(dirbuf);
    if (!file_size) {
        return 0;
    }

    nsec = DIV_ROUND_UP(file_size, GDROM_SECTOR);
    filebuf = g_malloc(nsec * GDROM_SECTOR);
    if (!dc_gdrom_read_logical(gd, file_lba, nsec, filebuf)) {
        g_free(filebuf);
        return 0;
    }
    dst = g_malloc(file_size);
    dc_descramble(filebuf, dst, file_size);
    /* 0x8c010000 -> physical SDRAM_BASE + 0x10000; a ROM blob so it survives
     * the CPU reset that copies ROM images into RAM. */
    rom_add_blob_fixed("dc.1st_read", dst, file_size, SDRAM_BASE + 0x10000);
    g_free(filebuf);
    g_free(dst);
    return 0x8c010000;
}

/* Map SH-4 P1/P2 kernel virtual addresses (0x8xxxxxxx / 0xAxxxxxxx) to RAM. */
static uint64_t dc_kernel_translate(void *opaque, uint64_t addr)
{
    return addr & 0x1fffffff;
}

#define TYPE_DREAMCAST_MACHINE MACHINE_TYPE_NAME("dreamcast")
OBJECT_DECLARE_SIMPLE_TYPE(DreamcastMachineState, DREAMCAST_MACHINE)

struct DreamcastMachineState {
    MachineState parent_obj;
    bool vmu_lcd;               /* present the VMU LCD as a second display */
};

static void dreamcast_init(MachineState *machine)
{
    const char *kernel_filename = machine->kernel_filename;
    SuperHCPU *cpu;
    CPUSH4State *env;
    ResetData *reset_info;
    struct SH7750State *s;
    DeviceState *holly;
    DeviceState *ta;
    DeviceState *gd;
    DriveInfo *dinfo;
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
    holly = dc_holly_init(HOLLY_INTC_BASE, sh7750_irl(s));

    /* Holly CH2 ("PVR") DMA: cascades SH4 DMAC channel 2 into VRAM. */
    dc_ch2dma_init(CH2DMA_BASE, s,
                   qdev_get_gpio_in(holly, HOLLY_EV_CH2_DMA));

    /*
     * Report a VGA video cable: port-A bits 8-9 both low select CT_VGA, which
     * gives pvr2fb a progressive 640x480 VGA mode.  A TV cable would force
     * interlaced NTSC/PAL with strict broadcast timing that Xfbdev's mode-set
     * (FBIOPUT_VSCREENINFO) cannot satisfy.
     */
    sh7750_set_porta(s, 0x0300, 0x0000);

    /* GD-ROM drive on the G1 bus, interrupts routed through Holly.
     * The disc image is supplied via -drive if=none,file=<disc>. */
    dinfo = drive_get(IF_NONE, 0, 0);
    gd = dc_gdrom_init(GDROM_BASE,
                       qdev_get_gpio_in(holly, HOLLY_EV_GDROM_CMD),
                       qdev_get_gpio_in(holly, HOLLY_EV_GDROM_DMA),
                       dinfo ? blk_by_legacy_dinfo(dinfo) : NULL);

    /*
     * G2 networking.  The Broadband Adapter (RTL8139 behind the GAPS PCI
     * bridge, IRQ via Holly event 35) is the default; "-nic ...,model=
     * dc-lanadapter" selects the older Sega LAN Adapter instead.
     */
    if (qemu_find_nic_info("dc-lanadapter", false, NULL)) {
        dc_lanadapter_init(LANADAPTER_BASE,
                           qdev_get_gpio_in(holly, HOLLY_EV_LAN));
    } else {
        dc_gaps_init(qdev_get_gpio_in(holly, HOLLY_EV_EXTERNAL));
    }

    /*
     * PowerVR2 Tile Accelerator: parses the parameter FIFO at 0x10000000 and
     * rasterises the binned lists into VRAM on STARTRENDER, raising "end of
     * render (TSP)" (Holly event 2).  The display forwards TA_LIST_INIT and
     * STARTRENDER register writes to it.
     */
    ta = dc_ta_init(TA_FIFO_BASE, vram,
                    qdev_get_gpio_in(holly, HOLLY_EV_PVR_RENDER));

    /* PowerVR2 display: scans out VRAM and raises VSYNC (Holly event 5). */
    dc_pvr_init(PVR_BASE, vram, qdev_get_gpio_in(holly, HOLLY_EV_VSYNC), ta);

    /*
     * Maple bus: keyboard (port 0), mouse (port 1), DMA-complete IRQ via Holly
     * event 12.  A second "-drive if=none" (unit 1, a 128 KB image) attaches a
     * VMU in a controller's slot on port 2.
     */
    dinfo = drive_get(IF_NONE, 0, 1);
    dc_maple_init(MAPLE_BASE, qdev_get_gpio_in(holly, HOLLY_EV_MAPLE_DMA),
                  dinfo ? dc_vmu_new(blk_by_legacy_dinfo(dinfo),
                                     DREAMCAST_MACHINE(machine)->vmu_lcd)
                        : NULL);

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
    } else {
        /* No -kernel: boot the disc itself (descramble 1ST_READ.BIN). */
        uint64_t entry = dc_boot_disc(gd);

        if (!entry) {
            error_report("qemu: no -kernel and no bootable disc "
                         "(1ST_READ.BIN not found)");
            exit(1);
        }
        reset_info->vector = entry;
    }

    /* Basic bus-state config the firmware would normally do (cs3 SDRAM). */
    address_space_stl(&address_space_memory, SH7750_BCR1, 1 << 3,
                      MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stw(&address_space_memory, SH7750_BCR2, 3 << (3 * 2),
                      MEMTXATTRS_UNSPECIFIED, NULL);

    /*
     * Optional initrd / initramfs.  The SH kernel reads INITRD_START and
     * INITRD_SIZE from boot_params_page (arch/sh/kernel/setup.c): INITRD_START
     * is an offset from __MEMORY_START, at PARAM+0x10; INITRD_SIZE at PARAM+0x14;
     * LOADER_TYPE at PARAM+0x0c must be non-zero.
     */
    if (machine->initrd_filename) {
        ssize_t initrd_size;
        uint32_t *bp;

        initrd_size = load_image_targphys(machine->initrd_filename,
                                          SDRAM_BASE + INITRD_OFFSET,
                                          SDRAM_SIZE - INITRD_OFFSET, NULL);
        if (initrd_size < 0) {
            error_report("qemu: could not load initrd '%s'",
                         machine->initrd_filename);
            exit(1);
        }
        /*
         * boot_params_page lives inside the kernel ELF, which is loaded as a
         * ROM blob copied to RAM at reset.  Patch the blob's backing data so
         * INITRD_START/SIZE survive that copy (a direct RAM write would be
         * overwritten by the ROM reset).
         */
        bp = rom_ptr(BOOT_PARAMS_PHYS, 0x20);
        if (bp) {
            bp[0x0c / 4] = cpu_to_le32(1);              /* LOADER_TYPE      */
            bp[0x10 / 4] = cpu_to_le32(INITRD_OFFSET);  /* INITRD_START     */
            bp[0x14 / 4] = cpu_to_le32(initrd_size);    /* INITRD_SIZE      */
        } else {
            error_report("qemu: could not locate boot_params_page in kernel");
            exit(1);
        }
    }
}

static bool dreamcast_get_vmu_lcd(Object *obj, Error **errp)
{
    return DREAMCAST_MACHINE(obj)->vmu_lcd;
}

static void dreamcast_set_vmu_lcd(Object *obj, bool value, Error **errp)
{
    DREAMCAST_MACHINE(obj)->vmu_lcd = value;
}

static void dreamcast_machine_init(MachineClass *mc)
{
    mc->desc = "Sega Dreamcast";
    mc->init = dreamcast_init;
    /* The SH7091 is an SH7750-class core; reuse sh7750r until a dedicated
     * sh7091 CPU type is added.  MMU/TLB behaviour is identical. */
    mc->default_cpu_type = TYPE_SH7750R_CPU;

    object_class_property_add_bool(OBJECT_CLASS(mc), "vmu-lcd",
                                   dreamcast_get_vmu_lcd,
                                   dreamcast_set_vmu_lcd);
    object_class_property_set_description(OBJECT_CLASS(mc), "vmu-lcd",
        "Show the VMU LCD as a second display (needs a VMU -drive)");
}

DEFINE_MACHINE_EXTENDED("dreamcast", MACHINE, DreamcastMachineState,
                        dreamcast_machine_init, false, NULL)
