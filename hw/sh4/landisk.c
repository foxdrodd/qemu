/*
 * I-O DATA LANDISK (HDL-U, Plextor PX-EH) emulation
 *
 * SH7751R, 64 MB SDRAM in area 3, a CPLD in area 4 for LEDs, switches,
 * shutdown and the IRL interrupt mask, and on the PCI bus an RTL8139C+
 * and an ACARD ATP865 IDE controller.
 *
 * A kernel given with -kernel is placed where its zImage is linked and
 * entered directly, with the boot parameters the sh-lilo second stage would
 * have set up.
 *
 * Without -kernel the CPU starts in the boot ROM given with -bios. The real
 * SH-IPL+g is not emulated; a stand-in ROM can use the "BIOS call" registers
 * at BIOS_BASE to have the disk-read service of the firmware's trapa #0x3f
 * interface done by QEMU.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "target/sh4/cpu.h"
#include "hw/core/sysbus.h"
#include "hw/sh4/sh.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "hw/core/boards.h"
#include "hw/pci/pci.h"
#include "hw/ide/pci.h"
#include "hw/ide/mmio.h"
#include "net/net.h"
#include "sh7750_regs.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "qemu/log.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "exec/tswap.h"

#define SDRAM_BASE 0x0c000000 /* Area 3 */
#define SDRAM_SIZE (64 * MiB)

#define CPLD_BASE  0x10000000 /* Area 4, seen by the kernel at 0xb0000000 */
#define CF_BASE    0x14000000 /* Area 5, CompactFlash (USL-5P style boards) */

#define ROM_BASE   0x00000000 /* Area 0 */
#define ROM_SIZE   (512 * KiB)

/*
 * Paravirtual firmware services, not present on the hardware. A boot ROM's
 * trapa #0x3f handler stores the caller's r0, r4-r7 here; the store to
 * BIOS_ARG_R7 runs the call and BIOS_RESULT returns the value for r0.
 */
#define BIOS_BASE   0x18000000 /* Area 6 */
#define BIOS_FN     0x00
#define BIOS_ARG_R4 0x04
#define BIOS_ARG_R5 0x08
#define BIOS_ARG_R6 0x0c
#define BIOS_ARG_R7 0x10
#define BIOS_RESULT 0x14

/* Function numbers of the SH-IPL+g BIOS interface, as used by sh-lilo */
#define BIOS_FN_SERIAL_IN    1
#define BIOS_FN_READ_SECTORS 2
#define BIOS_FN_FEATURE      3
#define BIOS_FN_MEMORY_SIZE  4
#define BIOS_FN_IO_BASE      5
#define BIOS_FN_CACHE        6

/* CONFIG_ZERO_PAGE_OFFSET and CONFIG_BOOT_LINK_OFFSET of the Linux kernel */
#define BOOT_PARAMS_OFFSET 0x0001000
#define LINUX_LOAD_OFFSET  0x0800000
#define INITRD_LOAD_OFFSET 0x1800000

/* CPLD registers, one byte each */
#define PA_ATARST     0x00
#define PA_LED        0x01
#define PA_STATUS     0x02
#define PA_SHUTDOWN   0x03
#define PA_PCIPME     0x04
#define PA_IMASK      0x05
#define PA_PWRINT_CLR 0x06

#define PA_STATUS_POWER 0x10

/* Interrupt sources in PA_IMASK bit order; source n is raised as IRL 5 + n */
enum landisk_irq {
    PCI_INTA, PCI_INTB, PCI_INTC, PCI_INTD, ATA, FATA, POWER, BUTTON,
    NR_IRQS
};
#define IRL_BASE 5

/*
 * PCI slots. The hardware has the NIC in slot 0, the ATP865 in slot 1 and
 * the USB controller in slot 2; here slot 0 is taken by the host bridge.
 */
#define SLOT_NIC "2"
#define SLOT_IDE 1

typedef struct {
    uint8_t atarst;
    uint8_t led;
    uint8_t imask;
    uint8_t pending;
    uint8_t status;

    Notifier powerdown;

    qemu_irq irl;
    IRQState irq[NR_IRQS];
    MemoryRegion iomem;
} LandiskCPLD;

