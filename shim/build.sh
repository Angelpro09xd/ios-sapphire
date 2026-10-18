#!/bin/bash
# Build the shim and wrap it in a boot.img v4: the topaz (Xiaomi) ABL ignores
# v1/v2 images. The DTB and the vendor ramdisk come from vendor_boot of the
# active slot; loader.c finds our payload behind the vendor ramdisk.
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$((16#$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print $1}')))
truncate -s "$size" out/Image
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${1:-}" ]; then
	# topaz ABL drops the v4 boot ramdisk: append the payload to the Image at
	# the first 4K boundary after _end (loader.c) and grow image_size.
	truncate -s $(( (size + 0xfff) & ~0xfff )) out/Image
	cat "$1" >> out/Image
	python3 -c 'import os,struct,sys; f=open(sys.argv[1],"r+b"); f.seek(16); f.write(struct.pack("<Q", os.path.getsize(sys.argv[1])))' out/Image
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
${MKBOOTIMG:-python3 /home/makr/work/topaz/mkbootimg_src/mkbootimg.py} --header_version 4 --kernel out/Image --ramdisk out/ramdisk.bin \
	--os_version 13.0.0 --os_patch_level 2025-01 \
	--cmdline "console=null" -o out/shim-boot.img
# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/shim-boot.img
