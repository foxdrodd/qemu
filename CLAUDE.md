# CLAUDE.md — QEMU with the J2 / J-Core SH-2 machine (`-M j2`)

This is a QEMU checkout with a **custom `j2` machine type** added, to boot a
**big-endian, MMU-less SH-2** (J-Core open-source CPU, `mach-jcore`) Linux
kernel to a serial shell. J2 is the sibling of the SH-4 Dreamcast work in the
neighbouring checkout (`/media/flo/nvme0-ssd/qemu/`); this tree targets only the
**`sh4eb-softmmu`** target → binary **`qemu-system-sh4eb`** (big-endian SH).

The value: QEMU has no J2 CPU or J-Core SoC upstream. J2 is an SH-2 variant that
adds `cas.l` (atomic compare-and-swap) and **hardware unaligned access**, and has
a simpler SH-2 exception model (no SH-4 banked SSR/SPC/INTEVT). This machine runs
the real FPGA kernel unmodified.

External harness / kernel sources / persisted run artifacts live **outside** this
tree:
- **Boot-ready artifacts + `run.sh`**: `/home/flo/devel/t2-hacking/fpga/spartan6/j2-qemu/`
  (`vmlinux`, `j2_mimas_v2.dtb` (no-SD), `j2_mimas_v2_sd.dtb` (with SD node),
  `rootfs/`, `devnodes.txt`).
- **Kernel source (7.2, mainline DT)**: `.../fpga/spartan6/linux-upstream/`
  (**do NOT edit** — the user builds this for the FPGA).
- **Toolchain**: `/media/flo/nvme0-ssd/musl-cross-make/output/bin` on `PATH`;
  build the kernel with `ARCH=sh CROSS_COMPILE=sh2eb-linux-muslfdpic- make`.

## The J2 code (what we added/edited)

| File | Role |
|------|------|
| `hw/sh4/j2.c` | Machine (`DEFINE_MACHINE("j2", …)`), memory map, ELF+DTB loader, **inline** AIC (interrupt controller) + PIT (timer) + **jcore SPI master with an SD-over-SPI card**. Inlining the SoC blocks is a prototyping shortcut like the Dreamcast machine's inline Holly/GD-ROM — split into QOM devices when hardening. |
| `target/sh4/cpu.{h,c}`, `cpu-qom.h` | `TYPE_J2_CPU "…j2…"`, `SH_FEATURE_J2`, `SH_CPU_J2`; J2 reset SR; new `env` fields `irq_vector`/`irq_level` (set by the AIC) and `irq_ack`/`irq_ack_opaque` (EOI hook); `TB_FLAG_UNALIGN` set for J2 in `superh_get_tb_cpu_state`. |
| `target/sh4/translate.c` | `cas.l` (opcode `0x2003`, `CHECK_J2`-gated); `IS_USER` forced false for J2 (SH-2 SR has no MD bit); system-mode `UNALIGN()` macro now honours `TB_FLAG_UNALIGN`. |
| `target/sh4/helper.c` | J2 branch in `superh_cpu_do_interrupt` (SH-2 exception model); J2 32-bit **identity** physical addressing (no SH-4 `& 0x1fffffff` mask — peripherals live at `0xabcd0000`). |

Build wiring: `hw/sh4/{meson.build,Kconfig}`. `CONFIG_J2` (default y, `depends on
SH4`) selects `XILINX` (UARTLite), `UNIMP`, `DEVICE_TREE` (libfdt), and `SSI` +
`SSI_SD` (SD-over-SPI). Target list includes it via `configs/devices/sh4eb-softmmu`.

## Build

```
cd build && ninja qemu-system-sh4eb
```
Configured with `../configure --target-list=sh4eb-softmmu --disable-docs --enable-slirp`.
`sh4eb-softmmu` is the **big-endian** SH target — endianness is handled by the
`MO_TE*` memops and the device `DEVICE_BIG_ENDIAN` ops; no new target needed.

## Run

Prefer `spartan6/j2-qemu/run.sh` (`./run.sh` = no SD; `./run.sh <img>` = attach an
SD card). Direct invocation essentials:

- Binary: `build/qemu-system-sh4eb -M j2` (RAM is **128 MB by default and must not
  be overridden** — the kernel's RAM size is compiled in, `CONFIG_MEMORY_SIZE`; a
  different `-m` makes bootmem BUG).
- **Console is UARTLite `ttyUL0` = `serial_hd(0)` = the 1st `-serial`.** Quit with
  `Ctrl-A X`. There are three UARTLites (vec 18/23/19); only the first is the console.