static void landisk_update_irl(LandiskCPLD *s)
{
    uint8_t active = s->pending & s->imask;
    int irl = 15;

    if (active) {
        irl = IRL_BASE + ctz32(active);
    }
    qemu_set_irq(s->irl, irl ^ 15);
}

static void landisk_irq_set(void *opaque, int n, int level)
{
    LandiskCPLD *s = opaque;

    if (level) {
        s->pending |= 1 << n;
    } else {
        s->pending &= ~(1 << n);
    }
    landisk_update_irl(s);
}

static uint64_t landisk_cpld_read(void *opaque, hwaddr addr, unsigned int size)
{
    LandiskCPLD *s = opaque;

    switch (addr) {
    case PA_ATARST:
        return s->atarst;
    case PA_LED:
        return s->led;
    case PA_STATUS:
        /* Switches are active low; power switch on, nothing pressed */
        return s->status;
    case PA_IMASK:
        return s->imask;
    }

    return 0;
}

static void landisk_cpld_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    LandiskCPLD *s = opaque;

    switch (addr) {
    case PA_ATARST:
        s->atarst = value;
        break;
    case PA_LED:
        s->led = value;
        break;
    case PA_SHUTDOWN:
        if (value & 1) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
        break;
    case PA_IMASK:
        s->imask = value;
        landisk_update_irl(s);
        break;
    case PA_PWRINT_CLR:
        s->pending &= ~(1 << POWER);
        landisk_update_irl(s);
        break;
    }
}

/* system_powerdown flips the power switch off; it stays off like the rocker */
static void landisk_powerdown_req(Notifier *n, void *opaque)
{
    LandiskCPLD *s = container_of(n, LandiskCPLD, powerdown);

    s->status &= ~PA_STATUS_POWER;
    s->pending |= 1 << POWER;
    landisk_update_irl(s);
}

static const MemoryRegionOps landisk_cpld_ops = {
    .read = landisk_cpld_read,
    .write = landisk_cpld_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static LandiskCPLD *landisk_cpld_init(MemoryRegion *sysmem, hwaddr base,
                                      qemu_irq irl)
{
    LandiskCPLD *s = g_new0(LandiskCPLD, 1);

    s->irl = irl;
    s->status = 0xff;
    s->powerdown.notify = landisk_powerdown_req;
    qemu_register_powerdown_notifier(&s->powerdown);

    memory_region_init_io(&s->iomem, NULL, &landisk_cpld_ops, s,
                          "landisk-cpld", 0x10);
    memory_region_add_subregion(sysmem, base, &s->iomem);

    qemu_init_irqs(s->irq, NR_IRQS, landisk_irq_set, s);

    return s;
}

typedef struct {
    uint32_t fn;
    uint32_t arg[4];
    uint32_t result;

    BlockBackend *blk;
    MemoryRegion iomem;
} LandiskBIOS;

static uint32_t landisk_bios_read_sectors(LandiskBIOS *s)
{
    uint64_t lba = s->arg[1];
    hwaddr buf = s->arg[2] & 0x1fffffff;
    uint32_t count = s->arg[3];
    uint8_t sector[BDRV_SECTOR_SIZE];

    if (!s->blk || s->arg[0] != 0) {
        return -1;
    }

    while (count--) {
        if (blk_pread(s->blk, lba * BDRV_SECTOR_SIZE, sizeof(sector), sector,
                      0) < 0) {
            return -1;
        }
        address_space_write(&address_space_memory, buf, MEMTXATTRS_UNSPECIFIED,
                            sector, sizeof(sector));
        lba++;
        buf += sizeof(sector);
    }

    return 0;
}

static void landisk_bios_call(LandiskBIOS *s)
{
    switch (s->fn) {
    case BIOS_FN_SERIAL_IN:
        s->result = -1;
        break;
    case BIOS_FN_READ_SECTORS:
        s->result = landisk_bios_read_sectors(s);
        break;
    case BIOS_FN_MEMORY_SIZE:
        s->result = SDRAM_SIZE;
        break;
    case BIOS_FN_FEATURE:
    case BIOS_FN_IO_BASE:
    case BIOS_FN_CACHE:
        s->result = 0;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "landisk: BIOS function %u not implemented\n",
                      s->fn);
        s->result = 0;
        break;
    }
}

