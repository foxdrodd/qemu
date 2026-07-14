/*
 * Sega Dreamcast PowerVR2 (CLX2) - video display / scanout only
 *
 * This models just the display list of the PowerVR2: the registers the Linux
 * pvr2fb driver programs to describe a linear framebuffer, plus scanout of
 * that framebuffer from VRAM to a QEMU display surface.  The tile accelerator
 * (3D core) is NOT emulated - Linux does not use it.
 *
 * Register semantics follow drivers/video/fbdev/pvr2fb.c.
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
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "framebuffer.h"

#define TYPE_DC_PVR "dc-pvr"
OBJECT_DECLARE_SIMPLE_TYPE(DCPvrState, DC_PVR)

#define PVR_REGS_SIZE   0x2000
#define PVR_NR_REGS     (PVR_REGS_SIZE / 4)

/* Display-list registers, byte offsets from the 0x005f8000 block. */
#define DISP_DIWMODE    0x44    /* bit0 = display enable, bits2-3 = bpp-1  */
#define DISP_DIWADDRL   0x50    /* framebuffer start (long field / even)   */
#define DISP_DIWADDRS   0x54    /* framebuffer start (short field / odd)   */
#define DISP_DIWSIZE    0x5c    /* modulo<<20 | (rows-1)<<10 | (words-1)   */

/* Tile-accelerator / render-path registers (same 0x005f8000 block). */
#define PVR_STARTRENDER   0x014 /* write: kick ISP/TSP over the binned lists */
#define PVR_FB_W_CTRL     0x048 /* render-target pixel format (bits 2-0)     */
#define PVR_FB_W_LINESTR  0x04c /* render-target line stride (64-bit units)  */
#define PVR_FB_W_SOF1     0x060 /* render-target base address in VRAM        */
#define PVR_FB_X_CLIP     0x068 /* render width:  max<<16 | min              */
#define PVR_FB_Y_CLIP     0x06c /* render height: max<<16 | min              */
#define PVR_TA_LIST_INIT  0x144 /* write bit31: reset the TA parameter parser */

/*
 * The bytes-per-pixel lives in DIWMODE bits 2-3 (value = bpp-1), written by
 * pvr2_init_display().  Register 0x108 is NOT a reliable pixel-depth source:
 * pvr2fb overloads it - pvr2fb_set_pal_type() writes the palette type there,
 * clobbering the depth value the moment X (or fbcon in palette mode) sets up
 * its colour map.  Always take the depth from DIWMODE.
 */
static inline uint32_t pvr_bytespp(uint32_t diwmode)
{
    return ((diwmode >> 2) & 3) + 1;
}

/* VRAM physical base; pvr2fb programs addresses in the 0xa5000000 view. */
#define VRAM_PHYS_BASE  0x05000000

#define PVR_VBLANK_HZ   60      /* vertical refresh; paces Maple polling */

struct DCPvrState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion *vram;         /* the machine's VRAM region      */
    MemoryRegionSection fbsection;
    QemuConsole *con;
    qemu_irq vblank_irq;        /* Holly VSYNC event (drives Maple polling) */
    QEMUTimer *vblank_timer;
    DeviceState *ta;            /* Tile Accelerator (dreamcast_ta.c)        */

    uint32_t regs[PVR_NR_REGS];

    /* Cached geometry from the last update, to detect mode changes. */
    uint32_t last_width;
    uint32_t last_height;
    bool invalidate;
};

