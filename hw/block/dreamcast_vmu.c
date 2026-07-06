/*
 * Sega Dreamcast Visual Memory Unit (VMU) - storage + LCD
 *
 * A VMU is a Maple bus peripheral that plugs into a controller's expansion
 * slot.  It is a multi-function device: 128 KB flash (MAPLE_FUNC_MEMCARD) and,
 * optionally, a 48x32 monochrome LCD (MAPLE_FUNC_LCD).  Linux drives the flash
 * with drivers/mtd/maps/vmu-flash.c (a 256-block x 512-byte MTD) and the LCD
 * with drivers/auxdisplay/vmu-lcd.c (a /dev/vmu_lcd char device).
 *
 * The flash is backed by a QEMU BlockBackend (128 KB host image).  When the
 * LCD is enabled it is presented as a second QEMU graphic console (a separate
 * window / display head); the guest pushes 192-byte framebuffers to it via a
 * Maple BWRITE tagged with MAPLE_FUNC_LCD.
 *
 * The Maple bus controller (hw/input/dreamcast_maple.c) forwards frames
 * addressed to the VMU's sub-unit here via dc_vmu_maple().
 *
 * Copyright (c) 2026 Florian Fuchs
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/sysbus.h"
#include "hw/sh4/sh.h"
#include "system/block-backend.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"

#define TYPE_DC_VMU "dc-vmu"
DECLARE_INSTANCE_CHECKER(DCVmu, DC_VMU, TYPE_DC_VMU)

#define VMU_BLOCK_SIZE   512
#define VMU_NUM_BLOCKS   256
#define VMU_SIZE         (VMU_BLOCK_SIZE * VMU_NUM_BLOCKS)   /* 128 KB */

/* Storage-function geometry advertised in DEVINFO (see vmu_connect()). */
#define VMU_ROOT_BLOCK   255        /* numblocks = root + 1 = 256          */
#define VMU_FAT_BLOCK    254
#define VMU_DIR_BLOCK    253
#define VMU_DIR_COUNT    13
#define VMU_USER_BLOCKS  200

/* LCD: 48x32, 1 bit per pixel, MSB first, 192 bytes; scaled up for display. */
#define LCD_WIDTH        48
#define LCD_HEIGHT       32
#define LCD_FB_SIZE      (LCD_WIDTH * LCD_HEIGHT / 8)
#define LCD_MAGNIFY      6

/* Maple function codes and command / response codes. */
#define FUNC_MEMCARD     0x02
#define FUNC_LCD         0x04
#define CMD_DEVINFO      1
#define CMD_GETMINFO     10
#define CMD_BREAD        11
#define CMD_BWRITE       12
#define CMD_BSYNC        13
#define RESP_DEVINFO     5
#define RESP_OK          7
#define RESP_DATATRF     8
#define RESP_NONE        0xff

struct DCVmu {
    SysBusDevice parent_obj;

    BlockBackend *blk;
    bool lcd_enabled;

    /* LCD state (only used when lcd_enabled). */
    QemuConsole *con;
    uint8_t lcd_fb[LCD_FB_SIZE];
    bool redraw;
};

static void st_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void st_le16(uint8_t *p, uint16_t v)
{
    p[0] = v; p[1] = v >> 8;
}

/* ---- LCD display console ---------------------------------------------- */

static bool vmu_lcd_gfx_update(void *opaque)
{
    DCVmu *v = opaque;
    DisplaySurface *surface = qemu_console_surface(v->con);
    int bpp = surface_bits_per_pixel(surface);
    int bypp = (bpp + 7) >> 3;
    int stride = surface_stride(surface);
    uint8_t *base = surface_data(surface);
    uint32_t on, off;
    int x, y, dx, dy;

    if (!v->redraw) {
        return true;
    }

    /* Dark pixels on the VMU's pale green-grey background. */
    switch (bpp) {
    case 8:
        on = rgb_to_pixel8(0x18, 0x20, 0x18);
        off = rgb_to_pixel8(0x8c, 0xc0, 0x9c);
        break;
    case 15:
        on = rgb_to_pixel15(0x18, 0x20, 0x18);
        off = rgb_to_pixel15(0x8c, 0xc0, 0x9c);
        break;
    case 16:
        on = rgb_to_pixel16(0x18, 0x20, 0x18);
        off = rgb_to_pixel16(0x8c, 0xc0, 0x9c);
        break;
    case 24:
        on = rgb_to_pixel24(0x18, 0x20, 0x18);
        off = rgb_to_pixel24(0x8c, 0xc0, 0x9c);
        break;
    case 32:
        on = rgb_to_pixel32(0x18, 0x20, 0x18);
        off = rgb_to_pixel32(0x8c, 0xc0, 0x9c);
        break;
    default:
        return true;
    }

    for (y = 0; y < LCD_HEIGHT; y++) {
        for (x = 0; x < LCD_WIDTH; x++) {
            int bit = (v->lcd_fb[y * (LCD_WIDTH / 8) + (x >> 3)]
                       >> (7 - (x & 7))) & 1;
            uint32_t c = bit ? on : off;

            for (dy = 0; dy < LCD_MAGNIFY; dy++) {
                uint8_t *p = base + (y * LCD_MAGNIFY + dy) * stride +
                             (x * LCD_MAGNIFY) * bypp;
                for (dx = 0; dx < LCD_MAGNIFY; dx++) {
                    memcpy(p, &c, bypp);
                    p += bypp;
                }
            }
        }
    }

    v->redraw = false;
    qemu_console_update(v->con, 0, 0, LCD_WIDTH * LCD_MAGNIFY,
                        LCD_HEIGHT * LCD_MAGNIFY);
    return true;
}

