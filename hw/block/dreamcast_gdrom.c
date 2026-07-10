/*
 * Sega Dreamcast GD-ROM drive (G1 bus, ATA/ATAPI-like)
 *
 * Register and command layout follow Linux's drivers/cdrom/gdrom.c exactly.
 * The drive completes commands asynchronously via a timer: a command write
 * sets BSY, and the timer clears BSY and posts the result, matching the
 * driver's "wait for BSY to set, then clear" polling (gdrom_wait_busy_sleeps).
 *
 * The drive owns a BlockBackend holding the disc image (raw .iso or a cdi4dc
 * .cdi); gdrom_probe_disc() detects the layout at realize time.  The board's
 * boot-from-disc firmware reads logical sectors through dc_gdrom_read_logical().
 *
 * Copyright (c) 2026 Florian Fuchs
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/sh4/sh.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "qom/object.h"
#include "system/block-backend.h"
#include "system/dma.h"

#define TYPE_DC_GDROM "dc-gdrom"
OBJECT_DECLARE_SIMPLE_TYPE(DCGdromState, DC_GDROM)

#define GDROM_SIZE  0x500

/* register offsets from the GD-ROM base */
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

struct DCGdromState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq_cmd;
    qemu_irq irq_dma;
    QEMUTimer *cmd_timer;
    QEMUTimer *dma_timer;
    BlockBackend *blk;
    uint32_t n_sectors;             /* data-track length in 2048-byte sectors */
    uint32_t data_lba;              /* data-track start LBA */
    /* Physical geometry of the data track inside the image. Raw ISO:
     * off=0, raw=2048, hdr=0 (flat). CDI (cdi4dc Mode2/Form1): off=byte
     * offset of the LBA-0 sector, raw=2336 stored sector, hdr=8 subheader. */
    uint64_t data_off;              /* byte offset of data-track sector 0 */
    uint32_t raw_size;              /* stored bytes per sector */
    uint32_t sec_hdr;              /* bytes before the 2048 user data */

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
};

/* Fill an ATA IDENTIFY-style GD-ROM device-id block (see struct gdrom_id). */
static void gdrom_fill_id(DCGdromState *s)
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
static void gdrom_build_toc(DCGdromState *s)
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
static void gdrom_packet_dispatch(DCGdromState *s)
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
static void gdrom_do_dma(DCGdromState *s)
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
            int64_t off = s->data_off + (int64_t)rel * s->raw_size + s->sec_hdr;
            blk_pread(s->blk, off, GDROM_SECTOR, sec, 0);
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
    DCGdromState *s = opaque;

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
    DCGdromState *s = opaque;

    s->status &= ~GDS_BSY;
    s->status |= GDS_DRDY | GDS_DSC;
    s->error = 0x00;
    qemu_set_irq(s->irq_dma, 1);
}