/* Convert one framebuffer row to the display surface (RGB565 or xRGB8888). */
static void pvr_draw_line(void *opaque, uint8_t *dst, const uint8_t *src,
                          int width, int deststep)
{
    DCPvrState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    int bpp = surface_bits_per_pixel(surface);
    uint32_t bytespp = pvr_bytespp(s->regs[DISP_DIWMODE / 4]);
    uint8_t r, g, b;

    while (width--) {
        if (bytespp >= 3) {                 /* RGB888 / ARGB8888: 0xxxRRGGBB LE */
            uint32_t v = ldl_le_p(src);
            r = (v >> 16) & 0xff;
            g = (v >> 8) & 0xff;
            b = v & 0xff;
            src += bytespp;
        } else {                            /* RGB565 */
            uint16_t v = lduw_le_p(src);
            r = ((v >> 11) & 0x1f) << 3;
            g = ((v >> 5) & 0x3f) << 2;
            b = (v & 0x1f) << 3;
            src += 2;
        }

        switch (bpp) {
        case 8:
            *dst++ = rgb_to_pixel8(r, g, b);
            break;
        case 15:
            *(uint16_t *)dst = rgb_to_pixel15(r, g, b);
            dst += 2;
            break;
        case 16:
            *(uint16_t *)dst = rgb_to_pixel16(r, g, b);
            dst += 2;
            break;
        case 24: {
            uint32_t v = rgb_to_pixel24(r, g, b);
            *dst++ = v & 0xff;
            *dst++ = (v >> 8) & 0xff;
            *dst++ = (v >> 16) & 0xff;
            break;
        }
        case 32:
            *(uint32_t *)dst = rgb_to_pixel32(r, g, b);
            dst += 4;
            break;
        default:
            return;
        }
    }
}

/* Decode the current mode from the display-list registers. */
static bool pvr_get_mode(DCPvrState *s, uint32_t *width, uint32_t *height,
                         uint32_t *src_width, hwaddr *fb_offset)
{
    uint32_t diwmode = s->regs[DISP_DIWMODE / 4];
    uint32_t diwsize = s->regs[DISP_DIWSIZE / 4];
    uint32_t bytespp = pvr_bytespp(diwmode);
    uint32_t words, rows, modulo, line_bytes;
    uint32_t addr;

    if (!(diwmode & 1)) {
        return false;               /* display disabled */
    }

    words = (diwsize & 0x3ff) + 1;          /* 32-bit words per line */
    rows = ((diwsize >> 10) & 0x3ff) + 1;
    modulo = (diwsize >> 20) & 0x3ff;
    line_bytes = words * 4;

    /*
     * modulo > 1 means an interlaced field layout: the driver stores yres/2
     * per field but the framebuffer in VRAM is a progressive full-height image
     * (each field line skips one line via the modulo).  Show the full frame.
     */
    if (modulo > 1) {
        rows *= 2;
    }

    addr = s->regs[DISP_DIWADDRL / 4] & 0x1fffffff;
    if (addr < VRAM_PHYS_BASE) {
        return false;
    }

    *src_width = line_bytes;
    *width = line_bytes / bytespp;
    *height = rows;
    *fb_offset = addr - VRAM_PHYS_BASE;

    return *width != 0 && *height != 0;
}

static void pvr_invalidate(void *opaque)
{
    DCPvrState *s = opaque;
    s->invalidate = true;
}

static bool pvr_gfx_update(void *opaque)
{
    DCPvrState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t width, height, src_width;
    hwaddr fb_offset;
    int first = 0, last = 0;

    if (!pvr_get_mode(s, &width, &height, &src_width, &fb_offset)) {
        return true;
    }

    if (width != s->last_width || height != s->last_height) {
        qemu_console_resize(s->con, width, height);
        surface = qemu_console_surface(s->con);
        s->last_width = width;
        s->last_height = height;
        s->invalidate = true;
    }

    if (s->invalidate) {
        framebuffer_update_memory_section(&s->fbsection, s->vram, fb_offset,
                                          height, src_width);
    }

    framebuffer_update_display(surface, &s->fbsection, width, height,
                               src_width, surface_stride(surface), 0,
                               s->invalidate, pvr_draw_line, s, &first, &last);

    if (first >= 0) {
        qemu_console_update(s->con, 0, first, width, last - first + 1);
    }
    s->invalidate = false;
    return true;
}