- Kernel: `-kernel <vmlinux>` (big-endian SH ELF, load `0x10000000`-ish, entry from
  the ELF). Two kernels boot off the same register layout:
  - **mainline 7.2 device-tree kernel** (`jcore,j2-soc`): pass `-dtb <j2.dtb>`; the
    DTB is loaded into RAM at **`0x12000000`** (32 MB in) and its phys addr handed to
    the kernel in **r4** (`mach-jcore head_32.S`). Boots to an interactive shell.
    **This is the current one.** (The DTB address was `0x10800000` / 8 MB in; a kernel
    with a large `CONFIG_INITRAMFS_SOURCE` built in — e.g. the SMP kernel, ELF end
    ~8.2 MB — overlaps that, so `j2.c` now stages it at 32 MB, still inside the DTS
    64 MB memory node so `early_init_fdt_reserve_self` reserves it. QEMU aborts with
    "Some ROM regions are overlapping" if the DTB lands inside the kernel image.)
  - the pre-DT 4.3.0 board-file kernel: no `-dtb`, uses static platform devices.
- SD card: `-drive if=sd,file=<img>,format=raw` → **`/dev/mmcblk0`** via mainline
  `mmc_spi`. **Requires the SD-node DTB** (`j2_mimas_v2_sd.dtb`, which has
  `spi@40/sdcard@0`). Without a backing `-drive`, mmc_spi retry-spams the console —
  so the no-SD DTB (`j2_mimas_v2.dtb`) is the default when no card is attached.

Build a DTB from the kernel DTS:
`dtc -I dts -O dtb -o j2.dtb .../linux-upstream/arch/sh/boot/dts/j2_mimas_v2.dts`

## Memory map (identity — J2 has no MMU/segmentation)

SDRAM `0x10000000` (128 MB) · DTB blob `0x12000000` (32 MB in) · SoC base `0xabcd0000`:
GPIO `+0x000` (unimpl) · **SPI+SD `+0x040`** · cache `+0x0c0` (unimpl) · UART0/console
`+0x100` (vec 18) · AIC+PIT `+0x200` · UART1 `+0x300` (vec 23) · UART2 `+0x400`
(vec 19) · cpuid `+0x600` (unimpl).

The **AIC+PIT** shared register block at `0xabcd0200` (`j2_aicpit_ops`):
`+0x00` PIT enable (bit26 enable, hwirq[19:12], prio[23:20]) · `+0x08` AIC INTPRI
(8×4-bit priorities for vectors 17..24) · `+0x10` PIT throttle/reload (clockevent
delta, bus cycles) · `+0x14` free-running clocksource counter · `+0x18` bus period
ns (=20) · `+0x20/24/28` RTC sec-hi/sec-lo/nsec.

## Hard-won gotchas (read before debugging)

- **`IS_USER` must be false for J2.** SH-2 SR has no MD bit, so `stc/ldc sr` would
  fault as illegal if the SH-4 user/kernel split were applied. `CHECK_PRIV`/`IS_USER`
  is gated on `SH_FEATURE_J2` in `translate.c`.
- **J2 does unaligned access in hardware** (plain SH-2/4 fault). Two edits made it
  work in system mode: `cpu.c` sets `TB_FLAG_UNALIGN` for J2, and `translate.c`'s
  **system-mode** `UNALIGN()` (was hardcoded `MO_ALIGN`) now honours the flag — there
  were *two* UNALIGN macros; the CONFIG_USER_ONLY one already checked it.
- **`cas.l` operand order**: compare = `Rm` (bits 7:4), store = `Rn` (bits 11:8),
  old value → `Rn`; `T = (old == Rn)`. Verified against the kernel's inline asm
  (`arch/sh/include/asm/cmpxchg-cas.h`). Getting this wrong wedges every spinlock.
- **SH-2 exception model** (`helper.c` J2 branch): on IRQ, push SR then PC to the R15
  stack, set `SR.IMASK = level`, vector via `PC = mem32[VBR + vec*4]`. **Do NOT gate
  on `SR_BL`** (the SH-2 kernel never clears a BL bit → would freeze the timer). `rte`
  pops PC then SR back. The AIC stashes the pending vector in `env->irq_vector` before
  `cpu_interrupt(CPU_INTERRUPT_HARD)`; the CPU calls `env->irq_ack()` to clear pending.
- **AIC must be edge-triggered.** QEMU's `xilinx_uartlite` leaves `STATUS_IE` sticky
  (`/* hax */`) so its IRQ line stays high; a level controller would storm ~8M irq/s,
  and a naive 0→1-edge latch would drop every TX-empty re-assertion (console output
  truncated at ~15 bytes). Fix in `j2_aic_set_irq`: **(re)latch pending on *any*
  asserting call (level=1)**, not only a line edge — driven by discrete device events,
  so no storm and no lost re-assertions.
