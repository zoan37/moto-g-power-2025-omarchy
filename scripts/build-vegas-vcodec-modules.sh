#!/usr/bin/env bash
# Rebuild the MediaTek vendor video-codec modules (mtk-vcodec-dec-v1.ko and
# friends) for the vegas stock kernel, from Motorola's kernel-mtk tree, the way
# MediaTek's build does it: a "mixed" build (KBUILD_MIXED_TREE) of the device
# tree's modules against the GKI kernel (vmlinux, vmlinux.symvers, System.map of
# ab13743836, rebuilt byte-identically by scripts/build-vegas-gki-kernel.sh).
# Never talks to the phone.
#
# usage: scripts/build-vegas-vcodec-modules.sh stock|patched [make targets]
#   stock    unmodified kernel-mtk (tag android-16-release-w1ve36h.10-12r3)
#   patched  same + port/vegas/patches/mtk-vcodec-dec-mm21-2plane.patch;
#            its out dir starts as a copy of the stock out dir (incremental)
# Default target: modules. JOBS=4 by default.
#
# Layout (under third_party/kernel/):
#   kernel-mtk/          MotorolaMobilityLLC/kernel-mtk checkout (read-only here)
#   gki-src/             GKI tree + prebuilts from notes/kernel-build-20261010.md,
#                        with out/android13-5.15/common built ("stock" variant)
#   mtk-patched/         created: hardlinked copy of kernel-mtk, real copy of
#                        drivers/media/platform/mtk-vcodec, patch applied
#   (no mount point on disk) the source tree used is bind-mounted, in a
#                        bubblewrap sandbox (no root), at the path of MediaTek's
#                        build host, $LR below: clang 14 embeds that absolute
#                        path in the CFI failure data, so .rodata only matches
#                        stock byte for byte when the path is the same
#   mtk-build/{stock,patched}/  O= build directories
#   mtk-mixed/           symlinks to the GKI vmlinux, vmlinux.symvers, System.map
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
K=$REPO/third_party/kernel
VARIANT=${1:?usage: $0 stock|patched [make targets]}
shift
TARGETS=${*:-modules}
PATCH=$REPO/port/vegas/patches/mtk-vcodec-dec-mm21-2plane.patch
VC=drivers/media/platform/mtk-vcodec
# source path of the stock vendor_dlkm build (strings in the stock .ko files);
# the tree expects Motorola's modules (third_party/kernel/motorola-kernel-modules)
# at ../motorola/kernel/modules
LR=/localrepo/hudson/jenkins-master/tmp_ws_bld_dirs/15c6feb3a844/bound_build_dir/kernel-5.15

case $VARIANT in
stock) TREE=$K/kernel-mtk ;;
patched) TREE=$K/mtk-patched ;;
*) echo "unknown variant $VARIANT" >&2; exit 2 ;;
esac

if [ -z "${IN_MTK_NS:-}" ]; then
	if [ "$VARIANT" = patched ] && [ ! -e "$TREE/.patched" ]; then
		# hardlink everything except the vcodec driver, which gets real copies
		rsync -a --link-dest="$K/kernel-mtk/" --exclude="/$VC/" "$K/kernel-mtk/" "$TREE/"
		rsync -a "$K/kernel-mtk/$VC/" "$TREE/$VC/"
		patch -d "$TREE" -p1 --no-backup-if-mismatch < "$PATCH"
		cp "$PATCH" "$TREE/.patched"
	fi
	# new root: host /usr, /etc, /home (+ $LR); nothing else is visible
	exec bwrap --ro-bind /usr /usr --symlink usr/bin /bin --symlink usr/lib /lib \
		--symlink usr/lib /lib64 --symlink usr/bin /sbin --ro-bind /etc /etc \
		--bind /home /home --proc /proc --dev /dev --tmpfs /tmp \
		--ro-bind "$K/motorola-kernel-modules" "${LR%/*}/motorola/kernel/modules" --bind "$TREE" "$LR" \
		--setenv IN_MTK_NS 1 --chdir "$REPO" \
		bash "$REPO/scripts/build-vegas-vcodec-modules.sh" "$VARIANT" $TARGETS
fi

SRC=$LR
OUT=$K/mtk-build/$VARIANT
GKI=$(cd "$K/gki-src" && pwd -P)
GKIOUT=$GKI/out/android13-5.15/common
MIXED=$K/mtk-mixed
FRAG=$SRC/arch/arm64/configs

# Environment from kernel-mtk build.config.{common,constants,mtk.aarch64} and
# scripts/gen_build_config.py (BUILD_NUMBER unset, no KMI trimming).
export PATH=$GKI/prebuilts/clang/host/linux-x86/clang-r450784e/bin:$GKI/prebuilts/kernel-build-tools/linux-x86/bin:/usr/bin
export ARCH=arm64 BRANCH=android13-5.15 KMI_GENERATION=8
unset BUILD_NUMBER
export TZ=UTC LC_ALL=C
export KCFLAGS=-D__ANDROID_COMMON_KERNEL__
NDK_SYSROOT=$GKI/prebuilts/ndk-r23/toolchains/llvm/prebuilt/linux-x86_64/sysroot
export USERCFLAGS="--target=aarch64-linux-android31 --sysroot=$NDK_SYSROOT -Wno-unused-function "
export USERLDFLAGS="-fuse-ld=lld --rtlib=compiler-rt --target=aarch64-linux-android31 "
unset LD_LIBRARY_PATH PYTHONPATH PYTHONHOME PYTHONSTARTUP

mkdir -p "$MIXED"
for f in vmlinux vmlinux.symvers System.map; do ln -sfn "$GKIOUT/$f" "$MIXED/$f"; done

# Release string of the stock vendor modules: 5.15.180-android13-8-gd63125a6a58d
# (kernel-mtk commit d63125a6a58d; the public tag is a later merge of it).
[ "$(cat "$SRC/.scmversion" 2>/dev/null)" = -android13-8-gd63125a6a58d ] ||
	printf '%s\n' -android13-8-gd63125a6a58d > "$SRC/.scmversion"

M=(make -C "$SRC" O="$OUT" LLVM=1 LLVM_IAS=1 DEPMOD=depmod KBUILD_MIXED_TREE="$MIXED")

if [ "$VARIANT" = patched ] && [ ! -e "$OUT/.config" ]; then
	mkdir -p "$K/mtk-build"
	cp -a "$K/mtk-build/stock" "$OUT"
fi

if [ ! -e "$OUT/.config" ]; then
	mkdir -p "$OUT"
	# DEFCONFIG="gki_defconfig mgk_64_k515.config moto-mgk_64_k515.config
	#            moto-mgk_64_k515-vegas.config" (user build: no debug-*, the -df
	#            fragment is empty). Each %.config target = merge + olddefconfig.
	"${M[@]}" gki_defconfig
	for f in "$FRAG/mgk_64_k515_defconfig" "$FRAG/ext_config/moto-mgk_64_k515.config" \
		"$FRAG/ext_config/moto-mgk_64_k515-vegas.config"; do
		(cd "$OUT" && "$SRC/scripts/kconfig/merge_config.sh" -m .config "$f")
		"${M[@]}" olddefconfig
	done
fi

[ "$TARGETS" = none ] || "${M[@]}" -j"${JOBS:-4}" $TARGETS
