/*
 * J2 / J-Core open-source SH-2 SoC ("j2" machine, Mimas v2 board layout)
 *
 * Boots a big-endian, MMU-less SH-2 (mach-jcore) Linux kernel. The J-Core SoC
 * peripherals are inlined here (fast-prototype style, like hw/sh4/dreamcast.c's
 * Holly/GD-ROM) rather than split into QOM devices:
 *
 *   0x10000000  SDRAM (64 MB)
 *   0xabcd0100  Xilinx UARTLite  ttyUL0 (console), vector 18
 *   0xabcd0300  Xilinx UARTLite  ttyUL1,           vector 23
 *   0xabcd0400  Xilinx UARTLite  ttyUL2,           vector 19
 *   0xabcd0200  AIC + PIT shared per-CPU register block:
 *                 +0x00 PIT enable (bit26 enable, hwirq in [19:12], prio [23:20])
 *                 +0x08 AIC INTPRI: 8x4-bit priorities for vectors 17..24
 *                 +0x10 PIT throttle/reload (clockevent delta, in bus cycles)
 *                 +0x14 free-running clocksource counter
 *                 +0x18 bus period in ns (read-only)
 *                 +0x20/24/28 RTC seconds-hi/seconds-lo/nanoseconds (read-only)
 *
 * Two kernels are supported off the same register layout:
 *   - the pre-device-tree 4.3.0 board-file kernel (no -dtb): load the ELF and
 *     jump to its entry point; its timer runs periodic off a single enable write.
 *   - the mainline device-tree kernel ("jcore,j2-soc", pass its .dtb via -dtb):
 *     the DTB is loaded into RAM and its address handed to the kernel in r4; the
 *     jcore-pit clockevent driver programs the reload via +0x10.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "target/sh4/cpu.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/xilinx_uartlite.h"
#include "hw/ssi/ssi.h"
#include "hw/sd/sd.h"
#include "hw/misc/unimp.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "system/reset.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "system/device_tree.h"
#include "elf.h"

#define SDRAM_BASE      0x10000000
#define SOC_BASE        0xabcd0000

#define UART0_ADDR      (SOC_BASE + 0x100)
#define UART1_ADDR      (SOC_BASE + 0x300)
#define UART2_ADDR      (SOC_BASE + 0x400)
#define UART0_VEC       18
#define UART1_VEC       23
#define UART2_VEC       19

#define AICPIT_ADDR     (SOC_BASE + 0x200)
#define AICPIT_SIZE     0x100
#define REG_PIT_CTRL    0x00    /* PIT enable (bit26), hwirq [19:12], prio [23:20] */
#define REG_INTPRI      0x08    /* AIC priorities for vectors 17..24 */
#define REG_PIT_THROT   0x10    /* clockevent reload / delta, in bus cycles */
#define REG_PIT_COUNT   0x14    /* free-running clocksource counter */
#define REG_PIT_BUSPD   0x18    /* bus period in ns (read-only) */
#define REG_PIT_SECHI   0x20    /* RTC seconds, high word (read-only) */
#define REG_PIT_SECLO   0x24    /* RTC seconds, low word (read-only) */
#define REG_PIT_NSEC    0x28    /* RTC nanoseconds in current second (read-only) */

#define PIT_ENABLE_BIT  (1u << 26)

/*
 * jcore SPI master (spi@40, "jcore,spi2"), with an SD card wired to CS0 in the
 * SPI-mmc slot (mmc_spi). Per-byte protocol used by drivers/spi/spi-jcore.c:
 * write the TX byte to DATA, write CTRL = cs-bits | speed | XMIT to start the
 * transfer, poll CTRL.BUSY (we transfer instantly, so it never reads busy),
 * then read the RX byte from DATA.
 */
