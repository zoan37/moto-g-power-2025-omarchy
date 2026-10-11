#!/usr/bin/env bash
# Build the vegas boot kernel from source: Google's GKI android13-5.15 kernel,
# build ab13743836 (kernel/common 46a5565a0982), the exact kernel in the stock
# boot.img. Never talks to the phone.
#
# usage: scripts/build-vegas-gki-kernel.sh stock|tier1 [make targets]
#   stock  gki_defconfig + KMI trimming, as Google's build/build.sh does it
#   tier1  stock + DEVTMPFS, DEVTMPFS_MOUNT, FHANDLE, AUTOFS_FS (systemd)
# Default targets: Image modules ("none" = config only). JOBS=4 by default.
# EXTRA_MODULES adds drivers as modules, e.g. Bluetooth's virtual HCI:
#   EXTRA_MODULES=BT_HCIVHCI scripts/build-vegas-gki-kernel.sh tier1 modules_prepare
#   EXTRA_MODULES=BT_HCIVHCI scripts/build-vegas-gki-kernel.sh tier1 M=drivers/bluetooth modules
#
# Expects the layout from notes/kernel-build-20261010.md under
# third_party/kernel/gki-root/disks/build-disk/src/android/common-android13-5.15-2025-05/
# (common/, build/kernel/, prebuilts/...). The script bind-mounts
# third_party/kernel/gki-root on /mnt in a private user+mount namespace so the
# tree sits at Google's original build path (/mnt/disks/build-disk/...): that
# path is embedded in the stock config (CONFIG_UNUSED_KSYMS_WHITELIST, which is
# in the Image via IKCONFIG). No root needed.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
K=$REPO/third_party/kernel
VARIANT=${1:?usage: $0 stock|tier1 [make targets]}
shift
TARGETS=${*:-Image modules}

if [ -z "${IN_GKI_NS:-}" ]; then
	exec unshare --map-root-user --mount env IN_GKI_NS=1 bash -c \
		'mount --bind "$1/gki-root" /mnt && shift && exec bash "$@"' \
		_ "$K" "$REPO/scripts/build-vegas-gki-kernel.sh" "$VARIANT" $TARGETS
fi

ROOT=/mnt/disks/build-disk/src/android/common-android13-5.15-2025-05
SRC=$ROOT/common
case $VARIANT in
stock) OUT=$ROOT/out/android13-5.15/common ;;
tier1) OUT=$ROOT/out/tier1/common ;;
*) echo "unknown variant $VARIANT" >&2; exit 2 ;;
esac

# Environment from build.config.{common,aarch64,gki.aarch64} and
# build/kernel/_setup_env.sh at kernel/build b05fb2e75ba8.
export PATH=$ROOT/prebuilts/clang/host/linux-x86/clang-r450784e/bin:$ROOT/prebuilts/kernel-build-tools/linux-x86/bin:/usr/bin
export ARCH=arm64 BRANCH=android13-5.15 KMI_GENERATION=8 BUILD_NUMBER=13743836
export TZ=UTC LC_ALL=C
export SOURCE_DATE_EPOCH=$(git -C "$SRC" log -1 --pretty=%ct)
export KBUILD_BUILD_TIMESTAMP="$(date -d @"$SOURCE_DATE_EPOCH")"
export KBUILD_BUILD_HOST=build-host KBUILD_BUILD_USER=build-user KBUILD_BUILD_VERSION=1
if [ "$VARIANT" = tier1 ]; then
	# same uname -r (keeps /lib/modules/<release> working), different uname -v
	export KBUILD_BUILD_HOST=vegas-tier1 KBUILD_BUILD_VERSION=2
