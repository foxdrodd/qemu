/*
 * Sega Dreamcast Holly CH2 ("PVR") DMA
 *
 * The SH4 on-chip DMAC channel 2 is cascaded in DDT mode: the kernel latches
 * the transfer source in SAR2 (arch/sh/drivers/dma/dma-sh.c) and programs this
 * block (dma-pvr2.c, used by pvr2fb's fb_write) with the destination and byte
 * count; writing 1 to SB_C2DST starts the transfer.  Completion raises Holly
 * event 19 ("end of DMA: CH2") and reports back into the DMAC latch (TCR2 = 0,
 * CHCR2.TE) so the kernel's residue check reads zero.
 *
 * Destination decoding: the texture-path windows 0x10000000-0x13ffffff map to
 * VRAM (the 64/32-bit bus distinction and LMMODE interleave are not modelled);
 * any other value is treated as a plain 29-bit bus address.  The latter makes
 * the P2 framebuffer pointers the (unfixed) pvr2fb driver programs (0xa5xxxxxx)
 * land in VRAM at 0x05xxxxxx, matching the "data arrives at the start of the
 * visible framebuffer" behaviour seen on hardware.
 *
 * Copyright (c) 2026 Florian Fuchs
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/sh4/sh.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "qom/object.h"
#include "system/dma.h"

#define TYPE_DC_CH2DMA "dc-ch2dma"
OBJECT_DECLARE_SIMPLE_TYPE(DCCh2DmaState, DC_CH2DMA)

#define CH2DMA_SIZE  0x100

#define SB_C2DSTAT   0x00       /* destination address */
#define SB_C2DLEN    0x04       /* byte count (32-byte units) */
#define SB_C2DST     0x08       /* write 1: start; reads busy */
#define SB_LMMODE0   0x84       /* 0x11000000 window bus width */
#define SB_LMMODE1   0x88       /* 0x13000000 window bus width */

#define CH2DMA_DELAY_NS 100000  /* ~0.1 ms transfer latency */

/* PowerVR2 video RAM window (Dreamcast memory map). */
#define VRAM_BASE  0x05000000
#define VRAM_SIZE  (8 * MiB)

struct DCCh2DmaState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;
    struct SH7750State *sh;     /* DMAC channel 2 cascade source */
    uint32_t dstat;
    uint32_t dlen;
    uint32_t st;
    uint32_t lmmode0, lmmode1;
};

static void dc_ch2dma_complete(void *opaque)
{
    DCCh2DmaState *s = opaque;

    sh7750_dmac_transfer_done(s->sh, 2, s->dlen);
    s->dstat += s->dlen;        /* hardware leaves the end address here */
    s->dlen = 0;
    s->st = 0;
    qemu_set_irq(s->irq, 1);    /* Holly latches the ESR bit (edge) */
}

static void dc_ch2dma_kick(DCCh2DmaState *s)
{
    uint32_t src = sh7750_dmac_sar(s->sh, 2) & 0x1fffffff;
    uint32_t len = s->dlen & 0x00ffffe0;
    uint32_t dst = s->dstat;
    uint32_t off = 0;
    int lm = -1;                /* -1: plain bus copy, else LMMODE for VRAM */
    g_autofree uint8_t *buf = NULL;

    /*
     * Destination decode, verified against hardware with marker scans:
     * the texture windows 0x11/0x13xxxxxx go to VRAM with the bus width
     * chosen by SB_LMMODE0/1; a plain (masked) bus address in the VRAM
     * areas -- which is what pvr2fb's P2 pointers become -- also goes
     * through the LMMODE0 path.  LMMODE = 0 selects 64-bit access, which
     * interleaves the two 4 MB banks per 32-bit word: stream offset A
     * lands at 32-bit-area offset (A/8)*4, odd words in the second bank
     * (+0x400000).  LMMODE = 1 is a linear 1:1 mapping.
     */
    if ((dst & 0x1c000000) == 0x10000000) {
        off = dst & 0x00ffffff;
        lm  = (dst & 0x02000000) ? s->lmmode1 : s->lmmode0;
    } else {
        uint32_t bus = dst & 0x1fffffe0;

        if (bus >= 0x04000000 && bus < 0x06000000) {
            off = bus & 0x00ffffff;
            lm  = s->lmmode0;
        } else {
            dst = bus;
        }
    }
    if (len) {
        buf = g_malloc(len);
        dma_memory_read(&address_space_memory, src, buf, len,
                        MEMTXATTRS_UNSPECIFIED);
        if (lm < 0) {
            dma_memory_write(&address_space_memory, dst, buf, len,
                             MEMTXATTRS_UNSPECIFIED);
        } else if (lm) {
            dma_memory_write(&address_space_memory,
                             VRAM_BASE + (off & (VRAM_SIZE - 1)), buf, len,
                             MEMTXATTRS_UNSPECIFIED);
        } else {
            uint32_t i;

            for (i = 0; i < len; i += 4) {
                uint32_t a = off + i;
                uint32_t vis = (((a >> 3) << 2) | (a & 3))
                             + (((a >> 2) & 1) << 22);

                dma_memory_write(&address_space_memory,
                                 VRAM_BASE + (vis & (VRAM_SIZE - 1)),
                                 buf + i, 4, MEMTXATTRS_UNSPECIFIED);
            }
        }
    }
    s->st = 1;
    timer_mod(s->timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CH2DMA_DELAY_NS);
}

