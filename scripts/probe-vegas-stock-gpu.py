#!/usr/bin/env python3
"""Upload verified original modules; --load performs a reversible one-boot probe."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import runpy
import shlex

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = ROOT / 'artifacts/vegas-linux-bringup/gpu-stock-probe'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--load', action='store_true')
    args = parser.parse_args()
    manifest = json.loads((PACKAGE / 'manifest.json').read_text())
    payload = (PACKAGE / manifest['archive']).read_bytes()
    assert hashlib.sha256(payload).hexdigest() == manifest['sha256']
    if not args.load:
        print('Original GPU module archive checked. No phone changes; --load runs one boot only.')
        return
    checkpoint = json.loads((ROOT / 'private/linux-bringup/desktop-progress.json').read_text())
    assert checkpoint['verified']
    cid = checkpoint['cid']
    assert len(cid) == 32 and all(c in '0123456789abcdefABCDEF' for c in cid)
    command = runpy.run_path(str(ROOT / 'scripts/measure-vegas-input.py'))['command']
    guard = ('set -e\n'
             '[[ $(findmnt -n -o SOURCE /) == "/dev/mmcblk0p1[/omarchy]" ]]\n'
             '[[ $(uname -r) == "5.15.180-android13-8-00021-g46a5565a0982-ab13743836" ]]\n'
             '[[ $(cat /proc/sys/kernel/hostname) == moto-vegas ]]\n'
             f'[[ $(cat /sys/class/block/mmcblk0/device/cid) == {shlex.quote(cid)} ]]\n')
    stage = '/run/vegas-gpu-stock-probe'
    command(guard + f'umask 077\nmkdir {stage}\n: >{stage}/modules.tar.gz\n')
    for offset in range(0, len(payload), 49152):
        chunk = base64.b64encode(payload[offset:offset + 49152]).decode()
        command(guard + f'printf %s {shlex.quote(chunk)} | base64 -d >>{stage}/modules.tar.gz\n')
    check = guard + f'cd {stage}\n[[ $(sha256sum modules.tar.gz | cut -d" " -f1) == {manifest["sha256"]} ]]\nmkdir modules\ntar -xzf modules.tar.gz -C modules\n'
    for r in manifest['modules']:
        check += f'[[ $(sha256sum modules/{r["file"]} | cut -d" " -f1) == {r["sha256"]} ]]\n'
    command(check)
    props = base64.b64encode((ROOT / 'bringup/gpu-props.py').read_bytes()).decode()
    command(guard + f'printf %s {shlex.quote(props)} | base64 -d >{stage}/gpu-props.py\n')
    # First validate the complete payload. Then load in dependency order, skip
    # already loaded modules, stop on the first error. No force-load/unload.
    probe = guard + f'cd {stage}\n: >probe.log\n'
    probe += '[[ $(cat /sys/class/power_supply/battery/temp) -lt 350 ]]\n'
    probe += 'token=$(mktemp /run/vegas-gpu-watchdog.XXXXXX)\n'
    probe += '(sleep 120; [[ -f $token ]] && /usr/local/libexec/vegas/power-transition reboot) >watchdog.log 2>&1 &\nwatchdog=$!\n'
    probe += "trap 'rm -f \"$token\"; kill \"$watchdog\" 2>/dev/null || true' EXIT\n"
    for r in manifest['modules']:
        probe += (f'if [[ ! -d /sys/module/{r["name"]} ]]; then\n'
                  f'  echo LOAD:{r["name"]} >>probe.log\n'
                  f'  timeout 12 insmod modules/{r["file"]} >>probe.log 2>&1\n'
                  'fi\n')
    probe += 'udevadm trigger --action=add --subsystem-match=misc --sysname-match="mali*"\nudevadm settle --timeout=5\ncat probe.log\n'
    probe += 'for attempt in {1..50}; do [[ -r /sys/class/misc/mali0/dev ]] && break; sleep .1; done\n'
    probe += 'if [[ ! -r /sys/class/misc/mali0/dev ]]; then cat /sys/kernel/debug/devices_deferred 2>/dev/null || true; exit 1; fi\n'
    probe += 'IFS=: read -r major minor </sys/class/misc/mali0/dev\n[[ $major =~ ^[0-9]+$ && $minor =~ ^[0-9]+$ ]]\n'
    probe += '[[ -e /dev/mali0 ]] || mknod /dev/mali0 c "$major" "$minor"\nchmod 0600 /dev/mali0\n'
    probe += 'python gpu-props.py\n'
    probe += 'ls /dev/mali* /dev/dri/* 2>/dev/null || true\n'
    probe += 'cat /sys/class/power_supply/battery/temp\n'
    out = ROOT / 'private/linux-bringup/gpu-20261007'
    out.mkdir(parents=True, exist_ok=True)
    try:
        result = command(probe, timeout=85)
    except Exception:
        result = command(f'cat {stage}/probe.log\ndmesg | tail -100\n')
        (out / 'stock-module-failure.log').write_text(result)
        raise
    (out / 'stock-module-probe.log').write_text(result)
    print(result.split('NATIVE_COMMAND_STATUS:')[0].strip())
    print('No automatic GPU startup installed. Native reboot returns to the original module set.')


if __name__ == '__main__':
    main()