static uint64_t landisk_bios_read(void *opaque, hwaddr addr, unsigned int size)
{
    LandiskBIOS *s = opaque;

    return addr == BIOS_RESULT ? s->result : 0;
}

static void landisk_bios_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    LandiskBIOS *s = opaque;

    switch (addr) {
    case BIOS_FN:
        s->fn = value;
        break;
    case BIOS_ARG_R4 ... BIOS_ARG_R7:
        s->arg[(addr - BIOS_ARG_R4) / 4] = value;
        if (addr == BIOS_ARG_R7) {
            landisk_bios_call(s);
        }
        break;
    }
}

static const MemoryRegionOps landisk_bios_ops = {
    .read = landisk_bios_read,
    .write = landisk_bios_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void landisk_bios_init(MemoryRegion *sysmem, hwaddr base,
                              BlockBackend *blk)
{
    LandiskBIOS *s = g_new0(LandiskBIOS, 1);

    s->blk = blk;
    memory_region_init_io(&s->iomem, NULL, &landisk_bios_ops, s,
                          "landisk-bios", 0x18);
    memory_region_add_subregion(sysmem, base, &s->iomem);
}

typedef struct ResetData {
    SuperHCPU *cpu;
    uint32_t vector;
} ResetData;

static void main_cpu_reset(void *opaque)
{
    ResetData *s = (ResetData *)opaque;
    CPUSH4State *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));
    env->pc = s->vector;
}

static struct QEMU_PACKED
{
    int mount_root_rdonly;
    int ramdisk_flags;
    int orig_root_dev;
    int loader_type;
    int initrd_start;
    int initrd_size;

    char pad[232];

    char kernel_cmdline[256] QEMU_NONSTRING;
} boot_params;

