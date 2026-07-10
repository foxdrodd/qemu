/*
 * Sega Dreamcast Holly (System ASIC) interrupt controller
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
 *
 * The 96 hardware-event lines are exposed as qdev GPIO inputs; on-board
 * peripherals connect their IRQ outputs to qdev_get_gpio_in(holly, HOLLY_EV_*).
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
#include "hw/core/irq.h"
#include "qom/object.h"

#define TYPE_DC_HOLLY "dc-holly"
OBJECT_DECLARE_SIMPLE_TYPE(DCHollyState, DC_HOLLY)

#define HOLLY_INTC_SIZE 0x40
#define HOLLY_NR_EVENTS 96

struct DCHollyState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irl;           /* encoded IRL line into the SH-4 INTC         */

    uint32_t esr[3];        /* 0x00/0x04/0x08 : ISTNRM / ISTEXT / ISTERR   */
    uint32_t emr[12];       /* 0x10..0x3c     : event mask registers       */
};

static void holly_update_irl(DCHollyState *s)
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
    DCHollyState *s = DC_HOLLY(opaque);

    if (level) {
        s->esr[n >> 5] |= 1u << (n & 31);
        holly_update_irl(s);
    }
}

static uint64_t holly_read(void *opaque, hwaddr addr, unsigned int size)
{
    DCHollyState *s = opaque;

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
    DCHollyState *s = opaque;

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

static void holly_realize(DeviceState *dev, Error **errp)
{
    DCHollyState *s = DC_HOLLY(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &holly_ops, s, "dc-holly-intc",
                          HOLLY_INTC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irl);          /* output: encoded SH-4 IRL */
    qdev_init_gpio_in(dev, holly_event_set, HOLLY_NR_EVENTS);
}

static void holly_reset_hold(Object *obj, ResetType type)
{
    DCHollyState *s = DC_HOLLY(obj);

    memset(s->esr, 0, sizeof(s->esr));
    memset(s->emr, 0, sizeof(s->emr));
    holly_update_irl(s);                    /* nothing pending -> deassert */
}

static const VMStateDescription vmstate_dreamcast_holly = {
    .name = "dc-holly",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(esr, DCHollyState, 3),
        VMSTATE_UINT32_ARRAY(emr, DCHollyState, 12),
        VMSTATE_END_OF_LIST()
    }
};

static void holly_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = holly_realize;
    rc->phases.hold = holly_reset_hold;
    dc->vmsd = &vmstate_dreamcast_holly;
}

static const TypeInfo holly_info = {
    .name          = TYPE_DC_HOLLY,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DCHollyState),
    .class_init    = holly_class_init,
};

static void holly_register_types(void)
{
    type_register_static(&holly_info);
}

type_init(holly_register_types)

/*
 * Board helper: create the Holly interrupt controller, map its registers, and
 * connect its encoded IRL output to the SH-4 INTC.  Returns the DeviceState so
 * the board can wire peripheral IRQs to qdev_get_gpio_in(dev, HOLLY_EV_*).
 */
DeviceState *dc_holly_init(hwaddr base, qemu_irq irl_out)
{
    DeviceState *dev = qdev_new(TYPE_DC_HOLLY);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, base);
    sysbus_connect_irq(sbd, 0, irl_out);
    return dev;
}
