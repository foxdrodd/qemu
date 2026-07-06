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
#define PVR_BASE        0x005f8000       /* PowerVR2 display registers */

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
 * The kernel derives its irq from evt2irq(INTEVT) = INTEVT >> 5, and QEMU's
 * vectors_irl maps chain index i to INTEVT 0x200 + i*0x20, so:
 *   IRQ13 -> INTEVT 0x3a0 -> index 13 -> 13 ^ 15 = 2
 *   IRQ11 -> INTEVT 0x360 -> index 11 -> 11 ^ 15 = 4
 *   IRQ9  -> INTEVT 0x320 -> index  9 ->  9 ^ 15 = 6
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
     * irl_enc = index ^ 15:  IRQ13->13^15=2, IRQ11->11^15=4, IRQ9->9^15=6 */
    static const int emr_idx[3] = { 0, 5, 10 };
    static const int irl_enc[3] = { 2, 4, 6 };
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

/* Holly hardware-event line numbers used by on-board peripherals. */
#define HOLLY_EV_GDROM_DMA 14   /* ISTNRM bit 14 -> IRQ13 */
#define HOLLY_EV_GDROM_CMD 32   /* ISTEXT bit  0 -> IRQ11 */
#define HOLLY_EV_LAN       34   /* ISTEXT bit  2 -> IRQ11 (G2 external) */

/* Sega LAN Adapter (HIT-0300) G2 I/O base. */
#define LANADAPTER_BASE    0x00600400

static qemu_irq holly_event_irq(HollyState *s, int event)
{
    return &s->event[event];
}

/*
 * GD-ROM drive (G1 bus, ATA/ATAPI-like).  Register and command layout follow
 * Linux's drivers/cdrom/gdrom.c exactly.  The drive completes commands
 * asynchronously via a timer: a command write sets BSY, and the timer clears
 * BSY and posts the result, matching the driver's "wait for BSY to set, then
 * clear" polling (gdrom_wait_busy_sleeps).
 */
#define GDROM_BASE  0x005f7000
#define GDROM_SIZE  0x500

/* register offsets from GDROM_BASE */
#define GDR_ALTSTATUS 0x018     /* r: status (no int ack) ; w: device control */
#define GDR_DATA      0x080     /* 16-bit PIO data port */
#define GDR_ERROR     0x084     /* r: error/sense ; w: features */
#define GDR_INTSEC    0x088
#define GDR_SECNUM    0x08c
#define GDR_BCL       0x090     /* byte count low */
#define GDR_BCH       0x094     /* byte count high */
#define GDR_DSEL      0x098
#define GDR_STATUSCMD 0x09c     /* r: status (acks int) ; w: command */
#define GDR_DMA_ADDR   0x404
#define GDR_DMA_LEN    0x408
#define GDR_DMA_DIR    0x40c
#define GDR_DMA_ENABLE 0x414
#define GDR_DMA_STATUS 0x418
#define GDR_DMA_WAIT   0x4a0
#define GDR_DMA_ACCESS 0x4b8
#define GDR_RESET      0x4e4

/* ATA status bits */
#define GDS_BSY  0x80
#define GDS_DRDY 0x40
#define GDS_DSC  0x10
#define GDS_DRQ  0x08
#define GDS_ERR  0x01

/* ATA / SPI commands */
#define GDC_EXECDIAG  0x90
#define GDC_PACKET    0xa0
#define GDC_IDDEV     0xa1
#define GDC_SETFEAT   0xef

/* SPI (packet) sub-commands (packet[0]) */
#define SPI_TESTUNIT  0x00
#define SPI_REQSTAT   0x10
#define SPI_REQSENSE  0x13
#define SPI_READTOC   0x14
#define SPI_CDREAD    0x30
#define SPI_PREPDISK  0x70

#define GDROM_CMD_DELAY_NS 100000       /* ~0.1 ms command latency */
#define GDROM_SECTOR      2048
#define GDROM_SESSION_FAD 150           /* LBA -> FAD offset (2s pregap) */
#define GDROM_DATA_LBA    11702         /* CD-R data-track start (genisoimage -C) */
#define GDROM_TOC_BYTES   408           /* sizeof(struct gdromtoc) */

