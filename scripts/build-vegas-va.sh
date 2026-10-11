#!/usr/bin/env bash
# Build and install the mtk_stateful VA-API driver (port/vegas/va-stateful/) on
# the phone: VA-API for the stateful MediaTek decoder (mtk-vcodec-dec-v1,
# /dev/video0), so Google Chrome (VA-API only on Linux) decodes H.264 and VP9
# in hardware. See notes/video-decode-chrome-va-20261011.md.
#
# usage: scripts/build-vegas-va.sh            from the laptop: copies the source to
#                                            the phone over the USB command endpoint
#                                            and builds there
#        scripts/build-vegas-va.sh --local    on the phone (root), from a checkout
#
# Installs /opt/vegas-video/va/mtk_stateful_drv_video.so (outside pacman's files)
# and checks it with vainfo. Needs on the phone: base-devel meson libva libdrm
# libva-utils. The driver ABI follows libva's __vaDriverInit_1_xx: rebuild after
# a libva update.
#
# Using it: LIBVA_DRIVERS_PATH=/opt/vegas-video/va LIBVA_DRIVER_NAME=mtk_stateful;
# libva needs a DRM device to open, and the phone has no render node, so create
# one for the mediatek-drm card: mknod -m 660 /dev/dri/renderD128 c 226 0;
# chgrp video /dev/dri/renderD128 (the driver doesn't use it).
set -euo pipefail

SRC=/opt/vegas-video/src/va-stateful
DEST=/opt/vegas-video/va
URL=${VEGAS_URL:-http://192.168.77.1:8080/cgi-bin/command}

build_local() {
	local src=$1
	cd "$src"
	if [ -d build ]; then
		meson setup --reconfigure --buildtype=release -Ddriverdir="$DEST" build >/dev/null
	else
		meson setup --buildtype=release -Ddriverdir="$DEST" build >/dev/null
	fi
	meson compile -C build
	install -Dm755 build/mtk_stateful_drv_video.so "$DEST/mtk_stateful_drv_video.so"
	echo "installed $DEST/mtk_stateful_drv_video.so"
	if [ ! -e /dev/dri/renderD128 ]; then
		mknod -m 660 /dev/dri/renderD128 c 226 0
		chgrp video /dev/dri/renderD128
	fi
	LIBVA_DRIVERS_PATH=$DEST LIBVA_DRIVER_NAME=mtk_stateful \
		vainfo --display drm --device /dev/dri/renderD128 2>&1 |
		grep -E "VAProfile(H264|VP9)" || { echo "vainfo lists no profiles"; exit 1; }
}

if [ "${1:-}" = --local ]; then
	build_local "$(cd "$(dirname "$0")/../port/vegas/va-stateful" && pwd)"
	exit 0
fi

# From the laptop: ship the source tree, then build on the phone.
here=$(cd "$(dirname "$0")/.." && pwd)
post() { curl -sS --noproxy '*' --max-time 120 --data-binary @- "$URL"; }
payload=$(tar -C "$here/port/vegas/va-stateful" --exclude=build -czf - . | base64 -w0)
echo "set -e; mkdir -p $SRC; rm -f $SRC.tgz.b64" | post >/dev/null
for ((off = 0; off < ${#payload}; off += 60000)); do
	printf 'printf %%s %q >> %s.tgz.b64\n' "${payload:off:60000}" "$SRC" | post >/dev/null
done
{
	cat <<EOF
set -e
base64 -d $SRC.tgz.b64 | tar -C $SRC -xzf - && rm -f $SRC.tgz.b64
EOF
	declare -f build_local
	echo "SRC=$SRC DEST=$DEST; build_local $SRC"
} | post
