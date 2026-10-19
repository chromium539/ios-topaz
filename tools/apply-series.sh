#!/bin/bash
# Reproduce darwin-xnu-build's patches() step locally (dxb skips a patch that
# fails `git apply --check` with only a warning, so check every one here).
#   tools/apply-series.sh [xnu-src] [dxb] [out-tree]
# Defaults: research/xnu (xnu-8796.141.3), research/dxb (pinned DXB_SHA),
# research/xnu-work (recreated). Exits non-zero if one of our numbered
# patches is skipped.
set -euo pipefail
top=$(cd "$(dirname "$0")/.." && pwd)
src=${1:-$top/research/xnu}
dxb=${2:-$top/research/dxb}
work=${3:-$top/research/xnu-work}

rm -rf "$work"
cp -a "$src" "$work"
rm -rf "$work/.git"
git -C "$work" init -q
git -C "$work" add -A
git -C "$work" -c user.name=x -c user.email=x commit -qm base

cd "$work"
# dxb's sed edits (BSD `sed -i ''` there, GNU here)
sed -i 's|^AVAILABILITY_PL="${SDKROOT}/${DRIVERKITROOT}|AVAILABILITY_PL="${FAKEROOT_DIR}|g' bsd/sys/make_symbol_aliasing.sh
sed -i 's|^#include.*BSD.xcconfig.*||g' libsyscall/Libsyscall.xcconfig
sed -i 's|^LDFLAGS_KERNEL_SDK	= -L$(SDKROOT).*|LDFLAGS_KERNEL_SDK	= -L$(FAKEROOT_DIR)/usr/local/lib/kernel -lfirehose_kernel|g' makedefs/MakeInc.def
sed -i 's|^\(INCFLAGS_SDK[[:space:]]*=\) -I$(SDKROOT)/usr/local/include/\(kernel\)\{0,1\}|\1 -I$(FAKEROOT_DIR)/usr/local/include/kernel|' makedefs/MakeInc.def
sed -i 's|export MIG := $(shell $(XCRUN) -sdk $(SDKROOT) -find mig)|export MIG := $(shell find $(FAKEROOT_DIR) -name "mig")|g' makedefs/MakeInc.cmd
sed -i 's|export MIGCOM := $(shell $(XCRUN) -sdk $(SDKROOT) -find migcom)|export MIGCOM := $(shell find $(FAKEROOT_DIR) -name "migcom")|g' makedefs/MakeInc.cmd

# CI copies ours next to blacktop's top-level patches (13.5 uses patches/)
pd=$(mktemp -d)
trap 'rm -rf "$pd"' EXIT
cp "$dxb"/patches/*.patch "$pd"/
cp "$top"/xnu/patches/*.patch "$top"/xnu/patches/*.sh "$pd"/

export LC_ALL=C MACHINE_CONFIG=${MACHINE_CONFIG:-SWEET} MACOS_VERSION=13.5
# Off macOS the stage scripts only need an SDK path holding TargetConditionals.h.
if ! command -v xcrun >/dev/null; then
	mkdir -p "$pd/sdk/usr/include" "$pd/bin"
	echo '/* stub: local apply check only */' > "$pd/sdk/usr/include/TargetConditionals.h"
	printf '#!/bin/sh\necho %s\n' "$pd/sdk" > "$pd/bin/xcrun"
	chmod +x "$pd/bin/xcrun"
	export PATH="$pd/bin:$PATH"
fi
for s in "$pd"/*.sh; do
	echo "script $(basename "$s")"
	bash "$s"
done
skipped=0
for p in "$pd"/*.patch; do
	if out=$(git apply --check "$p" 2>&1); then
		git apply "$p"
	else
		case $(basename "$p") in
		[0-9]*)	echo "SKIPPED $(basename "$p"): $out"
			skipped=$((skipped + 1)) ;;
		*)	echo "skipped dxb $(basename "$p") (other macOS versions; CI skips it too)" ;;
		esac
	fi
done
git add -A
git -c user.name=x -c user.email=x commit -qm series
echo "applied series into $work, skipped=$skipped"
[ "$skipped" = 0 ]