static const GraphicHwOps pvr_gfx_ops = {
    .invalidate = pvr_invalidate,
    .gfx_update = pvr_gfx_update,
};

static uint64_t pvr_read(void *opaque, hwaddr addr, unsigned int size)
{
    DCPvrState *s = opaque;

    if (addr / 4 < PVR_NR_REGS) {
        return s->regs[addr / 4];
    }
    return 0;
}

static void pvr_write(void *opaque, hwaddr addr, uint64_t val,
                      unsigned int size)
{
    DCPvrState *s = opaque;

    if (addr / 4 < PVR_NR_REGS) {
        s->regs[addr / 4] = val;
        /* A mode-affecting write forces a full redraw next frame. */
        switch (addr) {
        case DISP_DIWMODE:
        case DISP_DIWADDRL:
        case DISP_DIWSIZE:
            s->invalidate = true;
            break;

        /* Tile-accelerator triggers, forwarded to dreamcast_ta.c. */
        case PVR_TA_LIST_INIT:
            if (s->ta && (val & 0x80000000)) {
                dc_ta_list_init(s->ta);
            }
            break;
        case PVR_STARTRENDER:
            if (s->ta) {
                dc_ta_start_render(s->ta,
                                   s->regs[PVR_FB_W_SOF1 / 4],
                                   s->regs[PVR_FB_W_CTRL / 4],
                                   s->regs[PVR_FB_W_LINESTR / 4],
                                   s->regs[PVR_FB_X_CLIP / 4],
                                   s->regs[PVR_FB_Y_CLIP / 4]);
            }
            break;
        }
    }
}

static const MemoryRegionOps pvr_ops = {
    .read = pvr_read,
    .write = pvr_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void pvr_reset_hold(Object *obj, ResetType type)
{
    DCPvrState *s = DC_PVR(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->last_width = 0;
    s->last_height = 0;
    s->invalidate = true;
}

/* Periodic vertical-blank: raises the Holly VSYNC event (Maple polls on it). */
static void pvr_vblank(void *opaque)
{
    DCPvrState *s = opaque;

    qemu_set_irq(s->vblank_irq, 1);     /* Holly latches the event (edge) */
    timer_mod(s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / PVR_VBLANK_HZ);
}

static void pvr_realize(DeviceState *dev, Error **errp)
{
    DCPvrState *s = DC_PVR(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!s->vram) {
        error_setg(errp, "dc-pvr: vram region not connected");
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &pvr_ops, s, "dc-pvr",
                          PVR_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->vblank_irq);

    s->con = qemu_graphic_console_create(dev, 0, &pvr_gfx_ops, s);

    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pvr_vblank, s);
    timer_mod(s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / PVR_VBLANK_HZ);
}

static const VMStateDescription vmstate_pvr = {
    .name = "dc-pvr",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, DCPvrState, PVR_NR_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void pvr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = pvr_realize;
    rc->phases.hold = pvr_reset_hold;
    dc->vmsd = &vmstate_pvr;
}

static const TypeInfo pvr_info = {
    .name          = TYPE_DC_PVR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DCPvrState),
    .class_init    = pvr_class_init,
};

static void pvr_register_types(void)
{
    type_register_static(&pvr_info);
}

type_init(pvr_register_types)

/* Board helper: create the PVR2 display, connect VRAM, map regs, wire VSYNC.
 * The Tile Accelerator (ta) receives forwarded TA_LIST_INIT / STARTRENDER. */
void dc_pvr_init(hwaddr base, MemoryRegion *vram, qemu_irq vblank_irq,
                 DeviceState *ta)
{
    DeviceState *dev;
    SysBusDevice *sbd;

    dev = qdev_new(TYPE_DC_PVR);
    DC_PVR(dev)->vram = vram;
    DC_PVR(dev)->ta = ta;
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, base);
    sysbus_connect_irq(sbd, 0, vblank_irq);
}
