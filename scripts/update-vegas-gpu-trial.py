#!/usr/bin/env python3
"""Check or atomically update one isolated Mali trial driver, never system Mesa."""
import argparse
import base64
import gzip
import hashlib
import json
from pathlib import Path
import runpy
import shlex

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = ROOT / "artifacts/vegas-linux-bringup/mesa-kbase-build"
TARGET = "/opt/vegas-gpu/mesa-kbase/lib/dri/panfrost_kbase_dri.so"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    binary = args.binary.resolve()
    assert binary.is_relative_to(PACKAGE.resolve()) and binary.suffix == ".so"
    data = binary.read_bytes()
    assert data[:4] == b"\x7fELF" and data[4:6] == b"\x02\x01"
    assert int.from_bytes(data[18:20], "little") == 183, "AArch64 ELF required"
    assert len(data) < 32 * 1024 * 1024
    digest = hashlib.sha256(data).hexdigest()
    payload = gzip.compress(data, mtime=0)
    compressed_digest = hashlib.sha256(payload).hexdigest()
    print(f"AArch64 trial driver checked: {digest}", flush=True)
    if not args.apply:
        assert not args.smoke, "--smoke requires --apply"
        return
    checkpoint = json.loads((ROOT / "private/linux-bringup/desktop-progress.json").read_text())
    assert checkpoint["verified"]
    cid = checkpoint["cid"]
    assert len(cid) == 32 and all(c in "0123456789abcdefABCDEF" for c in cid)
    command = runpy.run_path(str(ROOT / "scripts/measure-vegas-input.py"))["command"]
    guard = ("set -e\n"
             '[[ $(findmnt -n -o SOURCE /) == "/dev/mmcblk0p1[/omarchy]" ]]\n'
             '[[ $(uname -r) == "5.15.180-android13-8-00021-g46a5565a0982-ab13743836" ]]\n'
             '[[ $(cat /proc/sys/kernel/hostname) == moto-vegas ]]\n'
             f'[[ $(cat /sys/class/block/mmcblk0/device/cid) == {shlex.quote(cid)} ]]\n')
    stage = f"/run/vegas-gpu-update-{digest[:16]}"
    command(guard + f"umask 077\nmkdir {stage}\n: >{stage}/driver.gz\n")
    for offset in range(0, len(payload), 49152):
        chunk = base64.b64encode(payload[offset:offset + 49152]).decode()
        command(guard + f"printf %s {shlex.quote(chunk)} | base64 -d >>{stage}/driver.gz\n")
    command(guard + f"""[[ $(sha256sum {stage}/driver.gz | cut -d' ' -f1) == {compressed_digest} ]]
gzip -dc {stage}/driver.gz >{stage}/driver.so
[[ $(sha256sum {stage}/driver.so | cut -d' ' -f1) == {digest} ]]
for p in /opt /opt/vegas-gpu /opt/vegas-gpu/mesa-kbase /opt/vegas-gpu/mesa-kbase/lib /opt/vegas-gpu/mesa-kbase/lib/dri {TARGET}; do [[ ! -L $p ]]; done
[[ -f {TARGET} && ! -e {TARGET}.next ]]
cp -a {TARGET} {stage}/previous-driver.so
install -m 0755 {stage}/driver.so {TARGET}.next
mv {TARGET}.next {TARGET}
[[ $(sha256sum {TARGET} | cut -d' ' -f1) == {digest} ]]
""")
    print(f"Updated isolated driver; previous version: {stage}/previous-driver.so", flush=True)
    if args.smoke:
        log = command(guard + """ulimit -c 0
token=$(mktemp /run/vegas-gpu-watchdog.XXXXXX)
(sleep 60; [[ -f $token ]] && /usr/local/libexec/vegas/power-transition reboot) >/run/vegas-gpu-watchdog.log 2>&1 &
watchdog=$!
trap 'rm -f "$token"; kill "$watchdog" 2>/dev/null || true' EXIT
set +e
runuser -u omarchy -- env -u MESA_LOADER_DRIVER_OVERRIDE VEGAS_GPU_TRACE=1 LD_LIBRARY_PATH=/opt/vegas-gpu/mesa-kbase/lib LIBGL_DRIVERS_PATH=/opt/vegas-gpu/mesa-kbase/lib/dri timeout 15 /opt/vegas-gpu/mesa-kbase/egl-smoke
status=$?
printf 'EGL_STATUS:%s\\n' "$status"
exit 0
""", timeout=75)
        logs = ROOT / "private/linux-bringup/gpu-20261007"
        logs.mkdir(parents=True, exist_ok=True)
        (logs / f"egl-{digest[:16]}.log").write_text(log)
        print(log)


if __name__ == "__main__":
    main()
