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

#define RESP_OK          7

#define FUNC_KEYBOARD    0x40
#define FUNC_MOUSE       0x200
#define FUNC_CONTROLLER  0x01

/*
 * Maple device address byte: bits 6-7 = port, bit 5 = base unit, bits 0-4 =
 * expansion sub-unit (bit 0 = slot 1).  A VMU always lives in a controller's
 * slot, so it appears as sub-unit 1 of the controller on CONTROLLER_PORT.
 */
#define MAPLE_ADDR(port)     (0x20 | ((port) << 6))
#define MAPLE_SUBADDR(port)  (((port) << 6) | 0x01)   /* sub-unit 1 */
#define MAPLE_HOSTADDR(port) ((port) << 6)
#define KEYBOARD_PORT    0
#define MOUSE_PORT       1
#define CONTROLLER_PORT  2
#define VMU_SUBMASK      0x01       /* controller reports a device in slot 1 */

#define MAPLE_POLL_HZ    60      /* DMA is paced to VBLANK on real hardware */

struct DCMapleState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;               /* Holly MAPLE_DMA event */
    QEMUTimer *timer;

    uint32_t dmaaddr;
    uint32_t enable;

    HIDState kbd;               /* keyboard on port 0 */
    HIDState mouse;             /* mouse on port 1    */
    DCVmu *vmu;                 /* VMU in the controller slot on port 2 */
};

/* Big-endian store of a 32-bit maple payload word (function codes etc). */
static void st_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

/* Build a device-information response for the device on the given port. */
static int maple_build_devinfo(int port, uint32_t func, const char *name,
                               uint8_t *buf)
{
    memset(buf, 0, 116);
    buf[0] = RESP_DEVINFO;
    buf[1] = MAPLE_ADDR(port);  /* sender: port, base unit    */
    buf[2] = 0x00;              /* recipient: host            */
    buf[3] = 28;               /* payload length in words    */
    st_be32(&buf[4], func);
    buf[20] = 0xff;            /* area code                  */
    buf[21] = 0x00;            /* connector direction        */
    memset(&buf[22], ' ', 30);
    memcpy(&buf[22], name, strlen(name));
    memcpy(&buf[52],
           "Produced By or Under License From SEGA ENTERPRISES,LTD.     ", 60);
    return 116;
}

/* Build the keyboard "get condition" response (8-byte HID report). */
static int maple_build_kbd_getcond(DCMapleState *s, uint8_t *buf)
{
    memset(buf, 0, 16);
    buf[0] = RESP_DATATRF;
    buf[1] = MAPLE_ADDR(KEYBOARD_PORT);
    buf[2] = 0x00;
    buf[3] = 3;                /* function word + two data words */
    st_be32(&buf[4], FUNC_KEYBOARD);
    hid_keyboard_poll(&s->kbd, &buf[8], 8);
    return 16;
}

/*
 * Build the mouse "get condition" response.  Linux drivers/input/mouse/
 * maplemouse.c reads: buttons = ~res[8] (active-low; bit2 left, bit1 right,
 * bit3 middle) and three little-endian 16-bit axes at res+12/14/16, each
 * biased by 512 (512 == no movement).  QEMU's HID mouse report gives us
 * relative dx/dy/dz in [-127,127], which fits directly into that bias.
 */
static int maple_build_mouse_getcond(DCMapleState *s, uint8_t *buf)
{
    uint8_t rep[4];
    int dx, dy, dz;
    uint8_t dcbtn = 0;

    hid_pointer_poll(&s->mouse, rep, sizeof(rep));
    /* rep[0] = buttons (bit0 left, bit1 right, bit2 middle) */
    if (rep[0] & 0x01) {
        dcbtn |= 0x04;         /* left  */
    }
    if (rep[0] & 0x02) {
        dcbtn |= 0x02;         /* right */
    }
    if (rep[0] & 0x04) {
        dcbtn |= 0x08;         /* middle */
    }
    dx = 512 + (int8_t)rep[1];
    dy = 512 + (int8_t)rep[2];
    dz = 512 + (int8_t)rep[3];

    memset(buf, 0, 20);
    buf[0] = RESP_DATATRF;
    buf[1] = MAPLE_ADDR(MOUSE_PORT);
    buf[2] = 0x00;
    buf[3] = 4;                /* function word + three data words */
    st_be32(&buf[4], FUNC_MOUSE);
    buf[8] = ~dcbtn;           /* buttons, active-low        */
    buf[9] = 0xff;             /* button high byte (released) */
    buf[12] = dx & 0xff;  buf[13] = (dx >> 8) & 0xff;
    buf[14] = dy & 0xff;  buf[15] = (dy >> 8) & 0xff;
    buf[16] = dz & 0xff;  buf[17] = (dz >> 8) & 0xff;
    return 20;
}

/*
 * Controller base unit on CONTROLLER_PORT.  It exists to host the VMU: the
 * sub-device mask in response byte 2 tells Linux a device sits in slot 1, so
 * the bus then scans sub-unit 1 (the VMU).
 */
