#!/bin/bash
# Build the shim and wrap it the way Huawei Kirin fastboot wants a kernel:
# Android boot image v1, gzip'd arm64 Image at base+0x80000, no ramdisk
# (same header values as ../marie/clean/kernel_boot.img). Huawei fastboot
# refuses `fastboot boot`, so the result is flashed to the kernel partition.
set -euo pipefail
cd "$(dirname "$0")"
CC="clang --target=aarch64-none-elf"
OBJCOPY=llvm-objcopy
CFLAGS="-O2 -mcpu=cortex-a53 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-jump-tables -Wall -Wextra"
OBJS="start shim console loader fdt emmc touch"
mkdir -p out
for f in $OBJS; do
	src=$f.c
	[ -f $src ] || src=$f.S
	$CC $CFLAGS -c $src -o out/$f.o
done
# The image runs wherever fastboot puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(for f in $OBJS; do [ $f = start ] || llvm-readelf -r out/$f.o; done | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -fuse-ld=lld -T shim.ld -Wl,--build-id=none -Wl,-N \
	$(for f in $OBJS; do echo out/$f.o; done) -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# Pad to image_size so .bss/stacks exist as zeroed bytes.
size=$((16#$(llvm-nm out/shim.elf | awk '$3=="_end"{print $1}')))
truncate -s "$size" out/Image
# Optional xnu payload (tools/mkpayload.py) as $1: append it to the Image at
# the first 4K boundary after _end and grow image_size (loader.c finds it).
if [ -n "${1:-}" ]; then
	truncate -s $(( (size + 0xfff) & ~0xfff )) out/Image
	cat "$1" >> out/Image
	python3 -c 'import os,struct,sys; f=open(sys.argv[1],"r+b"); f.seek(16); f.write(struct.pack("<Q", os.path.getsize(sys.argv[1])))' out/Image
fi
gzip -9 -n -c out/Image > out/Image.gz
mkbootimg --header_version 1 --kernel out/Image.gz \
	--base 0 --kernel_offset 0x80000 --ramdisk_offset 0x07c00000 \
	--tags_offset 0x07a00000 --pagesize 2048 \
	--os_version 9.0.0 --os_patch_level 2019-04 \
	--cmdline "console=null" -o out/shim-kernel.img
# mkbootimg zeroes ramdisk/second addresses without a ramdisk; the stock
# image keeps them (the header id hash does not cover addresses).
python3 -c 'import struct,sys; f=open(sys.argv[1],"r+b"); f.seek(20); f.write(struct.pack("<I",0x07c00000)); f.seek(28); f.write(struct.pack("<I",0x00f00000))' out/shim-kernel.img
# Host render of the console layout.
cc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/shim-kernel.img