enum { PHASE_IDLE, PHASE_PACKET };

typedef struct GdromState {
    MemoryRegion iomem;
    qemu_irq irq_cmd;
    qemu_irq irq_dma;
    QEMUTimer *cmd_timer;
    QEMUTimer *dma_timer;
    BlockBackend *blk;
    uint32_t n_sectors;             /* disc image size in 2048-byte sectors */
    uint32_t data_lba;              /* data-track start LBA */

    uint8_t status;
    uint8_t error;
    uint8_t feature;
    uint8_t intsec;
    uint8_t secnum;
    uint8_t bcl, bch;
    uint8_t dsel;
    uint8_t pending_cmd;
    int post_cmd_irq;

    int phase;
    uint8_t packet[12];
    int packet_idx;

    /* PIO data-in buffer (IDENTIFY / TOC / sense) */
    uint8_t  pio[GDROM_SECTOR];
    uint32_t pio_len;
    uint32_t pio_idx;

    /* pending DMA read request */
    uint32_t read_fad, read_count;
    int dma_pending;

    /* DMA engine registers */
    uint32_t dma_addr, dma_len, dma_dir, dma_enable;
} GdromState;

/* Fill an ATA IDENTIFY-style GD-ROM device-id block (see struct gdrom_id). */
static void gdrom_fill_id(GdromState *s)
{
    memset(s->pio, 0, 80);
    memcpy(&s->pio[16], "SEGA ENTERPRISES", 16);  /* mname    */
    memcpy(&s->pio[32], "CD-ROM DRIVE    ", 16);  /* modname  */
    memcpy(&s->pio[48], "1.00            ", 16);  /* firmver  */
    s->pio_len = 80;
    s->pio_idx = 0;
}

/*
 * Encode a TOC entry as the driver decodes it (drivers/cdrom/gdrom.c):
 *   byte0 = (ctrl<<4)|adr, bytes1-3 = FAD big-endian.
 * get_entry_lba() computes cpu_to_be32(entry & 0xffffff00) - 150.
 */
static uint32_t gdrom_toc_entry(uint8_t ctrl_adr, uint32_t fad)
{
    return ctrl_adr
         | (((fad >> 16) & 0xff) << 8)
         | (((fad >> 8) & 0xff) << 16)
         | ((uint32_t)(fad & 0xff) << 24);
}

/* Synthesize a single-data-track TOC into the PIO buffer. */
static void gdrom_build_toc(GdromState *s)
{
    uint32_t data_fad = s->data_lba + GDROM_SESSION_FAD;
    uint32_t leadout_fad = data_fad + s->n_sectors;
    uint32_t entry[99];
    uint32_t hdr[3];
    int i;

    for (i = 0; i < 99; i++) {
        entry[i] = 0xffffffff;
    }
    /* Track 3 is the data track (ctrl=4 "data", adr=1). */
    entry[2] = gdrom_toc_entry(0x41, data_fad);
    hdr[0] = 3 << 8;                             /* first track */
    hdr[1] = 3 << 8;                             /* last track  */
    hdr[2] = gdrom_toc_entry(0x41, leadout_fad); /* leadout     */

    memset(s->pio, 0, GDROM_TOC_BYTES);
    memcpy(s->pio, entry, sizeof(entry));
    memcpy(s->pio + sizeof(entry), hdr, sizeof(hdr));
    s->pio_len = GDROM_TOC_BYTES;
    s->pio_idx = 0;
}

/* A packet (SPI) command has been fully received: act on packet[0]. */
static void gdrom_packet_dispatch(GdromState *s)
{
    switch (s->packet[0]) {
    case SPI_READTOC:
        gdrom_build_toc(s);
        break;
    case SPI_REQSENSE:
        memset(s->pio, 0, 10);          /* NO_SENSE */
        s->pio_len = 10;
        s->pio_idx = 0;
        break;
    case SPI_CDREAD:
        /* Latch the read; the transfer starts on the DMA_STATUS write. */
        s->read_fad = (s->packet[2] << 16) | (s->packet[3] << 8) | s->packet[4];
        s->read_count = (s->packet[8] << 16) | (s->packet[9] << 8) | s->packet[10];
        s->dma_pending = 1;
        return;                         /* no PIO completion */
    case SPI_TESTUNIT:
    case SPI_PREPDISK:
    default:
        s->pio_len = 0;                 /* status-only command */
        s->pio_idx = 0;
        break;
    }
    /* PIO/status command: complete after a short delay and post CMD IRQ. */
    s->status |= GDS_BSY;
    s->status &= ~GDS_DRQ;
    s->pending_cmd = GDC_PACKET;
    s->post_cmd_irq = 1;
    timer_mod(s->cmd_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + GDROM_CMD_DELAY_NS);
}