static uint64_t dc_ch2dma_read(void *opaque, hwaddr addr, unsigned size)
{
    DCCh2DmaState *s = opaque;

    switch (addr) {
    case SB_C2DSTAT:
        return s->dstat;
    case SB_C2DLEN:
        return s->dlen;
    case SB_C2DST:
        return s->st;
    case SB_LMMODE0:
        return s->lmmode0;
    case SB_LMMODE1:
        return s->lmmode1;
    default:
        return 0;
    }
}

static void dc_ch2dma_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    DCCh2DmaState *s = opaque;

    switch (addr) {
    case SB_C2DSTAT:
        s->dstat = val;
        break;
    case SB_C2DLEN:
        s->dlen = val;
        break;
    case SB_C2DST:
        if ((val & 1) && !s->st) {
            dc_ch2dma_kick(s);
        }
        break;
    case SB_LMMODE0:
        s->lmmode0 = val & 1;
        break;
    case SB_LMMODE1:
        s->lmmode1 = val & 1;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps dc_ch2dma_ops = {
    .read = dc_ch2dma_read,
    .write = dc_ch2dma_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dc_ch2dma_realize(DeviceState *dev, Error **errp)
{
    DCCh2DmaState *s = DC_CH2DMA(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dc_ch2dma_complete, s);
    memory_region_init_io(&s->iomem, OBJECT(s), &dc_ch2dma_ops, s, "dc-ch2dma",
                          CH2DMA_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void dc_ch2dma_reset_hold(Object *obj, ResetType type)
{
    DCCh2DmaState *s = DC_CH2DMA(obj);

    timer_del(s->timer);
    s->dstat = 0;
    s->dlen = 0;
    s->st = 0;
    s->lmmode0 = 0;
    s->lmmode1 = 0;
}

static const VMStateDescription vmstate_dreamcast_ch2dma = {
    .name = "dc-ch2dma",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dstat, DCCh2DmaState),
        VMSTATE_UINT32(dlen, DCCh2DmaState),
        VMSTATE_UINT32(st, DCCh2DmaState),
        VMSTATE_UINT32(lmmode0, DCCh2DmaState),
        VMSTATE_UINT32(lmmode1, DCCh2DmaState),
        VMSTATE_END_OF_LIST()
    }
};

static void dc_ch2dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = dc_ch2dma_realize;
    rc->phases.hold = dc_ch2dma_reset_hold;
    dc->vmsd = &vmstate_dreamcast_ch2dma;
}

static const TypeInfo dc_ch2dma_info = {
    .name          = TYPE_DC_CH2DMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DCCh2DmaState),
    .class_init    = dc_ch2dma_class_init,
};

static void dc_ch2dma_register_types(void)
{
    type_register_static(&dc_ch2dma_info);
}

type_init(dc_ch2dma_register_types)

/*
 * Board helper: create the CH2 DMA engine, connect it to the SH7750 DMAC
 * channel-2 latch, map its registers, and wire its completion IRQ to Holly.
 */
void dc_ch2dma_init(hwaddr base, struct SH7750State *sh, qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_DC_CH2DMA);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    DC_CH2DMA(dev)->sh = sh;
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, base);
    sysbus_connect_irq(sbd, 0, irq);
}