#define SPI_ADDR        (SOC_BASE + 0x040)
#define SPI_SIZE        0x8
#define REG_SPI_CTRL    0x00
#define REG_SPI_DATA    0x04
#define SPI_CTRL_XMIT   0x02    /* start a byte transfer */
#define SPI_CTRL_BUSY   0x02    /* busy status (instant transfer => never set) */
#define SPI_CS0_BIT     0x01    /* 1 << (2*0): chip-select bit for CS0 (the SD) */

/* Number of interrupt vectors modelled (J-Core hwirqs reach 127). */
#define J2_NUM_VEC      128

/*
 * Approximate clocking. The clocksource counter and RTC advance at BUS_HZ; the
 * bus period reported to the jcore-pit clockevent driver is its reciprocal.
 * When the kernel programs a reload (REG_PIT_THROT), the tick period is
 * throt * (1/BUS_HZ); otherwise (the board-file kernel, which never writes it)
 * a fixed rate is used. Exact values only affect perceived time, not boot.
 */
#define BUS_HZ          50000000
#define PIT_TICK_HZ     100
#define BUS_PERIOD_NS   (NANOSECONDS_PER_SECOND / BUS_HZ)   /* 20 ns @ 50 MHz */

typedef struct J2Soc {
    SuperHCPU *cpu;
    MemoryRegion iomem;
    qemu_irq *irqs;             /* index == interrupt vector */
    QEMUTimer *pit_timer;

    uint32_t intpri;            /* REG_INTPRI latch */
    uint32_t pit_ctrl;          /* REG_PIT_CTRL latch */
    uint32_t pit_throt;         /* REG_PIT_THROT reload, in bus cycles */
    int pit_vec;                /* hwirq the PIT raises (from control write) */
    int pit_prio;              /* priority the PIT is programmed with */
    bool pit_enabled;

    /*
     * The AIC is edge-triggered (the kernel uses handle_simple_irq /
     * handle_edge_irq). We latch a rising edge into 'pending' and clear it when
     * the CPU takes the interrupt; 'line' tracks the raw device line so we only
     * latch 0->1 transitions (the Xilinx UARTLite holds its line high, which
     * would otherwise storm a level controller).
     */
    uint8_t line[J2_NUM_VEC];
    uint8_t pending[J2_NUM_VEC];

    /* jcore SPI controller (spi@40) + SD card over SPI. */
    MemoryRegion spi_iomem;
    SSIBus *spi_bus;
    qemu_irq spi_cs;            /* ssi-sd chip-select input (active low) */
    uint8_t spi_data;           /* DATA_REG: last byte written (TX) / read (RX) */
    uint32_t spi_ctrl;          /* CTRL_REG latch (CS + speed bits) */
} J2Soc;

/* Priority (0 = disabled) the AIC assigns to a given vector. */
static int j2_aic_prio(J2Soc *s, int vec)
{
    if (s->pit_enabled && vec == s->pit_vec) {
        /* The PIT carries its own priority in its enable word. */
        return s->pit_prio ? s->pit_prio : 15;
    }
    if (vec >= 17 && vec <= 24) {
        return (s->intpri >> ((vec - 17) * 4)) & 0xf;
    }
    return 0;
}