static int maple_build_controller_devinfo(uint8_t *buf)
{
    memset(buf, 0, 116);
    buf[0] = RESP_DEVINFO;
    buf[1] = MAPLE_HOSTADDR(CONTROLLER_PORT);
    buf[2] = 0x20 | VMU_SUBMASK;   /* base unit + slot 1 occupied */
    buf[3] = 28;
    st_be32(&buf[4], FUNC_CONTROLLER);
    buf[20] = 0xff;
    buf[21] = 0x00;
    memset(&buf[22], ' ', 30);
    memcpy(&buf[22], "Dreamcast Controller", 20);
    memcpy(&buf[52],
           "Produced By or Under License From SEGA ENTERPRISES,LTD.     ", 60);
    return 116;
}

/* Controller "get condition": neutral (nothing pressed, sticks centered). */
static int maple_build_controller_getcond(uint8_t *buf)
{
    memset(buf, 0, 16);
    buf[0] = RESP_DATATRF;
    buf[1] = MAPLE_HOSTADDR(CONTROLLER_PORT);
    buf[2] = MAPLE_ADDR(CONTROLLER_PORT);
    buf[3] = 3;
    st_be32(&buf[4], FUNC_CONTROLLER);
    buf[8] = 0xff;                 /* buttons are active-low: none pressed */
    buf[9] = 0xff;
    buf[12] = 0x80;               /* analog sticks centered */
    buf[13] = 0x80;
    buf[14] = 0x80;
    buf[15] = 0x80;
    return 16;
}

/* Process one queued transfer list: walk blocks, write responses. */
static void maple_process_dma(DCMapleState *s)
{
    AddressSpace *as = &address_space_memory;
    hwaddr ptr = s->dmaaddr;
    uint8_t resp[1032];            /* a VMU block-read response is ~524 bytes */
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

        if (to & 0x20) {
            /* Base unit: keyboard (0), mouse (1), controller (2). */
            if (port == KEYBOARD_PORT) {
                switch (cmd) {
                case CMD_DEVINFO:
                    n = maple_build_devinfo(KEYBOARD_PORT, FUNC_KEYBOARD,
                                            "Keyboard", resp);
                    break;
                case CMD_GETCOND:
                    n = maple_build_kbd_getcond(s, resp);
                    break;
                default:
                    resp[0] = RESP_NONE;
                    n = 4;
                    break;
                }
            } else if (port == MOUSE_PORT) {
                switch (cmd) {
                case CMD_DEVINFO:
                    n = maple_build_devinfo(MOUSE_PORT, FUNC_MOUSE,
                                            "Mouse", resp);
                    break;
                case CMD_GETCOND:
                    n = maple_build_mouse_getcond(s, resp);
                    break;
                default:
                    resp[0] = RESP_NONE;
                    n = 4;
                    break;
                }
            } else if (port == CONTROLLER_PORT && s->vmu) {
                switch (cmd) {
                case CMD_DEVINFO:
                    n = maple_build_controller_devinfo(resp);
                    break;
                case CMD_GETCOND:
                    n = maple_build_controller_getcond(resp);
                    break;
                default:
                    resp[0] = RESP_NONE;
                    n = 4;
                    break;
                }
            } else {
                resp[0] = RESP_NONE;
                n = 4;
            }
        } else if (port == CONTROLLER_PORT && (to & VMU_SUBMASK) && s->vmu) {
            /* VMU in the controller's expansion slot (sub-unit 1). */
            uint8_t cmddata[256];
            int datalen = len * 4;

            if (datalen > (int)sizeof(cmddata)) {
                datalen = sizeof(cmddata);
            }
            if (datalen > 0) {
                dma_memory_read(as, ptr + 12, cmddata, datalen,
                                MEMTXATTRS_UNSPECIFIED);
            }
            n = dc_vmu_maple(s->vmu, cmd, MAPLE_HOSTADDR(CONTROLLER_PORT),
                             MAPLE_SUBADDR(CONTROLLER_PORT),
                             cmddata, datalen, resp);
            if (n <= 0) {
                resp[0] = RESP_NONE;
                n = 4;
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

static void maple_input_event(HIDState *hid)
{
    /* State is pulled on the next GETCOND poll; nothing to do here. */
}

static void maple_reset_hold(Object *obj, ResetType type)
{
    DCMapleState *s = DC_MAPLE(obj);

    s->dmaaddr = 0;
    s->enable = 0;
    hid_reset(&s->kbd);
    hid_reset(&s->mouse);
}

static void maple_realize(DeviceState *dev, Error **errp)
{
    DCMapleState *s = DC_MAPLE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &maple_ops, s, "dc-maple",
                          MAPLE_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    hid_init(&s->kbd, HID_KEYBOARD, maple_input_event);
    hid_init(&s->mouse, HID_MOUSE, maple_input_event);

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
        VMSTATE_HID_KEYBOARD_DEVICE(kbd, DCMapleState),
        VMSTATE_HID_POINTER_DEVICE(mouse, DCMapleState),
        VMSTATE_END_OF_LIST()
    }
};

static void maple_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = maple_realize;
    rc->phases.hold = maple_reset_hold;
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

/* Board helper: create the Maple controller, map it, wire its interrupt.
 * If vmu is non-NULL, a controller with that VMU in slot 1 appears on port 2. */
void dc_maple_init(hwaddr base, qemu_irq irq, DCVmu *vmu)
{
    DeviceState *dev;
    SysBusDevice *sbd;

    dev = qdev_new(TYPE_DC_MAPLE);
    DC_MAPLE(dev)->vmu = vmu;
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, base);
    sysbus_connect_irq(sbd, 0, irq);
}
