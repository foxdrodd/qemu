# QEMU `-M j2` — J2 / J-Core SH-2 machine

A QEMU fork with a custom **`j2`** machine that boots a big-endian, MMU-less
**SH-2 (J-Core)** Linux kernel to a serial shell. Target: `sh4eb-softmmu` →
`qemu-system-sh4eb`. See `CLAUDE.md` for design notes and gotchas.

## Build

```sh
./configure --target-list=sh4eb-softmmu --disable-docs --enable-slirp
cd build && ninja qemu-system-sh4eb
```

## Usage

```sh
# boot the mainline device-tree kernel to a shell on ttyUL0
build/qemu-system-sh4eb -M j2 \
  -kernel vmlinux -dtb j2_mimas_v2.dtb \
  -display none -serial mon:stdio

# with an SD card over SPI (appears as /dev/mmcblk0) — needs the SD-node DTB
build/qemu-system-sh4eb -M j2 \
  -kernel vmlinux -dtb j2_mimas_v2_sd.dtb \
  -drive if=sd,file=card.img,format=raw \
  -display none -serial mon:stdio
```

Notes:
- Console is UARTLite **`ttyUL0` = the 1st `-serial`**. Quit with `Ctrl-A X`.
- RAM is fixed at **128 MB** (compiled into the kernel) — don't pass `-m`.
- Build a DTB from the kernel DTS with
  `dtc -I dts -O dtb -o j2.dtb arch/sh/boot/dts/j2_mimas_v2.dts`.
- Ready-to-run `vmlinux` + DTBs + `run.sh` live in
  `/home/flo/devel/t2-hacking/fpga/spartan6/j2-qemu/`.

Local prototype for running the FPGA kernel.