- **PIT / clockevent**: the mainline kernel's `jcore-pit` programs a reload via `+0x10`
  (period = throt · 20 ns); the old board-file kernel never writes it (fixed 100 Hz).
  PIT priority comes from the enable word's bits 20-23. The delivered hwirq is **72**
  for the new kernel vs **64** for the old.
- **SD/SPI — CS bits are active-LOW** (the killer bug). The jcore driver `probe`s with
  `cs_reg = 0x15` (all cs bits **set** = all *deselected*) and **clears** a card's bit
  to select it (`csbit = 1<<(2*cs)`; SD on CS0 = bit `0x01`). ssi-sd's chip-select is
  also active-low, so the CS gpio mirrors the bit: `(val & 0x01) ? 1 : 0`
  (set→deselect, clear→select). Inverting it makes the card see every transfer with CS
  deasserted → returns 0xFF to everything → `mmc0: error -22 whilst initialising SD
  card` forever. Debug with ssi-sd.c's `#define DEBUG_SSI_SD 1`.

## jcore SPI master + SD-over-SPI — DONE

`hw/sh4/j2.c` `j2_spi_create()` emulates the `jcore,spi2` controller (`spi@40`,
`0xabcd0040`, size 0x8) with a real SD card wired to CS0. Reuses QEMU's stock
`ssi-sd` + `sd-card-spi` on a new SSI bus (`ssi_create_bus`/`ssi_create_peripheral`),
backed by `-drive if=sd`. Register model (per `drivers/spi/spi-jcore.c`): `CTRL_REG`
0x0, `DATA_REG` 0x4; per byte the driver writes TX→DATA, writes `cs_reg|speed|XMIT
(0x02)`→CTRL, polls `BUSY(0x02)`, reads RX←DATA. QEMU transfers instantly
(`ssi_transfer` on the XMIT write), so BUSY never reads set. Verified end-to-end:
`mmc0: new SD card on SPI`, `mmcblk0 … QEMU! 64.0 MiB`, block **read** returns the
real FAT boot sector, block **write** persists to the host image.

Notes:
- Needs the DTS `spi@40/sdcard@0` (`mmc-spi-slot`, `voltage-ranges = <3200 3400>`) —
  use the **SD DTB** *with* a `-drive`. QEMU SD OCR is `0x00FFFF00` (bits 8-23, covers
  3.2–3.4 V) so the host voltage-overlap check passes; ACMD41 HCS → SDHC.
- BusyBox `dd` has **no `conv=notrunc`** — a write test must push a full 512 B block
  (not an emulation bug, just a rootfs-tooling gotcha).

## Known non-QEMU bugs found via this machine (diagnosed, not our bugs)

- **New-kernel `sched_init_domains` hang** = a **GCC 9.4** SH SMP-topology codegen
  bug: `find_next_bit` did a second `__ffs(0)` unconditionally (zero-guard scheduled
  after the bit-scan) → infinite loop. Would hang on the FPGA too. Originally worked
  around with `CONFIG_SMP=n`. **FIXED (verified 2026-07-13): build with GCC 17**
  (`/media/flo/nvme0-ssd/musl-cross-make-gcc17/output/bin/sh2eb-linux-muslfdpic-gcc`,
  17.0.0; the old `.../musl-cross-make/` is 9.4.0). A `CONFIG_SMP=y NR_CPUS=2` kernel
  built with gcc17 now boots clean past `sched_init_domains` to an interactive shell
  (`smp: Brought up 1 node, 1 CPU`; only 1 core onlines because the Mimas DTS has no
  `cpu@1` — `CPU enable method: (null)`). So `CONFIG_SMP=n` is **no longer required**.
- **Old-kernel userspace `sed` hang**: toybox `sed` applies a stale `regmatch`
  offset from a prior matching line to a later non-matching line → `memcpy` with an
  underflowed (negative → ~4 GB) length. A toybox/regex issue, not CPU emulation (all
  bare-metal instruction self-tests pass). Worked around with a sed-free `/init2`.

## Status

Working & verified: big-endian SH-2 boot, identity MMU, SH-2 exception/`rte`,
`cas.l`, unaligned access; **mainline 7.2 device-tree kernel boots to an interactive
serial shell** on `ttyUL0` (`echo`/`ls`/`cat|pipe`/`uname`/`free` all work, 64 MB
seen); AIC (edge-triggered) + PIT clockevent (IRQ 72); 3× UARTLite; DTB handoff in
r4; **SD-over-SPI** (`/dev/mmcblk0`, read+write, host-persistent) via the inline
jcore SPI master + stock `ssi-sd`.

Not done / next ideas: split the inline AIC/PIT/SPI into QOM devices if it ever needs
hardening; wire `-append`/`-initrd` for the old kernel (it currently uses its built-in
cmdline + built-in initramfs). Ethernet: the FPGA has none of interest here.