static void vmu_lcd_invalidate(void *opaque)
{
    DCVmu *v = opaque;
    v->redraw = true;
}

static const GraphicHwOps vmu_lcd_ops = {
    .invalidate = vmu_lcd_invalidate,
    .gfx_update = vmu_lcd_gfx_update,
};

/* LCD BWRITE: [func][addr][192-byte framebuffer].  Repaint on the next frame. */
static int vmu_lcd_bwrite(DCVmu *v, const uint8_t *data, int datalen,
                          uint8_t host, uint8_t dev, uint8_t *buf)
{
    if (datalen < 8 + LCD_FB_SIZE) {
        return -1;
    }
    memcpy(v->lcd_fb, &data[8], LCD_FB_SIZE);
    v->redraw = true;

    buf[0] = RESP_OK;
    buf[1] = host;
    buf[2] = dev;
    buf[3] = 0;
    return 4;
}

/* ---- storage function -------------------------------------------------- */

/* DEVINFO: report the memory-card function (+ LCD when enabled). */
static int vmu_devinfo(DCVmu *v, uint8_t host, uint8_t dev, uint8_t *buf)
{
    uint32_t function = FUNC_MEMCARD | (v->lcd_enabled ? FUNC_LCD : 0);

    memset(buf, 0, 116);
    buf[0] = RESP_DEVINFO;
    buf[1] = host;
    buf[2] = dev;
    buf[3] = 28;                    /* payload length in words */
    st_be32(&buf[4], function);
    /*
     * function_data is ordered high-bit-first, and vmu-flash reads the storage
     * word at index hweight(function)-1.  With the LCD present that is index 1,
     * so LCD data goes first.  vmu-flash decodes the storage word as:
     *   partitions = (b>>24)+1 = 1, blocklen = ((b>>16 & 0xff)+1)<<5 = 512,
     *   writecnt = b>>12 & 0xf = 4, readcnt = b>>8 & 0xf = 1.
     */
    if (v->lcd_enabled) {
        st_be32(&buf[8],  0x00051000);   /* LCD (index 0; unused by driver) */
        st_be32(&buf[12], 0x000f4100);   /* storage (index 1)               */
    } else {
        st_be32(&buf[8],  0x000f4100);   /* storage (index 0)               */
    }
    buf[20] = 0xff;                /* area code             */
    buf[21] = 0x00;               /* connector direction   */
    memset(&buf[22], ' ', 30);
    memcpy(&buf[22], "Visual Memory", 13);
    memcpy(&buf[52],
           "Produced By or Under License From SEGA ENTERPRISES,LTD.     ", 60);
    return 116;
}

/*
 * GETMINFO: media info.  vmu-flash reads it as little-endian 16-bit words over
 * the whole response: res[6] (byte 12) = root block, res[12] (byte 24) = user
 * blocks; numblocks = root + 1.  media info therefore starts at byte 8.
 */
static int vmu_getminfo(uint8_t host, uint8_t dev, uint8_t *buf)
{
    memset(buf, 0, 32);
    buf[0] = RESP_DATATRF;
    buf[1] = host;
    buf[2] = dev;
    buf[3] = 7;                    /* function word + 6 data words */
    st_be32(&buf[4], FUNC_MEMCARD);
    st_le16(&buf[8],  VMU_ROOT_BLOCK);   /* [0] total blocks - 1        */
    st_le16(&buf[10], 0);                /* [1] partition number        */
    st_le16(&buf[12], VMU_ROOT_BLOCK);   /* [2] res[6]  -> root block   */
    st_le16(&buf[14], VMU_FAT_BLOCK);    /* [3] FAT block               */
    st_le16(&buf[16], 1);                /* [4] FAT block count         */
    st_le16(&buf[18], VMU_DIR_BLOCK);    /* [5] directory block         */
    st_le16(&buf[20], VMU_DIR_COUNT);    /* [6] directory block count   */
    st_le16(&buf[22], 0);                /* [7] icon shape              */
    st_le16(&buf[24], VMU_USER_BLOCKS);  /* [8] res[12] -> user blocks  */
    return 32;
}

