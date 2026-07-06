# Boot Dreamcast Linux with QEMU

```
./build/qemu-system-sh4 \
  -M dreamcast -m 16 \
  -serial null -serial stdio -display none \
  -kernel /home/flo/devel/t2-hacking/dreamcast/dreamcast-linux/.dreamcast/src/linux-7.1.3/vmlinux \
  -drive if=none,file=/home/flo/devel/t2-hacking/dreamcast/dreamcast-linux/build/linux-7.1.3-with-userland-musl.iso,format=raw,readonly=on
```

# Supports

- Booting Linux with serial console
- GDROM
- LAN Adapter Networking