fi
export KCFLAGS=-D__ANDROID_COMMON_KERNEL__
export KCPPFLAGS="-ffile-prefix-map=$SRC/= -ffile-prefix-map=$ROOT/="
NDK_SYSROOT=$ROOT/prebuilts/ndk-r23/toolchains/llvm/prebuilt/linux-x86_64/sysroot
export USERCFLAGS="--target=aarch64-linux-android31 --sysroot=$NDK_SYSROOT -Wno-unused-function "
export USERLDFLAGS="-fuse-ld=lld --rtlib=compiler-rt --target=aarch64-linux-android31 "
unset LD_LIBRARY_PATH PYTHONPATH PYTHONHOME PYTHONSTARTUP
# Optional: Google's hermetic gzip (toybox), xz and cpio from
# platform/prebuilts/build-tools. They only change the compressed IKCONFIG and
# IKHEADERS blobs; with them the Image is byte-identical to stock.
PBT=$ROOT/prebuilts/build-tools/path/linux-x86
if [ -x "$PBT/gzip" ]; then
	HB=$ROOT/out/hermetic-bin
	mkdir -p "$HB"
	ln -sf "$PBT/gzip" "$PBT/xz" "$PBT/cpio" "$HB/"
	PATH=$HB:$PATH
fi

M=(make -C "$SRC" O="$OUT" LLVM=1 DEPMOD=depmod DTC=dtc)

# git describe needs tags the shallow clone lacks; pin what Google's build printed
printf '%s\n' -android13-8-00021-g46a5565a0982 > "$SRC/.scmversion"
mkdir -p "$OUT"

"${M[@]}" gki_defconfig
# build.config.gki: POST_DEFCONFIG_CMDS=check_defconfig
"${M[@]}" savedefconfig
diff -u "$SRC/arch/arm64/configs/gki_defconfig" "$OUT/defconfig"

# KMI symbol list -> CONFIG_UNUSED_KSYMS_WHITELIST (TRIM_NONLISTED_KMI=1)
KMI_LISTS="android/abi_gki_aarch64 $(sed -n '/^ADDITIONAL_KMI_SYMBOL_LISTS="/,/^"/p' "$SRC/build.config.gki.aarch64" | grep '^android/' | tr '\n' ' ')"
(cd "$SRC" && python3 "$ROOT/build/kernel/abi/process_symbols" --out-dir="$OUT" \
	--out-file=abi_symbollist --report-file=abi_symbollist.report --in-dir=. $KMI_LISTS)
"$ROOT/build/kernel/abi/flatten_symbol_list" < "$OUT/abi_symbollist" > "$OUT/abi_symbollist.raw"
"$SRC/scripts/config" --file "$OUT/.config" -d UNUSED_SYMBOLS -e TRIM_UNUSED_KSYMS \
	--set-str UNUSED_KSYMS_WHITELIST "$OUT/abi_symbollist.raw"
if [ "$VARIANT" = tier1 ]; then
	"$SRC/scripts/config" --file "$OUT/.config" -e DEVTMPFS -e DEVTMPFS_MOUNT -e FHANDLE -e AUTOFS_FS
fi
# EXTRA_MODULES="BT_HCIVHCI ...": extra drivers as =m, for out-of-Image modules
# built with "M=<dir> modules" after modules_prepare (the Image is unchanged;
# they link against the exported KMI symbols in Module.symvers).
for c in ${EXTRA_MODULES:-}; do "$SRC/scripts/config" --file "$OUT/.config" -m "$c"; done
"${M[@]}" olddefconfig

# Embed the stock build's module-signing certificate (public part only, cut out
# of the stock Image) instead of generating a new key pair: the stock signed GKI
# modules (system_dlkm) still verify, and the Image can match stock. No private
# key exists, so `modules_install` cannot sign; Image/modules don't need it.
mkdir -p "$OUT/certs"
[ -e "$OUT/certs/x509.genkey" ] || : > "$OUT/certs/x509.genkey"
cmp -s "$K/gki-ab13743836/gki-module-signing-cert.pem" "$OUT/certs/signing_key.pem" ||
	cp "$K/gki-ab13743836/gki-module-signing-cert.pem" "$OUT/certs/signing_key.pem"

[ "$TARGETS" = none ] || "${M[@]}" -j"${JOBS:-4}" $TARGETS
