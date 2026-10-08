#!/bin/bash
# Root-only bounded off-screen validation. Never takes DRM master, restarts
# Hyprland, changes boot settings, or persists device permissions.
set -euo pipefail
[[ $EUID == 0 ]]
[[ $(cat /proc/sys/kernel/hostname) == moto-vegas ]]
[[ $(findmnt -n -o SOURCE /) == '/dev/mmcblk0p1[/omarchy]' ]]
[[ $(uname -r) == '5.15.180-android13-8-00021-g46a5565a0982-ab13743836' ]]
stage=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
iterations=${1:-300}
[[ $iterations =~ ^[1-9][0-9]{0,3}$ && $iterations -le 2000 ]]
exec 9>/run/vegas-libmali-probes.lock
flock -n 9
heap=/dev/dma_heap/system
[[ -c $heap ]]
read -r uid gid mode < <(stat -c '%u %g %a' "$heap")
cleanup() {
    chown "$uid:$gid" "$heap"
    chmod "$mode" "$heap"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
python3 - "$stage" <<'PY'
import hashlib,json,pathlib,sys
p=pathlib.Path(sys.argv[1]); m=json.loads((p/'manifest.json').read_text())
for name, expected in m['files'].items():
    f=p/name
    if f.is_symlink() or hashlib.sha256(f.read_bytes()).hexdigest() != expected:
        raise SystemExit('Trial file failed hash check: '+name)
for name, target in m['symlinks'].items():
    f=p/name
    if not f.is_symlink() or str(f.readlink()) != target:
        raise SystemExit('Trial link failed check: '+name)
PY
ulimit -c 0
chgrp video "$heap"
chmod 0660 "$heap"
timeout -k 2 60 runuser -u omarchy -- env \
    -u LIBGL_DRIVERS_PATH -u MESA_LOADER_DRIVER_OVERRIDE -u LIBGL_ALWAYS_SOFTWARE \
    XDG_RUNTIME_DIR=/run/user/1000 VEGAS_PROBE_GBM=1 VEGAS_LIBMALI_JM_COMPAT=stride72 \
    LD_LIBRARY_PATH="$stage/lib" LD_PRELOAD="$stage/libmali-jm-compat.so" \
    /bin/bash -euc '"$1/gpu-libmali-probe" "$2"; "$1/gpu-hypr-probe" 1080 2388 "$2" sbctox; "$1/gpu-fence-probe" "$2"' \
    bash "$stage" "$iterations"