static int vmu_bread(DCVmu *v, const uint8_t *data, int datalen,
                     uint8_t host, uint8_t dev, uint8_t *buf)
{
    uint32_t block;

    if (datalen < 8) {
        return -1;
    }
    block = data[7];               /* addr & 0xff (big-endian low byte) */
    if (block >= VMU_NUM_BLOCKS) {
        return -1;
    }

    memset(buf, 0, 12);
    buf[0] = RESP_DATATRF;
    buf[1] = host;
    buf[2] = dev;
    buf[3] = 2 + VMU_BLOCK_SIZE / 4;   /* func + addr + 128 data words */
    st_be32(&buf[4], FUNC_MEMCARD);
    memcpy(&buf[8], &data[4], 4);      /* echo the block address       */

    if (blk_pread(v->blk, (int64_t)block * VMU_BLOCK_SIZE, VMU_BLOCK_SIZE,
                  &buf[12], 0) < 0) {
        return -1;
    }
    return 12 + VMU_BLOCK_SIZE;
}

/* Storage BWRITE: one 128-byte phase written directly to its slice. */
static int vmu_bwrite(DCVmu *v, const uint8_t *data, int datalen,
                      uint8_t host, uint8_t dev, uint8_t *buf)
{
    uint32_t block, phase;
    int64_t ofs;

    if (datalen < 8 + 128) {
        return -1;
    }
    phase = data[5];               /* addr >> 16 & 0xff */
    block = data[7];               /* addr & 0xff       */
    if (block >= VMU_NUM_BLOCKS || phase >= 4) {
        return -1;
    }
    ofs = (int64_t)block * VMU_BLOCK_SIZE + (int64_t)phase * 128;
    if (blk_pwrite(v->blk, ofs, 128, &data[8], 0) < 0) {
        return -1;
    }

    buf[0] = RESP_OK;
    buf[1] = host;
    buf[2] = dev;
    buf[3] = 0;
    return 4;
}

static int vmu_bsync(DCVmu *v, uint8_t host, uint8_t dev, uint8_t *buf)
{
    blk_flush(v->blk);
    buf[0] = RESP_OK;
    buf[1] = host;
    buf[2] = dev;
    buf[3] = 0;
    return 4;
}

/*
 * Handle one Maple frame addressed to the VMU sub-unit.  Returns the response
 * length, or <=0 for "no/error response".  data/datalen are the raw command
 * data bytes; data[0..3] is the function code (big-endian), which selects the
 * memory-card vs LCD function for block writes.
 */
int dc_vmu_maple(DCVmu *v, uint8_t cmd, uint8_t host, uint8_t dev,
                 const uint8_t *data, int datalen, uint8_t *resp)
{
    switch (cmd) {
    case CMD_DEVINFO:
        return vmu_devinfo(v, host, dev, resp);
    case CMD_GETMINFO:
        return vmu_getminfo(host, dev, resp);
    case CMD_BREAD:
        return vmu_bread(v, data, datalen, host, dev, resp);
    case CMD_BWRITE:
        if (datalen >= 4 && v->lcd_enabled &&
            data[0] == 0 && data[1] == 0 && data[2] == 0 &&
            data[3] == FUNC_LCD) {
            return vmu_lcd_bwrite(v, data, datalen, host, dev, resp);
        }
        return vmu_bwrite(v, data, datalen, host, dev, resp);
    case CMD_BSYNC:
        return vmu_bsync(v, host, dev, resp);
    default:
        resp[0] = RESP_NONE;
        return 4;
    }
}

/* ---- QOM device -------------------------------------------------------- */

static void vmu_realize(DeviceState *dev, Error **errp)
{
    DCVmu *v = DC_VMU(dev);
    int64_t len;

    if (!v->blk) {
        error_setg(errp, "dc-vmu: no block backend connected");
        return;
    }
    len = blk_getlength(v->blk);
    if (len != VMU_SIZE) {
        error_setg(errp, "dc-vmu: image must be exactly %d bytes (128 KB), "
                   "got %" PRId64, VMU_SIZE, len);
        return;
    }
    if (blk_set_perm(v->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                     BLK_PERM_ALL, errp) < 0) {
        return;
    }

    if (v->lcd_enabled) {
        memset(v->lcd_fb, 0, sizeof(v->lcd_fb));
        v->redraw = true;
        v->con = qemu_graphic_console_create(dev, 0, &vmu_lcd_ops, v);
        qemu_console_resize(v->con, LCD_WIDTH * LCD_MAGNIFY,
                            LCD_HEIGHT * LCD_MAGNIFY);
    }
}

static const VMStateDescription vmstate_vmu = {
    .name = "dc-vmu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(lcd_fb, DCVmu, LCD_FB_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static void vmu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = vmu_realize;
    dc->vmsd = &vmstate_vmu;
    dc->user_creatable = false;
}

static const TypeInfo vmu_info = {
    .name          = TYPE_DC_VMU,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DCVmu),
    .class_init    = vmu_class_init,
};

static void vmu_register_types(void)
{
    type_register_static(&vmu_info);
}

type_init(vmu_register_types)

/* Create a VMU backed by blk (a 128 KB image); enable the LCD console if lcd. */
DCVmu *dc_vmu_new(BlockBackend *blk, bool lcd)
{
    DeviceState *dev = qdev_new(TYPE_DC_VMU);
    DCVmu *v = DC_VMU(dev);

    v->blk = blk;
    v->lcd_enabled = lcd;
    /* Give it a stable id so the LCD console can be targeted by
     * "screendump -d vmu" / QMP screendump device=vmu. */
    dev->id = g_strdup("vmu");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    return v;
}
