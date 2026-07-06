/*
 * Sega Dreamcast Maple bus controller + keyboard
 *
 * The Maple bus is a DMA-driven serial bus.  The guest builds a list of
 * command frames in RAM, points MAPLE_DMAADDR at it and arms MAPLE_ENABLE;
 * the controller walks the list, writes a response frame back to each block's
 * receive buffer and raises the Maple-DMA interrupt (Holly event 12).
 *
 * This models the controller plus one keyboard on port 0.  The frame and
 * response layout follow Linux's drivers/sh/maple/maple.c and
 * drivers/input/keyboard/maple_keyb.c.  The keyboard reuses QEMU's HIDState,
 * whose 8-byte report is exactly the Maple keyboard "get condition" payload.
 *
 * Copyright (c) 2026 Florian Fuchs
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/sh4/sh.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/dma.h"
#include "hw/input/hid.h"

#define TYPE_DC_MAPLE "dc-maple"
OBJECT_DECLARE_SIMPLE_TYPE(DCMapleState, DC_MAPLE)

/* Register offsets from the 0x005f6c00 block. */
#define MAPLE_DMAADDR   0x04
#define MAPLE_TRIGTYPE  0x10
#define MAPLE_ENABLE    0x14
#define MAPLE_STATE     0x18
#define MAPLE_SPEED     0x80
#define MAPLE_RESET     0x8c
#define MAPLE_MMIO_SIZE 0x100

/* Maple frame command / response codes (see include/linux/maple.h). */
#define CMD_DEVINFO      1
#define CMD_GETCOND      9
#define RESP_DEVINFO     5
#define RESP_DATATRF     8
#define RESP_NONE        0xff    /* (int8_t)-1 */

#define FUNC_KEYBOARD    0x40

#define MAPLE_POLL_HZ    60      /* DMA is paced to VBLANK on real hardware */

struct DCMapleState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;               /* Holly MAPLE_DMA event */
    QEMUTimer *timer;

    uint32_t dmaaddr;
    uint32_t enable;

    HIDState hid;               /* keyboard on port 0 */
};

/* Big-endian store of a 32-bit maple payload word (function codes etc). */
static void st_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

/* Build the device-information response for the keyboard on port 0. */
static int maple_build_devinfo(DCMapleState *s, uint8_t *buf)
{
    memset(buf, 0, 116);
    buf[0] = RESP_DEVINFO;
    buf[1] = 0x20;              /* sender: port 0, base unit  */
    buf[2] = 0x00;              /* recipient: host            */
    buf[3] = 28;               /* payload length in words    */
    st_be32(&buf[4], FUNC_KEYBOARD);
    buf[20] = 0xff;            /* area code                  */
    buf[21] = 0x00;            /* connector direction        */
    memcpy(&buf[22], "Keyboard                      ", 30);
    memcpy(&buf[52],
           "Produced By or Under License From SEGA ENTERPRISES,LTD.     ", 60);
    return 116;
}

/* Build the keyboard "get condition" response (8-byte HID report). */
static int maple_build_getcond(DCMapleState *s, uint8_t *buf)
{
    memset(buf, 0, 16);
    buf[0] = RESP_DATATRF;
    buf[1] = 0x20;
    buf[2] = 0x00;
    buf[3] = 3;                /* function word + two data words */
    st_be32(&buf[4], FUNC_KEYBOARD);
    hid_keyboard_poll(&s->hid, &buf[8], 8);
    return 16;
}