static void landisk_init(MachineState *machine)
{
    const char *kernel_filename = machine->kernel_filename;
    const char *kernel_cmdline = machine->kernel_cmdline;
    const char *initrd_filename = machine->initrd_filename;
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    SuperHCPU *cpu;
    CPUSH4State *env;
    ResetData *reset_info;
    struct SH7750State *s;
    MemoryRegion *sdram = g_new(MemoryRegion, 1);
    MemoryRegion *rom = g_new(MemoryRegion, 1);
    DriveInfo *dinfo;
    DeviceState *dev;
    SysBusDevice *busdev;
    MemoryRegion *address_space_mem = get_system_memory();
    PCIBus *pci_bus;
    PCIDevice *pci_dev;
    LandiskCPLD *cpld;

    cpu = SUPERH_CPU(cpu_create(machine->cpu_type));
    env = &cpu->env;

    reset_info = g_new0(ResetData, 1);
    reset_info->cpu = cpu;
    reset_info->vector = env->pc;
    qemu_register_reset(main_cpu_reset, reset_info);

    memory_region_init_ram(sdram, NULL, "landisk.sdram", SDRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(address_space_mem, SDRAM_BASE, sdram);

    s = sh7750_init(cpu, address_space_mem);
    cpld = landisk_cpld_init(address_space_mem, CPLD_BASE, sh7750_irl(s));

    dev = qdev_new("sh_pci");
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(busdev, &error_fatal);
    pci_bus = PCI_BUS(qdev_get_child_bus(dev, "pci"));
    sysbus_mmio_map(busdev, 0, P4ADDR(0x1e200000));
    sysbus_mmio_map(busdev, 1, A7ADDR(0x1e200000));
    sysbus_connect_irq(busdev, 0, &cpld->irq[PCI_INTA]);
    sysbus_connect_irq(busdev, 1, &cpld->irq[PCI_INTB]);
    sysbus_connect_irq(busdev, 2, &cpld->irq[PCI_INTC]);
    sysbus_connect_irq(busdev, 3, &cpld->irq[PCI_INTD]);

    /* Internal hard disk */
    pci_dev = pci_create_simple(pci_bus, PCI_DEVFN(SLOT_IDE, 0), "atp865-ide");
    pci_ide_create_devs(pci_dev);

    /*
     * CompactFlash slot of the USL-5P style boards (True IDE mode in area 5,
     * registers 2 bytes apart): only present when a drive is given for it
     * with -drive if=ide,index=4.
     */
    dinfo = drive_get(IF_IDE, 2, 0);
    if (dinfo) {
        dev = qdev_new("mmio-ide");
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_connect_irq(busdev, 0, &cpld->irq[FATA]);
        qdev_prop_set_uint32(dev, "shift", 1);
        sysbus_realize_and_unref(busdev, &error_fatal);
        sysbus_mmio_map(busdev, 0, CF_BASE + 0x40);
        sysbus_mmio_map(busdev, 1, CF_BASE + 0x2c);
        mmio_ide_init_drives(dev, dinfo, NULL);
    }

    /* Boot ROM and the firmware services for it */
    memory_region_init_rom(rom, NULL, "landisk.rom", ROM_SIZE, &error_fatal);
    memory_region_add_subregion(address_space_mem, ROM_BASE, rom);
    dinfo = drive_get(IF_IDE, 0, 0);
    landisk_bios_init(address_space_mem, BIOS_BASE,
                      dinfo ? blk_by_legacy_dinfo(dinfo) : NULL);

    if (machine->firmware) {
        if (load_image_targphys(machine->firmware, ROM_BASE, ROM_SIZE,
                                NULL) < 0) {
            error_report("qemu: could not load boot ROM '%s'",
                         machine->firmware);
            exit(1);
        }
    } else if (!kernel_filename) {
        error_report("qemu: the landisk machine needs -kernel or -bios");
        exit(1);
    }

    /* RTL8139C+ */
    pci_init_nic_in_slot(pci_bus, mc->default_nic, NULL, SLOT_NIC);
    pci_init_nic_devices(pci_bus, mc->default_nic);

    memset(&boot_params, 0, sizeof(boot_params));

    if (kernel_filename) {
        int kernel_size;

        kernel_size = load_image_targphys(kernel_filename,
                                        SDRAM_BASE + LINUX_LOAD_OFFSET,
                                        INITRD_LOAD_OFFSET - LINUX_LOAD_OFFSET,
                                        NULL);
        if (kernel_size < 0) {
            error_report("qemu: could not load kernel '%s'", kernel_filename);
            exit(1);
        }

        /* initialization which should be done by firmware */
        address_space_stl(&address_space_memory, SH7750_BCR1, 1 << 3,
                          MEMTXATTRS_UNSPECIFIED, NULL); /* cs3 SDRAM */
        address_space_stw(&address_space_memory, SH7750_BCR2, 3 << (3 * 2),
                          MEMTXATTRS_UNSPECIFIED, NULL); /* cs3 32bit */
        /* The boot loader runs, and enters the kernel, in the P1 area */
        reset_info->vector = (SDRAM_BASE + LINUX_LOAD_OFFSET) | 0x80000000;
    }

    if (initrd_filename) {
        int initrd_size;

        initrd_size = load_image_targphys(initrd_filename,
                                          SDRAM_BASE + INITRD_LOAD_OFFSET,
                                          SDRAM_SIZE - INITRD_LOAD_OFFSET,
                                          NULL);

        if (initrd_size < 0) {
            error_report("qemu: could not load initrd '%s'", initrd_filename);
            exit(1);
        }

        boot_params.loader_type = tswap32(1);
        boot_params.initrd_start = tswap32(INITRD_LOAD_OFFSET);
        boot_params.initrd_size = tswap32(initrd_size);
    }

    if (kernel_cmdline) {
        strncpy(boot_params.kernel_cmdline, kernel_cmdline,
                sizeof(boot_params.kernel_cmdline));
    }

    rom_add_blob_fixed("boot_params", &boot_params, sizeof(boot_params),
                       SDRAM_BASE + BOOT_PARAMS_OFFSET);
}

static void landisk_machine_init(MachineClass *mc)
{
    mc->desc = "I-O DATA LANDISK (HDL-U, Plextor PX-EH)";
    mc->init = landisk_init;
    mc->block_default_type = IF_IDE;
    mc->default_cpu_type = TYPE_SH7751R_CPU;
    mc->default_nic = "rtl8139";
}

DEFINE_MACHINE("landisk", landisk_machine_init)
