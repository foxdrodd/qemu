/*
 * Sega Dreamcast Visual Memory Unit (VMU) - storage function
 *
 * A VMU is a Maple bus peripheral that plugs into a controller's expansion
 * slot.  Its 128 KB flash is exposed by Linux's drivers/mtd/maps/vmu-flash.c
 * as a 256-block x 512-byte MTD (function MAPLE_FUNC_MEMCARD), on top of which
 * fs/vmufat mounts.  This models just the storage function, backed by a
 * QEMU BlockBackend (a 128 KB host image).
 *
 * The Maple storage command set (DEVINFO / GETMINFO / BREAD / BWRITE / BSYNC)
 * follows include/linux/maple.h and drivers/mtd/maps/vmu-flash.c.  The Maple
 * bus controller (hw/input/dreamcast_maple.c) forwards frames addressed to the
 * VMU's sub-unit here via dc_vmu_maple().
 *
 * Copyright (c) 2026 Florian Fuchs
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/sh4/sh.h"
#include "system/block-backend.h"

#define VMU_BLOCK_SIZE   512
#define VMU_NUM_BLOCKS   256
#define VMU_SIZE         (VMU_BLOCK_SIZE * VMU_NUM_BLOCKS)   /* 128 KB */

/* Storage-function geometry advertised in DEVINFO (see vmu_connect()). */
#define VMU_ROOT_BLOCK   255        /* numblocks = root + 1 = 256          */
#define VMU_FAT_BLOCK    254
#define VMU_DIR_BLOCK    253
#define VMU_DIR_COUNT    13
#define VMU_USER_BLOCKS  200

/* Maple function code and command / response codes. */
#define FUNC_MEMCARD     0x02
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
    BlockBackend *blk;
};

static void st_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void st_le16(uint8_t *p, uint16_t v)
{
    p[0] = v; p[1] = v >> 8;
}

/* DEVINFO: report the memory-card function and its geometry word. */
static int vmu_devinfo(uint8_t host, uint8_t dev, uint8_t *buf)
{
    memset(buf, 0, 116);
    buf[0] = RESP_DEVINFO;
    buf[1] = host;
    buf[2] = dev;
    buf[3] = 28;                    /* payload length in words */
    st_be32(&buf[4], FUNC_MEMCARD);
    /*
     * function_data[0], decoded by vmu_connect():
     *   partitions = (b>>24)+1 = 1, blocklen = ((b>>16 & 0xff)+1)<<5 = 512,
     *   writecnt = b>>12 & 0xf = 4, readcnt = b>>8 & 0xf = 1.
     */
    st_be32(&buf[8], 0x000f4100);
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

/*
 * BREAD: data = [func][addr]; addr = partition<<24 | phase<<16 | block.
 * With readcnt=1 the whole 512-byte block is returned in one frame, with the
 * data placed at response byte 12 (vmu_blockread reads recvbuf->buf + 12).
 */
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

/*
 * BWRITE: data = [func][addr][128 bytes]; write one 128-byte phase directly
 * to its slice of the block.  (A block is written as writecnt=4 phases, each
 * a distinct 128-byte region, so no accumulation buffer is needed.)
 */
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
 * Handle one storage-function Maple frame addressed to the VMU.  Returns the
 * response length in bytes, or <=0 to signal "no/So error response" (the
 * caller then writes a RESP_NONE).  data/datalen are the raw command data
 * bytes (data[0..3] = function, big-endian).
 */
int dc_vmu_maple(DCVmu *v, uint8_t cmd, uint8_t host, uint8_t dev,
                 const uint8_t *data, int datalen, uint8_t *resp)
{
    switch (cmd) {
    case CMD_DEVINFO:
        return vmu_devinfo(host, dev, resp);
    case CMD_GETMINFO:
        return vmu_getminfo(host, dev, resp);
    case CMD_BREAD:
        return vmu_bread(v, data, datalen, host, dev, resp);
    case CMD_BWRITE:
        return vmu_bwrite(v, data, datalen, host, dev, resp);
    case CMD_BSYNC:
        return vmu_bsync(v, host, dev, resp);
    default:
        resp[0] = RESP_NONE;
        return 4;
    }
}

/* Create a VMU backed by blk (must be a 128 KB image). */
DCVmu *dc_vmu_new(BlockBackend *blk)
{
    DCVmu *v;
    int64_t len;

    len = blk_getlength(blk);
    if (len != VMU_SIZE) {
        error_report("dc-vmu: image must be exactly %d bytes (128 KB), got %"
                     PRId64, VMU_SIZE, len);
        exit(1);
    }

    /* We read and write the flash; request those permissions on the backend. */
    if (blk_set_perm(blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                     BLK_PERM_ALL, &error_fatal) < 0) {
        exit(1);
    }

    v = g_new0(DCVmu, 1);
    v->blk = blk;
    return v;
}
