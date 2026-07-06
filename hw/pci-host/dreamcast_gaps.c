/*
 * Sega Dreamcast "GAPS" PCI bridge (Broadband Adapter, HIT-0400)
 *
 * The Dreamcast Broadband Adapter is a RealTek RTL8139C sitting behind SEGA's
 * GAPS PCI bridge on the G2 expansion bus.  Linux drives it with the stock
 * 8139too PCI driver plus arch/sh/drivers/pci/{pci,ops,fixups}-dreamcast.c.
 *
 * This models the GAPS bridge and reuses QEMU's rtl8139 device unchanged.  The
 * bridge presents, on the G2 bus:
 *
 *   0x01001400  control regs   - "GAPSPCI_BRIDGE_2" id + init handshake
 *   0x01001600  PCI config     - linear window onto the RTL8139 config space
 *   0x01001700  RTL8139 regs   - the chip's memory BAR, fixed-decoded here
 *   0x01840000  32 KB DMA SRAM - coherent window the driver DMAs rings into
 *
 * The PCI bus DMA/memory space is the SH system memory (the kernel uses
 * io_offset = mem_offset = 0, so bus address == CPU physical address), which
 * makes both the register decode and bus-master DMA fall out 1:1.
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
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci_device.h"
#include "net/net.h"
#include "system/address-spaces.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_DREAMCAST_GAPS "dreamcast-gaps"
OBJECT_DECLARE_SIMPLE_TYPE(DCGapsState, DREAMCAST_GAPS)

/* G2-bus addresses (physical; the kernel reaches them via P2 0xa10xxxxx). */
#define GAPS_REGS_BASE    0x01001400
#define GAPS_REGS_SIZE    0x100
#define GAPS_CONFIG_BASE  0x01001600
#define GAPS_CONFIG_SIZE  0x100
#define GAPS_BBA_REGS     0x01001700   /* = config + 0x100; fixed reg decode  */
#define GAPS_BBA_REGS_SZ  0x100        /* RTL8139 register block (memory BAR) */
#define GAPS_BBA_BAR      0x01000000   /* where gapspci_init points BAR1      */
#define GAPS_DMA_BASE     0x01840000
#define GAPS_DMA_SIZE     0x8000       /* 32 KB coherent DMA SRAM             */

/* The Broadband Adapter reports SEGA PCI ids; 8139too binds only on these. */
#define PCI_VENDOR_SEGA   0x11db
#define PCI_DEVICE_SEGA_BBA 0x1234

/* GAPS control-register offsets (from GAPS_REGS_BASE). */
#define GAPS_REG_ID       0x00         /* 16-byte "GAPSPCI_BRIDGE_2"          */
#define GAPS_REG_INIT     0x18         /* write magic -> reads back 1 (ready) */
#define GAPS_INIT_MAGIC   0x5a14a501

struct DCGapsState {
    PCIHostState parent_obj;

    PCIDevice *rtl;
    qemu_irq irq;

    MemoryRegion regs_mem;             /* control registers  */
    MemoryRegion config_mem;          /* PCI config window  */
    MemoryRegion bba_regs;            /* fixed-decode alias of the RTL8139 BAR */
    MemoryRegion dma_sram;            /* 32 KB DMA SRAM     */

    bool ready;                        /* set by the init handshake */
};

/* ---- GAPS control registers ------------------------------------------- */

static uint64_t gaps_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    DCGapsState *s = opaque;
    static const char id[16] = "GAPSPCI_BRIDGE_2";

    if (addr < GAPS_REG_ID + sizeof(id)) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size && addr + i < sizeof(id); i++) {
            v |= (uint8_t)id[addr + i] << (8 * i);
        }
        return v;
    }
    if (addr == GAPS_REG_INIT) {
        return s->ready ? 1 : 0;
    }
    return 0;
}

static void gaps_regs_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    DCGapsState *s = opaque;

    if (addr == GAPS_REG_INIT && val == GAPS_INIT_MAGIC) {
        s->ready = true;               /* bridge init complete */
    }
    /* Window/DMA-range regs (+0x14/+0x20..+0x2c/+0x34) are accepted, no-op. */
}

static const MemoryRegionOps gaps_regs_ops = {
    .read = gaps_regs_read,
    .write = gaps_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* ---- PCI config-space window ------------------------------------------ */
/*
 * Linear config window onto the RTL8139 (devfn 0).  Writes are forwarded
 * verbatim - including BAR1 - so Linux's BAR sizing works and resource[1]
 * is registered as IORESOURCE_MEM.  gapspci_init points BAR1 at
 * GAPS_BBA_BAR, and fixups-dreamcast overrides resource[1] to GAPS_BBA_REGS;
 * the fixed-decode alias below bridges that back to the mapped BAR.
 */
static uint64_t gaps_config_read(void *opaque, hwaddr addr, unsigned size)
{
    DCGapsState *s = opaque;
    PCIHostState *phb = PCI_HOST_BRIDGE(s);

    return pci_data_read(phb->bus, addr, size);
}

static void gaps_config_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    DCGapsState *s = opaque;
    PCIHostState *phb = PCI_HOST_BRIDGE(s);

    pci_data_write(phb->bus, addr, val, size);
}