/* Perform a latched CD read into guest memory, then post the DMA IRQ. */
static void gdrom_do_dma(GdromState *s)
{
    uint32_t data_fad = s->data_lba + GDROM_SESSION_FAD;
    uint8_t sec[GDROM_SECTOR];
    dma_addr_t addr = s->dma_addr;
    uint32_t i;

    for (i = 0; i < s->read_count; i++) {
        uint32_t fad = s->read_fad + i;
        uint32_t rel = fad - data_fad;

        memset(sec, 0, sizeof(sec));
        if (s->blk && fad >= data_fad && rel < s->n_sectors) {
            blk_pread(s->blk, (int64_t)rel * GDROM_SECTOR, GDROM_SECTOR, sec, 0);
        }
        dma_memory_write(&address_space_memory, addr, sec, GDROM_SECTOR,
                         MEMTXATTRS_UNSPECIFIED);
        addr += GDROM_SECTOR;
    }
    s->dma_pending = 0;
    s->status |= GDS_BSY;
    s->pending_cmd = 0;
    timer_mod(s->dma_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + GDROM_CMD_DELAY_NS);
}

static void gdrom_cmd_complete(void *opaque)
{
    GdromState *s = opaque;

    s->status &= ~GDS_BSY;
    s->status |= GDS_DRDY | GDS_DSC;

    switch (s->pending_cmd) {
    case GDC_EXECDIAG:
        s->error = 0x01;        /* device 0 present, device 1 absent */
        break;
    case GDC_IDDEV:
        gdrom_fill_id(s);
        break;
    case GDC_SETFEAT:
        s->error = 0x00;
        break;
    default:
        break;
    }

    if (s->post_cmd_irq) {
        s->post_cmd_irq = 0;
        qemu_set_irq(s->irq_cmd, 1);    /* Holly latches the ESR bit (edge) */
    }
}

static void gdrom_dma_complete(void *opaque)
{
    GdromState *s = opaque;

    s->status &= ~GDS_BSY;
    s->status |= GDS_DRDY | GDS_DSC;
    s->error = 0x00;
    qemu_set_irq(s->irq_dma, 1);
}

static uint64_t gdrom_read(void *opaque, hwaddr addr, unsigned int size)
{
    GdromState *s = opaque;

    switch (addr) {
    case GDR_ALTSTATUS:
    case GDR_STATUSCMD:
        return s->status;
    case GDR_ERROR:
        return s->error;
    case GDR_INTSEC:
        return s->intsec;
    case GDR_SECNUM:
        return s->secnum;
    case GDR_BCL:
        return s->bcl;
    case GDR_BCH:
        return s->bch;
    case GDR_DSEL:
        return s->dsel;
    case GDR_DATA: {
        uint16_t w = 0;
        if (s->pio_idx < s->pio_len) {
            w = s->pio[s->pio_idx];
            if (s->pio_idx + 1 < s->pio_len) {
                w |= s->pio[s->pio_idx + 1] << 8;
            }
            s->pio_idx += 2;
        }
        return w;
    }
    default:
        return 0;
    }
}

