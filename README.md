
# Boot Dreamcast Linux
```
 /media/flo/nvme0-ssd/qemu/build/qemu-system-sh4 \
  -M dreamcast -m 16 \
  -serial null -serial stdio -display none \
  -kernel /home/flo/devel/t2-hacking/dreamcast/dreamcast-linux/.dreamcast/src/linux-7.1.3/vmlinux \
  -drive if=none,file=/home/flo/devel/t2-hacking/dreamcast/dreamcast-linux/build/linux-7.1.3-with-userland-musl.iso,format=raw,readonly=on
```