static const MemoryRegionOps gaps_config_ops = {
    .read = gaps_config_read,
    .write = gaps_config_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* ---- PCI IRQ routing (single shared INTA -> Holly external event) ------ */

static int gaps_map_irq(PCIDevice *d, int irq_num)
{
    return 0;
}

static void gaps_set_irq(void *opaque, int irq_num, int level)
{
    DCGapsState *s = opaque;

    qemu_set_irq(s->irq, level);
}

static void gaps_realize(DeviceState *dev, Error **errp)
{
    DCGapsState *s = DREAMCAST_GAPS(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    MemoryRegion *sysmem = get_system_memory();
    uint8_t *cfg;

    sysbus_init_irq(sbd, &s->irq);

    /*
     * PCI bus whose memory and DMA space IS the SH system memory: bus address
     * == CPU physical, so the RTL8139 register BAR lands directly on the G2
     * bus and bus-master DMA reaches the SRAM window without translation.
     */
    phb->bus = pci_register_root_bus(dev, "gaps-pci",
                                     gaps_set_irq, gaps_map_irq, s,
                                     sysmem, get_system_io(),
                                     PCI_DEVFN(0, 0), 1, TYPE_PCI_BUS);

    /* Control registers, config window and DMA SRAM on the G2 bus. */
    memory_region_init_io(&s->regs_mem, OBJECT(s), &gaps_regs_ops, s,
                          "dc-gaps-regs", GAPS_REGS_SIZE);
    memory_region_add_subregion(sysmem, GAPS_REGS_BASE, &s->regs_mem);

    memory_region_init_io(&s->config_mem, OBJECT(s), &gaps_config_ops, s,
                          "dc-gaps-config", GAPS_CONFIG_SIZE);
    memory_region_add_subregion(sysmem, GAPS_CONFIG_BASE, &s->config_mem);

    memory_region_init_ram(&s->dma_sram, OBJECT(s), "dc-gaps-sram",
                           GAPS_DMA_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, GAPS_DMA_BASE, &s->dma_sram);

    /*
     * Fixed-decode alias: the RTL8139 memory BAR is mapped (by gapspci_init)
     * at GAPS_BBA_BAR, but fixups-dreamcast makes the driver ioremap
     * GAPS_BBA_REGS instead.  Real GAPS decodes the registers at that fixed
     * G2 address regardless of the BAR; mirror that with an alias of system
     * memory back onto the mapped BAR.
     */
    memory_region_init_alias(&s->bba_regs, OBJECT(s), "dc-gaps-bba-regs",
                             sysmem, GAPS_BBA_BAR, GAPS_BBA_REGS_SZ);
    memory_region_add_subregion(sysmem, GAPS_BBA_REGS, &s->bba_regs);

    /* The RTL8139C behind the bridge. */
    s->rtl = pci_new(PCI_DEVFN(0, 0), "rtl8139");
    qemu_configure_nic_device(DEVICE(s->rtl), true, NULL);
    if (!pci_realize_and_unref(s->rtl, phb->bus, errp)) {
        return;
    }

    /*
     * Present the SEGA Broadband Adapter ids: mainline 8139too and
     * fixups-dreamcast bind only on 0x11db:0x1234 (the RTL8139 model
     * otherwise reports RealTek 0x10ec:0x8139).
     */
    cfg = s->rtl->config;
    pci_config_set_vendor_id(cfg, PCI_VENDOR_SEGA);
    pci_config_set_device_id(cfg, PCI_DEVICE_SEGA_BBA);
}

static void gaps_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = gaps_realize;
    dc->user_creatable = false;
}

static const TypeInfo gaps_info = {
    .name          = TYPE_DREAMCAST_GAPS,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(DCGapsState),
    .class_init    = gaps_class_init,
};

static void gaps_register_types(void)
{
    type_register_static(&gaps_info);
}

type_init(gaps_register_types)

/* Board helper: create the GAPS bridge + RTL8139, wire the external IRQ. */
void dc_gaps_init(qemu_irq irq)
{
    DeviceState *dev;
    SysBusDevice *sbd;

    dev = qdev_new(TYPE_DREAMCAST_GAPS);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_connect_irq(sbd, 0, irq);
}
