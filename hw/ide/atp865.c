/*
 * QEMU ACARD (ARTOP) ATP865 PCI UDMA133 IDE controller emulation
 *
 * Minimal model: two native-mode channels with a standard bus master DMA
 * block. The timing registers in config space are plain storage, which is
 * all that the Linux pata_artop and aec62xx drivers need.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/ide/pci.h"
#include "hw/core/irq.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "ide-internal.h"

#define TYPE_ATP865_IDE "atp865-ide"
OBJECT_DECLARE_SIMPLE_TYPE(ATP865State, ATP865_IDE)

#define PCI_VENDOR_ID_ARTOP         0x1191
#define PCI_DEVICE_ID_ARTOP_ATP865  0x0008

/* Config space registers */
#define ATP865_CABLE    0x49    /* bit n set: 40-wire cable on channel n */
#define ATP865_CTRL     0x4a    /* bit 1/2: channel 0/1 enable */
#define ATP865_CTRL_EN  0x06

struct ATP865State {
    PCIIDEState parent_obj;

    bool irq_level[2];
};

static uint64_t atp865_bmdma_read(void *opaque, hwaddr addr, unsigned size)
{
    BMDMAState *bm = opaque;

    switch (addr) {
    case 0:
        return bm->cmd;
    case 2:
        return bm->status;
    }

    return 0xff;
}

static void atp865_bmdma_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    BMDMAState *bm = opaque;

    switch (addr) {
    case 0:
        bmdma_cmd_writeb(bm, val);
        break;
    case 2:
        bmdma_status_writeb(bm, val);
        break;
    }
}

static const MemoryRegionOps atp865_bmdma_ops = {
    .read = atp865_bmdma_read,
    .write = atp865_bmdma_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void atp865_bmdma_setup_bar(PCIIDEState *d)
{
    int i;

    memory_region_init(&d->bmdma_bar, OBJECT(d), "atp865-bmdma", 16);
    for (i = 0; i < 2; i++) {
        BMDMAState *bm = &d->bmdma[i];

        memory_region_init_io(&bm->extra_io, OBJECT(d), &atp865_bmdma_ops, bm,
                              "atp865-bmdma-bus", 4);
        memory_region_add_subregion(&d->bmdma_bar, i * 8, &bm->extra_io);
        memory_region_init_io(&bm->addr_ioport, OBJECT(d),
                              &bmdma_addr_ioport_ops, bm,
                              "atp865-bmdma-ioport", 4);
        memory_region_add_subregion(&d->bmdma_bar, i * 8 + 4, &bm->addr_ioport);
    }
}

static void atp865_set_irq(void *opaque, int channel, int level)
{
    ATP865State *s = opaque;

    s->irq_level[channel] = level;
    pci_set_irq(PCI_DEVICE(s), s->irq_level[0] || s->irq_level[1]);
}

static void atp865_reset(DeviceState *dev)
{
    PCIIDEState *d = PCI_IDE(dev);
    uint8_t *pci_conf = PCI_DEVICE(dev)->config;
    int i;

    for (i = 0; i < 2; i++) {
        ide_bus_reset(&d->bus[i]);
    }

    memset(pci_conf + 0x40, 0, 0x20);
    pci_conf[ATP865_CTRL] = ATP865_CTRL_EN;
}

static void atp865_realize(PCIDevice *dev, Error **errp)
{
    PCIIDEState *d = PCI_IDE(dev);
    DeviceState *ds = DEVICE(dev);
    int i;

    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    for (i = 0; i < 2; i++) {
        memory_region_init_io(&d->data_bar[i], OBJECT(d), &pci_ide_data_le_ops,
                              &d->bus[i], "atp865-data", 8);
        pci_register_bar(dev, i * 2, PCI_BASE_ADDRESS_SPACE_IO,
                         &d->data_bar[i]);

        memory_region_init_io(&d->cmd_bar[i], OBJECT(d), &pci_ide_cmd_le_ops,
                              &d->bus[i], "atp865-cmd", 4);
        pci_register_bar(dev, i * 2 + 1, PCI_BASE_ADDRESS_SPACE_IO,
                         &d->cmd_bar[i]);
    }

    atp865_bmdma_setup_bar(d);
    pci_register_bar(dev, 4, PCI_BASE_ADDRESS_SPACE_IO, &d->bmdma_bar);

    qdev_init_gpio_in(ds, atp865_set_irq, 2);
    for (i = 0; i < 2; i++) {
        ide_bus_init(&d->bus[i], sizeof(d->bus[i]), ds, i, 2);
        ide_bus_init_output_irq(&d->bus[i], qdev_get_gpio_in(ds, i));

        bmdma_init(&d->bus[i], &d->bmdma[i], d);
        ide_bus_register_restart_cb(&d->bus[i]);
    }
}

static void atp865_exit(PCIDevice *dev)
{
    PCIIDEState *d = PCI_IDE(dev);
    int i;

    for (i = 0; i < 2; i++) {
        memory_region_del_subregion(&d->bmdma_bar, &d->bmdma[i].extra_io);
        memory_region_del_subregion(&d->bmdma_bar, &d->bmdma[i].addr_ioport);
    }
}

static void atp865_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, atp865_reset);
    dc->vmsd = &vmstate_ide_pci;
    dc->desc = "ACARD ATP865 UDMA133 IDE controller";
    k->realize = atp865_realize;
    k->exit = atp865_exit;
    k->vendor_id = PCI_VENDOR_ID_ARTOP;
    k->device_id = PCI_DEVICE_ID_ARTOP_ATP865;
    k->revision = 0x10;
    k->class_id = PCI_CLASS_STORAGE_OTHER;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo atp865_types[] = {
    {
        .name          = TYPE_ATP865_IDE,
        .parent        = TYPE_PCI_IDE,
        .instance_size = sizeof(ATP865State),
        .class_init    = atp865_class_init,
    },
};

DEFINE_TYPES(atp865_types)