static void gdrom_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned int size)
{
    GdromState *s = opaque;

    switch (addr) {
    case GDR_ALTSTATUS:
        break;                  /* device control (IRQ_WAIT etc.) */
    case GDR_ERROR:
        s->feature = val;
        break;
    case GDR_INTSEC:
        s->intsec = val;
        break;
    case GDR_SECNUM:
        s->secnum = val;
        break;
    case GDR_BCL:
        s->bcl = val;
        break;
    case GDR_BCH:
        s->bch = val;
        break;
    case GDR_DSEL:
        s->dsel = val;
        break;
    case GDR_DATA:
        if (s->phase == PHASE_PACKET && s->packet_idx <= 10) {
            s->packet[s->packet_idx++] = val & 0xff;
            s->packet[s->packet_idx++] = (val >> 8) & 0xff;
            if (s->packet_idx >= 12) {
                s->phase = PHASE_IDLE;
                gdrom_packet_dispatch(s);
            }
        }
        break;
    case GDR_STATUSCMD:
        if ((val & 0xff) == GDC_PACKET) {
            /* Request the 12-byte command packet: assert DRQ, clear BSY. */
            s->phase = PHASE_PACKET;
            s->packet_idx = 0;
            s->status = GDS_DRDY | GDS_DRQ;
        } else {
            s->pending_cmd = val & 0xff;
            s->post_cmd_irq = 0;
            s->status |= GDS_BSY;
            s->status &= ~GDS_ERR;
            timer_mod(s->cmd_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + GDROM_CMD_DELAY_NS);
        }
        break;
    case GDR_RESET:
        s->status = GDS_DRDY | GDS_DSC;
        s->error = 0x01;
        s->phase = PHASE_IDLE;
        s->pio_len = s->pio_idx = 0;
        break;
    case GDR_DMA_ADDR:
        s->dma_addr = val;
        break;
    case GDR_DMA_LEN:
        s->dma_len = val;
        break;
    case GDR_DMA_DIR:
        s->dma_dir = val;
        break;
    case GDR_DMA_ENABLE:
        s->dma_enable = val;
        break;
    case GDR_DMA_STATUS:
        if ((val & 1) && s->dma_pending) {
            gdrom_do_dma(s);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps gdrom_ops = {
    .read = gdrom_read,
    .write = gdrom_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static GdromState *gdrom_init(MemoryRegion *sysmem, qemu_irq irq_cmd,
                              qemu_irq irq_dma, BlockBackend *blk)
{
    GdromState *s = g_new0(GdromState, 1);

    s->irq_cmd = irq_cmd;
    s->irq_dma = irq_dma;
    s->blk = blk;
    s->data_lba = GDROM_DATA_LBA;
    s->status = GDS_DRDY | GDS_DSC;
    s->error = 0x01;
    s->cmd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gdrom_cmd_complete, s);
    s->dma_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gdrom_dma_complete, s);

    if (blk) {
        int64_t len = blk_getlength(blk);
        if (len > 0) {
            s->n_sectors = len / GDROM_SECTOR;
        }
    }

    memory_region_init_io(&s->iomem, NULL, &gdrom_ops, s, "dc-gdrom",
                          GDROM_SIZE);
    memory_region_add_subregion(sysmem, GDROM_BASE, &s->iomem);
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
    HollyState *holly;
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
    holly = holly_init(address_space_mem, sh7750_irl(s));

    /* GD-ROM drive on the G1 bus, interrupts routed through Holly.
     * The disc image is supplied via -drive if=none,file=<disc>. */
    dinfo = drive_get(IF_NONE, 0, 0);
    gdrom_init(address_space_mem,
               holly_event_irq(holly, HOLLY_EV_GDROM_CMD),
               holly_event_irq(holly, HOLLY_EV_GDROM_DMA),
               dinfo ? blk_by_legacy_dinfo(dinfo) : NULL);

    /* Sega LAN Adapter (HIT-0300) on the G2 bus, IRQ via Holly event 34. */
    dc_lanadapter_init(LANADAPTER_BASE, holly_event_irq(holly, HOLLY_EV_LAN));

    /* PowerVR2 display: scans out the framebuffer from VRAM to a display. */
    dc_pvr_init(PVR_BASE, vram);

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

static void dreamcast_machine_init(MachineClass *mc)
{
    mc->desc = "Sega Dreamcast";
    mc->init = dreamcast_init;
    /* The SH7091 is an SH7750-class core; reuse sh7750r until a dedicated
     * sh7091 CPU type is added.  MMU/TLB behaviour is identical. */
    mc->default_cpu_type = TYPE_SH7750R_CPU;
}

DEFINE_MACHINE("dreamcast", dreamcast_machine_init)