static uint64_t gdrom_read(void *opaque, hwaddr addr, unsigned int size)
{
    DCGdromState *s = opaque;

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
    DCGdromState *s = opaque;

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

/* CDI (cdi4dc) data-track geometry: Mode2/Form1 stored as 2336-byte sectors,
 * with 8 subheader bytes before the 2048 user bytes. */
#define CDI_RAW_SIZE  2336
#define CDI_SEC_HDR   8
#define ISO_PVD_LBA   16            /* ISO9660 PVD at data-track sector 16 */
#define DISC_SCAN_WIN (8 << 20)     /* PVD lives ~1.4 MB in; 8 MB is safe */

/* Detect the image type and fill the data-track geometry. Defaults to a flat
 * raw ISO (off=0, 2048/sector). Detection is structural (size-independent, so
 * it survives the block layer padding the image up to a 512-byte boundary): we
 * scan for the ISO9660 primary volume descriptor ("\x01CD001") and read the
 * following volume-descriptor-set terminator ("\xffCD001"). The stride between
 * them is the stored sector size - 2048 for a flat ISO, 2336 for a cdi4dc CDI
 * (Mode2/Form1: 8-byte subheader + 2048 user + EDC/ECC). */
static void gdrom_probe_disc(DCGdromState *s)
{
    int64_t len;
    uint8_t *win;
    size_t winlen, i;

    /* flat-ISO defaults */
    s->data_off = 0;
    s->raw_size = GDROM_SECTOR;
    s->sec_hdr = 0;
    s->data_lba = GDROM_DATA_LBA;

    if (!s->blk) {
        return;
    }
    len = blk_getlength(s->blk);
    if (len <= 0) {
        return;
    }
    s->n_sectors = len / GDROM_SECTOR;

    winlen = MIN(len, DISC_SCAN_WIN);
    win = g_malloc(winlen);
    if (blk_pread(s->blk, 0, winlen, win, 0) < 0) {
        g_free(win);
        return;
    }
    for (i = 0; i + 6 <= winlen; i++) {
        size_t pvd = i;
        int64_t off;

        if (win[i] != 0x01 || memcmp(&win[i + 1], "CD001", 5) != 0) {
            continue;
        }
        if (lduw_le_p(&win[pvd + 128]) != GDROM_SECTOR) {
            break;                  /* PVD without 2048 block size - give up */
        }
        /* Only a CDI needs remapping; a flat ISO already matches the defaults.
         * Distinguish by the terminator stride. */
        if (pvd + CDI_RAW_SIZE + 6 <= winlen &&
            win[pvd + CDI_RAW_SIZE] == 0xff &&
            memcmp(&win[pvd + CDI_RAW_SIZE + 1], "CD001", 5) == 0) {
            off = (int64_t)pvd - CDI_SEC_HDR - (int64_t)ISO_PVD_LBA * CDI_RAW_SIZE;
            if (off >= 0) {
                s->data_off = off;
                s->raw_size = CDI_RAW_SIZE;
                s->sec_hdr = CDI_SEC_HDR;
                s->n_sectors = ldl_le_p(&win[pvd + 80]); /* PVD vol size */
            }
        }
        break;
    }
    g_free(win);
}

/*
 * Read `nsec` logical 2048-byte sectors from session LBA `lba` into buf,
 * honouring the probed data-track geometry.  Exposed to the board's
 * boot-from-disc firmware via dc_gdrom_read_logical().
 */
static bool gdrom_read_logical(DCGdromState *s, uint32_t lba, uint32_t nsec,
                               uint8_t *buf)
{
    uint32_t k;

    if (!s->blk) {
        return false;
    }
    for (k = 0; k < nsec; k++) {
        int64_t rel = (int64_t)lba + k - s->data_lba;
        int64_t off;

        if (rel < 0) {
            return false;
        }
        off = s->data_off + rel * s->raw_size + s->sec_hdr;
        if (blk_pread(s->blk, off, GDROM_SECTOR, buf + k * GDROM_SECTOR, 0) < 0) {
            return false;
        }
    }
    return true;
}

static void gdrom_realize(DeviceState *dev, Error **errp)
{
    DCGdromState *s = DC_GDROM(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->cmd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gdrom_cmd_complete, s);
    s->dma_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gdrom_dma_complete, s);

    gdrom_probe_disc(s);

    memory_region_init_io(&s->iomem, OBJECT(s), &gdrom_ops, s, "dc-gdrom",
                          GDROM_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_cmd);      /* 0: command-complete (event 32) */
    sysbus_init_irq(sbd, &s->irq_dma);      /* 1: DMA-complete    (event 14) */
}

static void gdrom_reset_hold(Object *obj, ResetType type)
{
    DCGdromState *s = DC_GDROM(obj);

    timer_del(s->cmd_timer);
    timer_del(s->dma_timer);

    /* Power-on ATA state: ready, device 0 present. Disc geometry (probed at
     * realize) is preserved. */
    s->status = GDS_DRDY | GDS_DSC;
    s->error = 0x01;
    s->feature = 0;
    s->intsec = 0;
    s->secnum = 0;
    s->bcl = s->bch = 0;
    s->dsel = 0;
    s->pending_cmd = 0;
    s->post_cmd_irq = 0;
    s->phase = PHASE_IDLE;
    s->packet_idx = 0;
    memset(s->packet, 0, sizeof(s->packet));
    s->pio_len = s->pio_idx = 0;
    s->read_fad = s->read_count = 0;
    s->dma_pending = 0;
    s->dma_addr = s->dma_len = s->dma_dir = s->dma_enable = 0;
}