/* Process one queued transfer list: walk blocks, write responses. */
static void maple_process_dma(DCMapleState *s)
{
    AddressSpace *as = &address_space_memory;
    hwaddr ptr = s->dmaaddr;
    uint8_t resp[128];
    int guard = 0;

    while (guard++ < 64) {
        uint32_t w0 = 0, recv = 0, w2 = 0;
        uint32_t port, len, cmd, to;
        int n = 0;

        ldl_le_dma(as, ptr, &w0, MEMTXATTRS_UNSPECIFIED);
        ldl_le_dma(as, ptr + 4, &recv, MEMTXATTRS_UNSPECIFIED);
        ldl_le_dma(as, ptr + 8, &w2, MEMTXATTRS_UNSPECIFIED);

        port = (w0 >> 16) & 3;
        len = w0 & 0xff;            /* command data words */
        cmd = w2 & 0xff;
        to = (w2 >> 8) & 0xff;      /* recipient device address */

        /* Only a keyboard on port 0, base unit (address bit 0x20). */
        if (port == 0 && (to & 0x20)) {
            switch (cmd) {
            case CMD_DEVINFO:
                n = maple_build_devinfo(s, resp);
                break;
            case CMD_GETCOND:
                n = maple_build_getcond(s, resp);
                break;
            default:
                resp[0] = RESP_NONE;
                n = 4;
                break;
            }
        } else {
            resp[0] = RESP_NONE;    /* nothing on this port/unit */
            n = 4;
        }

        if (n < 4) {
            memset(resp, 0, 4);
            n = 4;
        }
        dma_memory_write(as, recv, resp, n, MEMTXATTRS_UNSPECIFIED);

        if (w0 & 0x80000000) {
            break;                  /* end-of-list bit */
        }
        ptr += (3 + len) * 4;
    }
}

static void maple_timer(void *opaque)
{
    DCMapleState *s = opaque;

    if (s->enable & 1) {
        maple_process_dma(s);
        s->enable = 0;              /* one-shot per trigger */
        qemu_set_irq(s->irq, 1);    /* Holly latches MAPLE_DMA (edge) */
    }
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / MAPLE_POLL_HZ);
}

static uint64_t maple_read(void *opaque, hwaddr addr, unsigned int size)
{
    DCMapleState *s = opaque;

    switch (addr) {
    case MAPLE_DMAADDR:
        return s->dmaaddr;
    case MAPLE_ENABLE:
        return s->enable;
    case MAPLE_STATE:
        return 0;                   /* DMA is processed synchronously: idle */
    default:
        return 0;
    }
}

static void maple_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned int size)
{
    DCMapleState *s = opaque;

    switch (addr) {
    case MAPLE_DMAADDR:
        s->dmaaddr = val;
        break;
    case MAPLE_ENABLE:
        s->enable = val;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps maple_ops = {
    .read = maple_read,
    .write = maple_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void maple_keyboard_event(HIDState *hid)
{
    /* State is pulled on the next GETCOND poll; nothing to do here. */
}

static void maple_reset(DeviceState *dev)
{
    DCMapleState *s = DC_MAPLE(dev);

    s->dmaaddr = 0;
    s->enable = 0;
    hid_reset(&s->hid);
}

static void maple_realize(DeviceState *dev, Error **errp)
{
    DCMapleState *s = DC_MAPLE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &maple_ops, s, "dc-maple",
                          MAPLE_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    hid_init(&s->hid, HID_KEYBOARD, maple_keyboard_event);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maple_timer, s);
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / MAPLE_POLL_HZ);
}

static const VMStateDescription vmstate_maple = {
    .name = "dc-maple",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dmaaddr, DCMapleState),
        VMSTATE_UINT32(enable, DCMapleState),
        VMSTATE_HID_KEYBOARD_DEVICE(hid, DCMapleState),
        VMSTATE_END_OF_LIST()
    }
};

static void maple_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = maple_realize;
    device_class_set_legacy_reset(dc, maple_reset);
    dc->vmsd = &vmstate_maple;
}

static const TypeInfo maple_info = {
    .name          = TYPE_DC_MAPLE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DCMapleState),
    .class_init    = maple_class_init,
};

static void maple_register_types(void)
{
    type_register_static(&maple_info);
}

type_init(maple_register_types)

/* Board helper: create the Maple controller, map it, wire its interrupt. */
void dc_maple_init(hwaddr base, qemu_irq irq)
{
    DeviceState *dev;
    SysBusDevice *sbd;

    dev = qdev_new(TYPE_DC_MAPLE);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, base);
    sysbus_connect_irq(sbd, 0, irq);
}
