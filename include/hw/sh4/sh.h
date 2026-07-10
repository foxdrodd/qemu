/*
 * Definitions for SH board emulation
 *
 * Copyright (c) 2005 Samuel Tardieu
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef QEMU_HW_SH_H
#define QEMU_HW_SH_H

#include "hw/sh4/sh_intc.h"
#include "target/sh4/cpu-qom.h"

#define A7ADDR(x) ((x) & 0x1fffffff)
#define P4ADDR(x) ((x) | 0xe0000000)

/* sh7750.c */
struct SH7750State;

struct SH7750State *sh7750_init(SuperHCPU *cpu, MemoryRegion *sysmem);

#define TYPE_SH_SERIAL "sh-serial"
#define SH_SERIAL_FEAT_SCIF (1 << 0)

/* sh7750.c */
qemu_irq sh7750_irl(struct SH7750State *s);
void sh7750_set_porta(struct SH7750State *s, uint16_t dir_mask, uint16_t value);
/* On-chip DMAC latch, for board-level cascade engines (Dreamcast CH2 DMA). */
uint32_t sh7750_dmac_sar(struct SH7750State *s, unsigned channel);
void sh7750_dmac_transfer_done(struct SH7750State *s, unsigned channel,
                               uint32_t bytes);

/* hw/intc/dreamcast_holly.c - System ASIC interrupt controller.
 * Hardware-event line numbers used by on-board peripherals (Linux
 * arch/sh/boards/mach-dreamcast/irq.c): the bit within ISTNRM/ISTEXT. */
enum {
    HOLLY_EV_VSYNC     = 5,   /* ISTNRM bit  5 -> IRQ13 (video vblank)      */
    HOLLY_EV_MAPLE_DMA = 12,  /* ISTNRM bit 12 -> IRQ13                     */
    HOLLY_EV_GDROM_DMA = 14,  /* ISTNRM bit 14 -> IRQ13                     */
    HOLLY_EV_CH2_DMA   = 19,  /* ISTNRM bit 19 -> IRQ13 (PVR/CH2 DMA end)   */
    HOLLY_EV_GDROM_CMD = 32,  /* ISTEXT bit  0 -> IRQ11                     */
    HOLLY_EV_LAN       = 34,  /* ISTEXT bit  2 -> IRQ11 (G2 external)       */
    HOLLY_EV_EXTERNAL  = 35,  /* ISTEXT bit  3 -> IRQ11 (BBA / GAPS PCI)    */
};
DeviceState *dc_holly_init(hwaddr base, qemu_irq irl_out);

/* hw/dma/dreamcast_ch2dma.c - Holly CH2 ("PVR") DMA. */
void dc_ch2dma_init(hwaddr base, struct SH7750State *sh, qemu_irq irq);

/* hw/block/dreamcast_gdrom.c - GD-ROM drive. dc_gdrom_read_logical() and
 * dc_gdrom_data_lba() serve the board's boot-from-disc firmware. */
DeviceState *dc_gdrom_init(hwaddr base, qemu_irq irq_cmd, qemu_irq irq_dma,
                           BlockBackend *blk);
bool dc_gdrom_read_logical(DeviceState *dev, uint32_t lba, uint32_t nsec,
                           uint8_t *buf);
uint32_t dc_gdrom_data_lba(DeviceState *dev);

/* hw/net/dreamcast_la.c */
void dc_lanadapter_init(hwaddr base, qemu_irq irq);

/* hw/pci-host/dreamcast_gaps.c */
void dc_gaps_init(qemu_irq irq);

/* hw/display/dreamcast_pvr.c */
void dc_pvr_init(hwaddr base, MemoryRegion *vram, qemu_irq vblank_irq);

/* hw/block/dreamcast_vmu.c */
typedef struct DCVmu DCVmu;
DCVmu *dc_vmu_new(BlockBackend *blk, bool lcd);
int dc_vmu_maple(DCVmu *v, uint8_t cmd, uint8_t host, uint8_t dev,
                 const uint8_t *data, int datalen, uint8_t *resp);

/* hw/input/dreamcast_maple.c */
void dc_maple_init(hwaddr base, qemu_irq irq, DCVmu *vmu);

#endif
