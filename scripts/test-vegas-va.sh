#!/usr/bin/env bash
# Regression test for the mtk_stateful VA-API driver, run on the phone as root:
# decodes each clip through VA-API (ffmpeg -hwaccel vaapi, frames downloaded as
# NV12) and in software, compares per-frame MD5s, and times the hardware decode.
#
# usage: test-vegas-va.sh CLIP...   (H.264/VP9 files; ffmpeg + the driver needed)
# env:   FRAMES=N limits the frames compared (default: all)
set -uo pipefail

export LIBVA_DRIVERS_PATH=${LIBVA_DRIVERS_PATH:-/opt/vegas-video/va}
export LIBVA_DRIVER_NAME=${LIBVA_DRIVER_NAME:-mtk_stateful}
DEV=/dev/dri/renderD128
if [ ! -e $DEV ]; then
	mknod -m 660 $DEV c 226 0 && chgrp video $DEV
fi
lim=()
[ -n "${FRAMES:-}" ] && lim=(-frames:v "$FRAMES")
tmp=$(mktemp -d /tmp/vegas-va-test.XXXXXX)
fail=0

for clip in "$@"; do
	name=$(basename "$clip")
	timeout 300 ffmpeg -nostdin -y -v error -hwaccel vaapi -hwaccel_device $DEV \
		-hwaccel_output_format vaapi -i "$clip" "${lim[@]}" \
		-vf hwdownload,format=nv12 -f framemd5 "$tmp/hw.md5" 2>"$tmp/hw.err"
	timeout 300 ffmpeg -nostdin -y -v error -i "$clip" "${lim[@]}" -pix_fmt nv12 \
		-f framemd5 "$tmp/sw.md5" 2>/dev/null
	grep -v '^#' "$tmp/hw.md5" | awk -F, '{print $NF}' >"$tmp/hw.list"
	grep -v '^#' "$tmp/sw.md5" | awk -F, '{print $NF}' >"$tmp/sw.list"
	total=$(wc -l <"$tmp/sw.list")
	got=$(wc -l <"$tmp/hw.list")
	same=$(paste -d' ' "$tmp/hw.list" "$tmp/sw.list" | awk '$1 == $2' | wc -l)
	first=$(paste -d' ' "$tmp/hw.list" "$tmp/sw.list" | awk '$1 != $2 {print NR; exit}')
	# decode speed: download included (the driver detiles on dequeue anyway);
	# SW=1 also times the software decode for comparison
	TIMEFORMAT='%R %U %S'
	t=$( { time timeout 300 ffmpeg -nostdin -y -v error \
		-hwaccel vaapi -hwaccel_device $DEV -hwaccel_output_format vaapi \
		-i "$clip" -vf hwdownload,format=nv12 -f null - 2>/dev/null; } 2>&1 | tail -1)
	read -r el us sy <<<"$t"
	fps=$(awk -v n="$total" -v e="$el" 'BEGIN { printf "%.1f", (e > 0) ? n / e : 0 }')
	cpu=$(awk -v u="$us" -v s="$sy" -v e="$el" 'BEGIN { printf "%.2f", (e > 0) ? (u + s) / e : 0 }')
	swinfo=
	if [ -n "${SW:-}" ]; then
		t=$( { time timeout 300 ffmpeg -nostdin -y -v error -i "$clip" \
			-pix_fmt nv12 -f null - 2>/dev/null; } 2>&1 | tail -1)
		read -r el us sy <<<"$t"
		swinfo=$(awk -v n="$total" -v e="$el" -v u="$us" -v s="$sy" \
			'BEGIN { printf " (software: %.1f fps, %.2f cores)", (e > 0) ? n / e : 0, (e > 0) ? (u + s) / e : 0 }')
	fi
	status=OK
	[ "$same" = "$total" ] && [ "$got" = "$total" ] || { status=MISMATCH; fail=1; }
	printf '%-14s %s: %s/%s frames bit-exact (hw gave %s%s), %s fps, %s cores%s\n' \
		"$name" "$status" "$same" "$total" "$got" "${first:+, first diff #$first}" "$fps" "$cpu" "$swinfo"
	grep -m3 -E 'mtk_stateful|rror' "$tmp/hw.err" | sed 's/^/    /'
done
rm -f "$tmp"/*.md5 "$tmp"/*.list "$tmp"/*.err
rmdir "$tmp"
exit $fail
