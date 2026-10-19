#!/bin/bash
# Build the iOS 16.7.16 (iPhone X, 20H392) HFS+ root for marie from the dmgs
# unpacked into research/ipsw-x/ (OS 087-86698-044, SystemOS 087-86593-047,
# AppOS 087-86715-047, per the D22AP erase identity in BuildManifest.plist).
#   tools/marie-mkroot.sh [SIZE]      (SIZE default 16G)
# -> research/ios16-root.img (+ .simg for fastboot). Uses sudo (apfs-fuse,
# loop mounts, rsync as root); writes nothing outside the new image.
set -euo pipefail
top=$(cd "$(dirname "$0")/.." && pwd)
x=$top/research/ipsw-x
out=$top/research/ios16-root.img
size=${1:-16G}

bash "$top/tools/mkiosroot.sh" "$x/087-86698-044.dmg" "$x/087-86593-047.dmg" "$x/087-86715-047.dmg" "$out" "$size"

mnt=$(mktemp -d)
trap 'mountpoint -q "$mnt" && sudo umount "$mnt"; rmdir "$mnt"' EXIT
fsck.hfsplus -f -y "$out" | tail -2 || true
sudo mount -t hfsplus -o loop,force "$out" "$mnt"
sudo python3 -I "$top/tools/sweet-root-tweaks.py" "$mnt"
df -h "$mnt" | tail -1
sudo umount "$mnt"
fsck.hfsplus -f -n "$out" | tail -2
img2simg "$out" "$out.simg"
ls -l "$out" "$out.simg"
