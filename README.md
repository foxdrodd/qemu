# Boot Dreamcast Linux with QEMU

```
./build/qemu-system-sh4 \
  -M dreamcast -m 16 \
  -serial null -serial stdio \
  -kernel /home/flo/devel/t2-hacking/dreamcast/dreamcast-linux/.dreamcast/src/linux-7.1.3/vmlinux \
  -drive if=none,file=/home/flo/devel/t2-hacking/dreamcast/dreamcast-linux/build/linux-7.1.3-with-userland-musl.iso,format=raw,readonly=on
```

# NIC Support

## LAN Adapter

```
-nic model=dc-lanadapter
```

## BBA rtl8139too

is the default selection.


# VMU (Visual Memory)

A VMU is attached as a **second `-drive if=none`** (the first is the GD-ROM). It
appears as a memory card in a controller's slot on Maple port 2 and shows up in
Linux as a 128 KB MTD device (`/dev/mtd0`, `vmu-flash` + `vmufat`).

The image must be **exactly 128 KB** and writable (raw, not `readonly`):

```
dd if=/dev/zero of=vmu.bin bs=1024 count=128
```

```
-drive if=none,file=/path/to/game.iso,format=raw,readonly=on   # GD-ROM (first)
-drive if=none,file=vmu.bin,format=raw                          # VMU (second)
```

Inside Linux the flash is at `/dev/mtd0` and can be formatted/mounted with
`vmufat`. Writes persist back to `vmu.bin` on the host.


# Supports

- Booting Linux with serial console
- GDROM
- LAN Adapter Networking
- BBA (RTL8139) Networking
- X Framebuffer
- Maple Keyboard, Mouse
- VMU (Visual Memory) as an MTD / block device