static const VMStateDescription vmstate_dreamcast_gdrom = {
    .name = "dc-gdrom",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(n_sectors, DCGdromState),
        VMSTATE_UINT32(data_lba, DCGdromState),
        VMSTATE_UINT64(data_off, DCGdromState),
        VMSTATE_UINT32(raw_size, DCGdromState),
        VMSTATE_UINT32(sec_hdr, DCGdromState),
        VMSTATE_UINT8(status, DCGdromState),
        VMSTATE_UINT8(error, DCGdromState),
        VMSTATE_UINT8(feature, DCGdromState),
        VMSTATE_UINT8(intsec, DCGdromState),
        VMSTATE_UINT8(secnum, DCGdromState),
        VMSTATE_UINT8(bcl, DCGdromState),
        VMSTATE_UINT8(bch, DCGdromState),
        VMSTATE_UINT8(dsel, DCGdromState),
        VMSTATE_UINT8(pending_cmd, DCGdromState),
        VMSTATE_INT32(post_cmd_irq, DCGdromState),
        VMSTATE_INT32(phase, DCGdromState),
        VMSTATE_UINT8_ARRAY(packet, DCGdromState, 12),
        VMSTATE_INT32(packet_idx, DCGdromState),
        VMSTATE_UINT8_ARRAY(pio, DCGdromState, GDROM_SECTOR),
        VMSTATE_UINT32(pio_len, DCGdromState),
        VMSTATE_UINT32(pio_idx, DCGdromState),
        VMSTATE_UINT32(read_fad, DCGdromState),
        VMSTATE_UINT32(read_count, DCGdromState),
        VMSTATE_INT32(dma_pending, DCGdromState),
        VMSTATE_UINT32(dma_addr, DCGdromState),
        VMSTATE_UINT32(dma_len, DCGdromState),
        VMSTATE_UINT32(dma_dir, DCGdromState),
        VMSTATE_UINT32(dma_enable, DCGdromState),
        VMSTATE_TIMER_PTR(cmd_timer, DCGdromState),
        VMSTATE_TIMER_PTR(dma_timer, DCGdromState),
        VMSTATE_END_OF_LIST()
    }
};

static void gdrom_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = gdrom_realize;
    rc->phases.hold = gdrom_reset_hold;
    dc->vmsd = &vmstate_dreamcast_gdrom;
}

static const TypeInfo gdrom_info = {
    .name          = TYPE_DC_GDROM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DCGdromState),
    .class_init    = gdrom_class_init,
};

static void gdrom_register_types(void)
{
    type_register_static(&gdrom_info);
}

type_init(gdrom_register_types)

/*
 * Board helper: create the GD-ROM drive on the disc image `blk`, map its
 * registers, and wire the command/DMA completion IRQs to Holly.  Returns the
 * DeviceState so the board's boot-from-disc firmware can read sectors.
 */
DeviceState *dc_gdrom_init(hwaddr base, qemu_irq irq_cmd, qemu_irq irq_dma,
                           BlockBackend *blk)
{
    DeviceState *dev = qdev_new(TYPE_DC_GDROM);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    DC_GDROM(dev)->blk = blk;
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, base);
    sysbus_connect_irq(sbd, 0, irq_cmd);
    sysbus_connect_irq(sbd, 1, irq_dma);
    return dev;
}

/* Board boot-firmware access: read logical sectors from the mounted disc. */
bool dc_gdrom_read_logical(DeviceState *dev, uint32_t lba, uint32_t nsec,
                           uint8_t *buf)
{
    return gdrom_read_logical(DC_GDROM(dev), lba, nsec, buf);
}

/* Board boot-firmware access: the probed data-track start LBA. */
uint32_t dc_gdrom_data_lba(DeviceState *dev)
{
    return DC_GDROM(dev)->data_lba;
}
