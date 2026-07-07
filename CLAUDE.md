# CLAUDE.md — QEMU with the Sega Dreamcast machine (`-M dreamcast`)

This is a QEMU checkout with a **custom `dreamcast` machine type** added, to run
**real Linux** (not just games) on an emulated Dreamcast. The value over game
emulators is QEMU's mature SH-4 core with a working MMU/TLB, which the
mach-dreamcast kernel needs.

Only the `sh4-softmmu` target matters here. The run scripts / `Makefile` /
bare-metal test blob live **outside** this tree in the harness project:
`/home/flo/devel/t2-hacking/dcemulator/`.

## The Dreamcast code (what we added/edited)

| File | Role |
|------|------|
| `hw/sh4/dreamcast.c` | Machine (`DEFINE_MACHINE`), memory map, ELF/initrd loader, **inline** Holly ASIC (interrupt controller) + **inline** GD-ROM drive. Inlining Holly/GD-ROM is a prototyping shortcut, not idiomatic QEMU — split into QOM devices when hardening. |
| `hw/display/dreamcast_pvr.c` | PowerVR2 (`dc-pvr`): framebuffer **scanout only** (no 3D/TA — Linux doesn't use it) + 60 Hz VSYNC IRQ. |
| `hw/input/dreamcast_maple.c` | Maple bus (`dc-maple`): keyboard on port 0 + mouse on port 1, both via QEMU `HIDState`. |
| `hw/net/dreamcast_la.c` | Sega LAN Adapter (`dc-lanadapter`), Fujitsu MB86967 10 Mbit NIC. |
| `hw/sh4/sh7750.c` | SH7750/SH7091 SoC. Added `sh7750_set_porta()` + PCTRA/PDTRA read cases. |
| `hw/char/sh_serial.c` | SCIF serial; `can_receive` fix (use `rx_cnt`, not `rx_head`, or pasted input drops). |
| `include/hw/sh4/sh.h` | Prototypes for the `dc_*_init()` board helpers. |

Build wiring: `hw/{sh4,display,input,net}/{meson.build,Kconfig}`. `CONFIG_DREAMCAST`
selects `DC_PVR`, `DC_MAPLE`, `DC_LANADAPTER`, `SH7750`.

## Build

```
cd build && ninja qemu-system-sh4
```
Configured with `../configure --target-list=sh4-softmmu --disable-docs --enable-slirp`
(the `--enable-slirp` pulls bundled libslirp for user networking).

## Run

Prefer the `make run-*` targets in the harness dir. Direct invocation essentials:

- Binary: `build/qemu-system-sh4 -M dreamcast -m 16`
- **Console is SCIF = `ttySC1` = `serial_hd(1)` = the 2nd `-serial`.** Always pass
  `-serial null -serial <sink>`; the first slot is unused. Quit with `Ctrl-A X`.
- Kernel: `…/dreamcast-linux/.dreamcast/src/linux-7.1.3/vmlinux` (SH ELF, load
  `0x8c000000`, entry `0x8c002000`, `console=ttySC1`, no DTB).
- GD-ROM disc: `-drive if=none,file=<iso|cdi>,format=raw,readonly=on` (NOT
  `if=ide` — no HBA). Appears as `/dev/gdrom`. Data track at LBA 11702. Raw
  `.iso` and DiscJuggler `.cdi` auto-detected; omit `-kernel` to boot the disc's
  own `1ST_READ.BIN` (see "CDI images + boot-from-disc").
- Network: `-nic user` gives the default Broadband Adapter (RTL8139);
  `-nic user,model=dc-lanadapter` selects the LAN adapter. Use `-nic` (not a
  bare `-netdev`, which leaves the NIC peerless and drops packets).

## Memory map (phys = P1/P2 addr & 0x1fffffff)

SDRAM `0x0c000000` (16 MB) · VRAM `0x05000000` (8 MB) · AICA RAM `0x00800000` ·
Holly `0x005f6900` · GD-ROM `0x005f7000` · Maple `0x005f6c00` · PVR `0x005f8000` ·
LAN `0x00600400`. Boot params page at `0x8c001000` (`CONFIG_CMDLINE_OVERWRITE`).

## Hard-won gotchas (read before debugging)

- **Holly IRL encoding**: kernel `evt2irq = INTEVT >> 5` maps IRQ13/11/9 to chain
  indices 13/11/9; the IRL value the CPU needs is `index ^ 15` = `{2,4,6}`. Wrong
  values mis-vector the IRQ, raise IMASK and freeze the timer (froze GD-ROM mount).
- **Maple polling is VSYNC-driven**: the kernel queues GETCOND from its VBLANK
  handler (Holly event 5). Without the PVR raising 60 Hz VSYNC, only DEVINFO PnP
  scans happen and no keys/mouse motion ever arrive.
- **PVR pixel depth comes from `DIWMODE` bits 2-3, NOT register `0x108`.** pvr2fb
  overloads `0x108`: it's the pixel-depth *and* the palette-type register, so X's
  colormap setup clobbers it. Reading depth from `0x108` gave bytespp=1 → width
  1280 → each scanline over-read into the next = the "screen duplication" bug.
- **VGA vs TV cable**: `sh7750_set_porta(s, 0x0300, 0x0000)` drives PDTRA bits 8-9
  low = **VGA cable** → progressive 640×480. Otherwise pvr2fb picks interlaced NTSC
  broadcast timing and X fails with `FBIOPUT_VSCREENINFO: Invalid argument`.
- **initrd**: patch the boot-params page via `rom_ptr()` on the ELF blob; a direct
  `address_space_stl` gets clobbered by the ROM-blob reset copy.

## Headless testing (see `dcemulator/scratchpad/*.py`)

`-display none`, `-serial null -serial unix:…` for the console, `-qmp unix:…` for
input injection (`input-send-event` with `rel`/`btn`), `-monitor unix:…` for
`screendump`. Notes: a backgrounded QEMU does **not** survive across separate shell
tool calls (use one standalone script); socket paths must be < 108 bytes (use
`/tmp`); evdev char devices can't be read with `od` (it `lseek`s → EINVAL) — verify
input at the device level with a temporary `fprintf` in the driver instead.

## RTL8139 / Broadband Adapter via GAPS PCI bridge — DONE

Implemented in `hw/pci-host/dreamcast_gaps.c`. Verified: `8139too` binds,
100 Mbit link up, DHCP lease, ping 0% loss (IRQ 99 = event 35). It is the
**default** NIC (bare `-nic user` or none); `-nic ...,model=dc-lanadapter`
selects the older LAN adapter instead. Key implementation notes below; two
things differed from the first plan:
- **Do NOT drop guest BAR1 writes** — that breaks Linux's BAR sizing so
  `resource[1]` never gets `IORESOURCE_MEM` ("region #1 not a MMIO resource").
  Instead forward all config writes (BAR1 sizes normally, maps at `0x01000000`)
  and add a **fixed-decode `MemoryRegion` alias** at `0x01001700` → `0x01000000`
  so the driver's `resource[1]` (pinned by `fixups-dreamcast`) reaches the BAR.
- SEGA id override is a simple post-realize poke of `d->config` (vendor/device).

The Broadband Adapter (HIT-0400) is an **RTL8139C behind SEGA's GAPS PCI bridge**
on the G2 bus. Mainline drives it with the stock `8139too` PCI driver + the
in-tree `arch/sh/drivers/pci/{pci,ops,fixups}-dreamcast.c` gapspci glue (kernel
already has `CONFIG_PCI=y`, `CONFIG_8139TOO=y`). Advantage over the LAN adapter:
fully mainline driver, 100 Mbit. Strategy: **reuse QEMU's `hw/net/rtl8139.c`
PCIDevice unchanged and emulate the GAPS bridge around it.**

New file `hw/pci-host/dreamcast_gaps.c` (`TYPE_DREAMCAST_GAPS`):
- Creates a PCI root bus whose **memory/DMA space is the SH system memory**
  (kernel uses `io_offset=mem_offset=0`, so bus addr == CPU phys → 1:1, no bounce).
- Instantiates stock `rtl8139` at devfn 0; programs **BAR1(mem)=`0x01001700`** at
  realize so the chip regs decode at the fixed addr the driver ioremaps.
- Maps a **32 KB RAM at `0x01840000`** (GAPS DMA SRAM) on system memory.
- Control-reg MMIO at `0x01001400`: ID string `"GAPSPCI_BRIDGE_2"` + `+0x18`
  magic-write (`0x5a14a501`)→ready=1 handshake; window regs stored/ignored.
- Config MMIO bridge at `0x01001600`: forwards to rtl8139 config.
- Routes RTL8139 INTA → **Holly event 35** (`HW_EVENT_EXTERNAL`; note the LAN
  adapter uses 34 — do not copy that).

Three gotchas that will break it:
- **PCI id override**: rtl8139 reports RealTek `0x10ec:0x8139`, but `8139too`
  binds the DC only via `0x11db:0x1234` (SEGA BBA) and `fixups-dreamcast.c`
  switches on `PCI_DEVICE_ID_SEGA_BBA`. Config bridge must return `0x11db/0x1234`
  for offsets 0x00-0x03, forward the rest.
- **Fixed decode**: `gapspci_init` writes `BAR1=0x01000000`; if forwarded it
  remaps the regs off `0x01001700`. Real GAPS ignores BARs for decode → config
  bridge must **shadow BAR writes (0x10-0x27): store for readback, don't forward**.
- **32 KB DMA window**: RTL8139 rx ring must fit in the 32 KB SRAM — verify the
  driver's `RX_BUF_LEN` choice early.

Address map: control `0x01001400` (0x100) · config `0x01001600` (0x100) · regs
`0x01001700` (0x200, = config+0x100) · DMA SRAM `0x01840000` (32 KB).
Wiring: `hw/pci-host/{meson.build,Kconfig}` (`CONFIG_DREAMCAST_GAPS` select PCI +
rtl8139), `DREAMCAST` selects it, `dc_gaps_init()` in `dreamcast.c`. NIC choice
LAN-vs-BBA via a machine property or `-nic model=`. Reference host bridges:
`hw/pci-host/{dino,grackle}.c`.

## VMU (Visual Memory Unit) — DONE

`hw/block/dreamcast_vmu.c` (`DCVmu`, backed by a `BlockBackend`) + controller/
sub-unit dispatch in `hw/input/dreamcast_maple.c`. The VMU is a **Maple sub-unit
of a controller** (it plugs into a controller slot): a controller base unit
appears on port 2, advertising a device in slot 1, and the VMU answers at
`(port 2, unit 1)`. Guest sees a 128 KB MTD (`vmu-flash` → `fs/vmufat`).
Attach with a **second `-drive if=none`** (unit 1, exactly 128 KB); the first is
the GD-ROM. Verified: detected as controller (2,0) + `Visual Memory` (2,1),
`mtd0` 128 KB, BREAD/BWRITE/BSYNC, writes persist to the host image.

Gotchas:
- **Sub-unit addressing**: `to = (port<<6) | (unit>0 ? 1<<(unit-1) : 0x20)`, so
  VMU slot 1 = address bit `0x01`. The controller advertises it via the
  **sub-device mask in DEVINFO response byte 2** (`recvbuf[2] & 0x1F`).
- A **controller must host it** — `CONFIG_JOYSTICK_MAPLE=y`, so the base unit
  must also answer GETCOND with a neutral condition or the port (and its VMU)
  gets detached.
- **GETMINFO is little-endian 16-bit shorts** over the whole frame: `res[6]`
  (byte 12) = root block, `res[12]` (byte 24) = user blocks; `numblocks=root+1`.
- BREAD/BWRITE data words are **big-endian** (function, then addr =
  `partition<<24 | phase<<16 | block`); block payload is raw bytes. Read data
  goes at **response byte 12**; a block-read response is ~524 B (bump the maple
  `resp[]` buffer). Writes are 4×128-B phases → each phase written directly to
  its slice; BSYNC flushes.
- Must `blk_set_perm(blk, CONSISTENT_READ|WRITE, ALL)` on the backend or QEMU
  asserts on the first write.

### VMU LCD (opt-in second display) — DONE

The VMU's 48×32 1bpp LCD is a **second QEMU graphic console** owned by the
`DCVmu` device, opt-in via the `vmu-lcd=on` machine option (a bool on the
`DreamcastMachineState` subclass, wired through `DEFINE_MACHINE_EXTENDED`).
When enabled, DEVINFO advertises `function = FUNC_MEMCARD|FUNC_LCD` (`0x06`) and
the extra LCD function block, so mainline-style `vmu-flash` calls
`vmu_lcd_register()` → `/dev/vmu_lcd0`. The kernel draws a Tux splash on attach.
Verified end-to-end: `function 0x6` detected, `vmu_lcd0` registered, and a
`screendump -d vmu` of the second console shows the 288×192 (48·6 × 32·6) green
Tux+"Linux" splash.

Gotchas:
- LCD updates arrive as **BWRITE tagged with `FUNC_LCD` (`0x04`)** in the
  function word — dispatch on that in `dc_vmu_maple` before the storage path; the
  192-byte framebuffer is at `data[8..]`, MSB-first, top-left origin.
- `GraphicHwOps.gfx_update` must return **`bool`**, not void.
- The console is targetable by `screendump -d vmu` because `dc_vmu_new` sets
  `dev->id = "vmu"` (QMP screendump resolves `device` via `qdev_find_recursive`,
  i.e. by **id**, not QOM path). `query-consoles` does not exist in this build.
- The kernel draws the splash from a **workqueue** (never from maple probe
  context), so it appears a beat after `vmu_lcd0` registers.

## CDI images + boot-from-disc — DONE

All in `hw/sh4/dreamcast.c`. Two capabilities:

1. **`.cdi` disc images** (DiscJuggler, `cdi4dc` output) alongside raw `.iso`.
   `gdrom_probe_disc()` fills the data-track geometry `{data_off, raw_size,
   sec_hdr}`; `gdrom_do_dma`/`gdrom_read_logical` read
   `data_off + rel*raw_size + sec_hdr`. Raw ISO = `{0, 2048, 0}` (unchanged);
   CDI Mode2/Form1 = `{PVD-derived, 2336, 8}`.
2. **Boot from disc with no `-kernel`**: `dc_boot_disc()` parses ISO9660, finds
   `1ST_READ.BIN`, `dc_descramble()`s it, stages it at `0x8c010000` (a
   `rom_add_blob_fixed` so it survives reset), sets the reset vector there.

Gotchas:
- **Detection is structural, not by footer.** The block layer rounds
  `blk_getlength` up to 512, so the CDI version dword at real-EOF−8 reads back
  as zero padding — unusable. Instead scan for the ISO9660 PVD (`\x01CD001`) and
  read the VDS terminator (`\xffCD001`) that follows: stride **2048 → flat ISO**,
  stride **2336 → CDI**. `data_off = pvd_pos − 8 − 16*2336`.
- CDI data track is Mode2/Form1 stored as **2336 bytes/sector** (no 16-byte
  sync/header; 8-byte subheader, then 2048 user, then EDC/ECC). User data is at
  **offset 8**. Track starts at LBA **11702** (`genisoimage -C 0,11702`, matches
  `GDROM_DATA_LBA`); ISO extents are session-absolute (11702-based) so
  `rel = lba − data_lba`.
- **Descramble** = exact inverse of sh-boot `scramble.c`: 16-bit LCG
  (`seed=(seed*2109+9273)&0x7fff`, `ret=(seed+0xc000)&0xffff`), seed =
  `filesize & 0xffff` (set **once**), Fisher-Yates over 32-byte slices, windows
  2 MB→32 B. Verified byte-identical to the pre-scramble `kernel-boot.bin`.
- `1ST_READ.BIN` (= sh-boot `kernel-boot` stub + appended `zImage`) is linked
  at and entered from **`0x8c010000`**; it is self-contained (copies zImage to
  `0xac600000`, sets boot params at `0x8c001000`, jumps `0x8c600000`) and reads
  rootfs via the **hardware GD-ROM driver** — so **no BIOS syscall HLE** needed.
- kernel-boot does a **byte** write to **STBCR (`0xffc00004`)**; `sh7750.c`'s
  `sh7750_mem_writeb` used to `abort()` on it — now ignored (also STBCR2
  `0xffc00010`). The `-kernel` path never hit this.

## Status

Working & verified: machine boot, Holly IRQs, serial console, GD-ROM rootfs mount
(full musl userland), LAN adapter (ping + DHCP), PVR2 scanout (Tux + fbcon @640×480),
Maple keyboard, VGA-cable X (no duplication), Maple **mouse** (kernel detects
`function 0x200` on port 1 as `input1`/`mouse0`; QMP motion reaches the device),
**Broadband Adapter** (RTL8139 via GAPS PCI bridge, the default NIC: DHCP +
ping 0% loss, mainline `8139too`), **VMU** (controller sub-unit, 128 KB MTD via
mainline `vmu-flash`, second `-drive if=none`), **VMU LCD** (opt-in `vmu-lcd=on`
second console: `function 0x6`, `vmu_lcd0`, Tux splash renders), **CDI images +
boot-from-disc** (raw `.iso` and DiscJuggler `.cdi` auto-detected; no-`-kernel`
boot descrambles `1ST_READ.BIN` and runs it — musl & uclibc CDIs boot to shell).
Not done: AICA sound; PVR 3D/TA (not needed); VMU RTC sub-function.
