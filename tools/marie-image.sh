#!/bin/bash
# Build a flashable marie kernel-partition image from an xnu kernel.
#   [CPUS=n] [MARIE_DISPLAY=1] tools/marie-image.sh KERNEL NAME [--ramdisk IMAGE] [--qemu] [--args "extra boot-args"]
# -> shim/out/marie-NAME.img (fastboot flash kernel ...); with --qemu also
#    boots the payload in QEMU virt,gic-version=2 into research/q-NAME/.
set -euo pipefail
top=$(cd "$(dirname "$0")/.." && pwd)
kernel=$1 name=$2
shift 2
ramdisk= qemu= extra=
while [ $# -gt 0 ]; do
	case $1 in
	--ramdisk) ramdisk=$2; shift 2 ;;
	--qemu) qemu=1; shift ;;
	--args) extra=$2; shift 2 ;;
	*) echo "unknown option $1" >&2; exit 1 ;;
	esac
done
# Topaz drivers that touch Qualcomm hardware stay off on the Kirin 710: no
# fuel gauge (Qualcomm I2C), no cpufreq (Qualcomm LUT), no keys (SPMI); xnu
# 0207 compiles them out anyway. sweettouch is marie's Novatek reader since
# 0208 (kernels before 0207 need sweettouch=0: theirs is Qualcomm's).
args="debug=0xc serial=3 cpus=${CPUS:-1} keepsyms=1 -noprogress sweetbattery=0 sweetcpufreq=0 sweetkeys=0 wdt=-1 wdt_test=no_user_checkin"
rdarg=()
if [ -n "$ramdisk" ]; then
	args="$args rd=md0"
	rdarg=(--ramdisk "$ramdisk")
fi
args="$args $extra"
out=$top/shim/out
mkdir -p "$out"
# MARIE_DISPLAY=1: ADT display node (AppleCLCD, IOMobileFramebuffer); needs xnu 0206+
python3 -I "$top/tools/mkadt.py" "$out/marie-$name.adt" --marie ${MARIE_DISPLAY:+--marie-display}
python3 -I "$top/tools/mkpayload.py" "$kernel" "$out/marie-$name.adt" "$out/payload-$name.bin" \
	"${rdarg[@]}" --cmdline "$args"
"$top/shim/build.sh" "$out/payload-$name.bin" >/dev/null
cp "$out/shim-kernel.img" "$out/marie-$name.img"
ls -l "$out/marie-$name.img"
echo "boot-args: $args"
if [ -n "$qemu" ]; then
	python3 -I "$top/tools/mkadt.py" "$out/marie-$name-qemu.adt" --marie --qemu
	python3 -I "$top/tools/mkpayload.py" "$kernel" "$out/marie-$name-qemu.adt" "$out/payload-$name-qemu.bin" \
		"${rdarg[@]}" --cmdline "$args -enable_kprintf_spam"
	python3 -I "$top/tools/qemu-sweet.py" "$out/payload-$name-qemu.bin" "$top/research/q-$name" \
		--gic 2 --secs "${QEMU_SECS:-150}" --kernel "$kernel" --fail-on-panic || echo "qemu: exit $?"
fi