/* Recompute the highest-priority pending+enabled vector and poke the CPU. */
static void j2_aic_update(J2Soc *s)
{
    CPUSH4State *env = &s->cpu->env;
    int best_vec = -1;
    int best_prio = 0;
    int vec;

    for (vec = 0; vec < J2_NUM_VEC; vec++) {
        if (s->pending[vec]) {
            int prio = j2_aic_prio(s, vec);
            if (prio > best_prio) {
                best_prio = prio;
                best_vec = vec;
            }
        }
    }

    if (best_vec >= 0) {
        env->irq_vector = best_vec;
        env->irq_level = best_prio;
        cpu_interrupt(CPU(s->cpu), CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(CPU(s->cpu), CPU_INTERRUPT_HARD);
    }
}

/*
 * Device IRQ line (n == vector) changed state. The AIC is edge-triggered, but
 * QEMU's xilinx_uartlite asserts its line on every interrupting event (each
 * TX-empty / RX-valid) and leaves it high (its STATUS_IE is sticky), calling
 * this handler once per event. So we (re)latch a pending on *any* asserting
 * call rather than only on a 0->1 line transition: an ack clears pending, and
 * the next event re-latches it. This is driven by discrete device events (not a
 * held level), so it delivers every TX-empty re-assertion without the interrupt
 * storm a truly level-triggered controller would suffer on the stuck-high line.
 */
static void j2_aic_set_irq(void *opaque, int n, int level)
{
    J2Soc *s = opaque;

    if (n < 0 || n >= J2_NUM_VEC) {
        return;
    }
    if (level) {
        s->pending[n] = 1;
    }
    s->line[n] = level ? 1 : 0;
    j2_aic_update(s);
}

/* EOI hook called by the CPU right after it takes an interrupt. */
static void j2_aic_ack(void *opaque, int vec)
{
    J2Soc *s = opaque;

    if (vec >= 0 && vec < J2_NUM_VEC) {
        s->pending[vec] = 0;
    }
    j2_aic_update(s);
}

/* Nanoseconds until the next PIT edge, from the programmed reload if any. */
static int64_t j2_pit_period_ns(J2Soc *s)
{
    if (s->pit_throt) {
        return (int64_t)s->pit_throt * BUS_PERIOD_NS;
    }
    return NANOSECONDS_PER_SECOND / PIT_TICK_HZ;
}

static void j2_pit_tick(void *opaque)
{
    J2Soc *s = opaque;

    if (!s->pit_enabled) {
        return;
    }
    /* Periodic edge; the timer ISR performs no MMIO ack. */
    s->pending[s->pit_vec] = 1;
    j2_aic_update(s);
    timer_mod(s->pit_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + j2_pit_period_ns(s));
}

static uint64_t j2_aicpit_read(void *opaque, hwaddr offset, unsigned size)
{
    J2Soc *s = opaque;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    switch (offset) {
    case REG_PIT_CTRL:
        return s->pit_ctrl;
    case REG_INTPRI:
        return s->intpri;
    case REG_PIT_THROT:
        return s->pit_throt;
    case REG_PIT_COUNT:
        /* Free-running up-counter at BUS_HZ. */
        return muldiv64(now, BUS_HZ, NANOSECONDS_PER_SECOND);
    case REG_PIT_BUSPD:
        /* Bus period in ns; the clockevent driver derives its freq from this. */
        return BUS_PERIOD_NS;
    case REG_PIT_SECHI:
        return (uint32_t)((now / NANOSECONDS_PER_SECOND) >> 32);
    case REG_PIT_SECLO:
        return (uint32_t)(now / NANOSECONDS_PER_SECOND);
    case REG_PIT_NSEC:
        return (uint32_t)(now % NANOSECONDS_PER_SECOND);
    default:
        return 0;
    }
}

static void j2_aicpit_write(void *opaque, hwaddr offset, uint64_t val,
                            unsigned size)
{
    J2Soc *s = opaque;

    switch (offset) {
    case REG_PIT_CTRL:
        s->pit_ctrl = val;
        s->pit_vec = (val >> 12) & 0xff;
        s->pit_prio = (val >> 20) & 0xf;
        if ((val & PIT_ENABLE_BIT) && s->pit_vec) {
            s->pit_enabled = true;
            timer_mod(s->pit_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      j2_pit_period_ns(s));
        } else {
            s->pit_enabled = false;
            timer_del(s->pit_timer);
        }
        j2_aic_update(s);
        break;
    case REG_PIT_THROT:
        /* Clockevent reload; the running timer picks it up on its next arm. */
        s->pit_throt = val;
        break;
    case REG_INTPRI:
        s->intpri = val;
        j2_aic_update(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps j2_aicpit_ops = {
    .read = j2_aicpit_read,
    .write = j2_aicpit_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static uint64_t j2_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    J2Soc *s = opaque;

    switch (offset) {
    case REG_SPI_CTRL:
        /* Transfers complete instantly, so BUSY never reads set. */
        return s->spi_ctrl & ~SPI_CTRL_BUSY;
    case REG_SPI_DATA:
        return s->spi_data;
    default:
        return 0;
    }
}

static void j2_spi_write(void *opaque, hwaddr offset, uint64_t val,
                         unsigned size)
{
    J2Soc *s = opaque;

    switch (offset) {
    case REG_SPI_CTRL:
        s->spi_ctrl = val;
        /*
         * The jcore CS bits are active-low: the driver initialises cs_reg to
         * JCORE_SPI_CTRL_CS_BITS (all set = all deselected) and clears a card's
         * bit to select it. ssi-sd's chip-select is also active-low, so the CS
         * gpio simply mirrors the bit: set => deselect (1), clear => select (0).
         */
        qemu_set_irq(s->spi_cs, (val & SPI_CS0_BIT) ? 1 : 0);
        if (val & SPI_CTRL_XMIT) {
            s->spi_data = ssi_transfer(s->spi_bus, s->spi_data);
        }
        break;
    case REG_SPI_DATA:
        s->spi_data = val & 0xff;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps j2_spi_ops = {
    .read = j2_spi_read,
    .write = j2_spi_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* SPI master at 0xabcd0040 with an SD card over SPI wired to CS0. */
static void j2_spi_create(J2Soc *s, MemoryRegion *sysmem)
{
    DriveInfo *dinfo;
    BlockBackend *blk;
    DeviceState *sddev;
    DeviceState *carddev;

    memory_region_init_io(&s->spi_iomem, NULL, &j2_spi_ops, s,
                          "j2-spi", SPI_SIZE);
    memory_region_add_subregion(sysmem, SPI_ADDR, &s->spi_iomem);

    s->spi_bus = ssi_create_bus(DEVICE(s->cpu), "j2-spi-bus");
    sddev = ssi_create_peripheral(s->spi_bus, "ssi-sd");

    dinfo = drive_get(IF_SD, 0, 0);
    blk = dinfo ? blk_by_legacy_dinfo(dinfo) : NULL;
    carddev = qdev_new(TYPE_SD_CARD_SPI);
    qdev_prop_set_drive_err(carddev, "drive", blk, &error_fatal);
    qdev_realize_and_unref(carddev, qdev_get_child_bus(sddev, "sd-bus"),
                           &error_fatal);

    s->spi_cs = qdev_get_gpio_in_named(sddev, SSI_GPIO_CS, 0);
    /* Deselected at reset (active low => high). */
    qemu_set_irq(s->spi_cs, 1);
}

static void j2_uart_create(J2Soc *s, hwaddr addr, int vec, Chardev *chr)
{
    DeviceState *dev = qdev_new(TYPE_XILINX_UARTLITE);

    qdev_prop_set_enum(dev, "endianness", ENDIAN_MODE_BIG);
    qdev_prop_set_chr(dev, "chardev", chr);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, s->irqs[vec]);
}

typedef struct J2ResetData {
    SuperHCPU *cpu;
    uint32_t vector;
    uint32_t dtb_addr;          /* handed to the kernel in r4 (0 if no DTB) */
} J2ResetData;

static void j2_cpu_reset(void *opaque)
{
    J2ResetData *r = opaque;

    cpu_reset(CPU(r->cpu));
    r->cpu->env.pc = r->vector;
    /* mach-jcore head_32.S expects the FDT physical address in r4. */
    r->cpu->env.gregs[4] = r->dtb_addr;
}

static void j2_init(MachineState *machine)
{
    SuperHCPU *cpu;
    J2Soc *s;
    J2ResetData *reset_info;
    MemoryRegion *sysmem = get_system_memory();
    uint64_t entry = 0;

    if (!machine->kernel_filename) {
        error_report("The 'j2' machine requires a -kernel (SH-2/J2 vmlinux)");
        exit(1);
    }

    cpu = SUPERH_CPU(cpu_create(machine->cpu_type));

    s = g_new0(J2Soc, 1);
    s->cpu = cpu;
    s->pit_vec = -1;
    s->irqs = qemu_allocate_irqs(j2_aic_set_irq, s, J2_NUM_VEC);
    s->pit_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, j2_pit_tick, s);

    /* Wire the board interrupt controller into the CPU's SH-2 exception path. */
    cpu->env.irq_ack = j2_aic_ack;
    cpu->env.irq_ack_opaque = s;

    /* SDRAM at 0x10000000 (machine->ram sized via -m, default 64 MB). */
    memory_region_add_subregion(sysmem, SDRAM_BASE, machine->ram);

    /* AIC + PIT shared register block. */
    memory_region_init_io(&s->iomem, NULL, &j2_aicpit_ops, s,
                          "j2-aic-pit", AICPIT_SIZE);
    memory_region_add_subregion(sysmem, AICPIT_ADDR, &s->iomem);

    /* Three Xilinx UARTLite ports (big-endian). */
    j2_uart_create(s, UART0_ADDR, UART0_VEC, serial_hd(0));
    j2_uart_create(s, UART1_ADDR, UART1_VEC, serial_hd(1));
    j2_uart_create(s, UART2_ADDR, UART2_VEC, serial_hd(2));

    /* SPI master + SD-over-SPI card (backed by -drive if=sd). */
    j2_spi_create(s, sysmem);

    /* Other SoC registers the kernel may poke but we don't model. */
    create_unimplemented_device("j2-gpio",  SOC_BASE + 0x000, 0x40);
    create_unimplemented_device("j2-cache", SOC_BASE + 0x0c0, 0x40);
    create_unimplemented_device("j2-cpuid", SOC_BASE + 0x600, 0x40);

    if (load_elf(machine->kernel_filename, NULL, NULL, NULL, &entry,
                 NULL, NULL, NULL, ELFDATA2MSB, EM_SH, 0, 0) < 0) {
        error_report("Could not load kernel '%s'", machine->kernel_filename);
        exit(1);
    }

    reset_info = g_new0(J2ResetData, 1);
    reset_info->cpu = cpu;
    reset_info->vector = entry;

    /*
     * The mainline device-tree kernel needs its FDT; load it above the kernel
     * image (which reserves it via early_init_fdt_reserve_self) and pass the
     * address in r4. The board-file kernel takes no -dtb and ignores r4.
     */
    if (machine->dtb) {
        int dtb_size;
        void *dtb = load_device_tree(machine->dtb, &dtb_size);
        /*
         * 32 MB in: clear of the kernel image *including* any built-in
         * initramfs (the SMP kernel's ~1.7 MB CONFIG_INITRAMFS_SOURCE pushes
         * the ELF end to ~8.2 MB, past the old 8 MB spot), and still inside the
         * DTS 64 MB memory node so early_init_fdt_reserve_self reserves it.
         */
        hwaddr dtb_addr = SDRAM_BASE + 0x02000000;

        if (!dtb) {
            error_report("Could not load device tree '%s'", machine->dtb);
            exit(1);
        }
        rom_add_blob_fixed("dtb", dtb, dtb_size, dtb_addr);
        g_free(dtb);
        reset_info->dtb_addr = dtb_addr;
    }

    qemu_register_reset(j2_cpu_reset, reset_info);
}

static void j2_machine_init(MachineClass *mc)
{
    mc->desc = "J2 J-Core SH-2 SoC (Mimas v2)";
    mc->init = j2_init;
    mc->default_cpu_type = TYPE_J2_CPU;
    /*
     * The target kernel's RAM size is compiled in (CONFIG_MEMORY_SIZE=0x8000000)
     * and is not passed via -m, so default to 128 MB to match; overriding -m
     * will make the kernel's bootmem setup BUG.
     */
    mc->default_ram_id = "j2.sdram";
    mc->default_ram_size = 128 * MiB;
    mc->max_cpus = 1;
}

DEFINE_MACHINE("j2", j2_machine_init)
