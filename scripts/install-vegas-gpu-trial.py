#!/usr/bin/env python3
"""Install only the isolated GPU trial; system graphics libraries stay intact."""
import argparse
import base64
import hashlib
import json
from pathlib import Path, PurePosixPath
import runpy
import shlex
import tarfile

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = ROOT / 'artifacts/vegas-linux-bringup/mesa-kbase-build'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    manifest = json.loads((PACKAGE / 'manifest.json').read_text())
    payload = (PACKAGE / manifest['archive']).read_bytes()
    assert hashlib.sha256(payload).hexdigest() == manifest['sha256']
    with tarfile.open(PACKAGE / manifest['archive']) as tar:
        for member in tar.getmembers():
            name = PurePosixPath(member.name)
            assert not name.is_absolute() and '..' not in name.parts
            assert name.parts[0] in ('lib', 'share', 'egl-smoke')
            assert member.isfile() or member.isdir() or member.issym() or member.islnk()
            if member.issym() or member.islnk():
                link = PurePosixPath(member.linkname)
                assert not link.is_absolute() and '..' not in link.parts
            if member.isfile():
                r = next(x for x in manifest['files'] if x['path'] == member.name)
                assert hashlib.sha256(tar.extractfile(member).read()).hexdigest() == r['sha256']
    if not args.apply:
        print('Isolated graphics archive checked; no phone changes.')
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
    prefix = manifest['prefix']
    assert prefix == '/opt/vegas-gpu/mesa-kbase'
    stage = '/run/vegas-gpu-userspace'
    command(guard + f'[[ ! -e {prefix} ]]\n[[ ! -L /opt && ! -L /opt/vegas-gpu ]]\numask 077\nmkdir {stage}\n: >{stage}/trial.tar.gz\n')
    for offset in range(0, len(payload), 49152):
        chunk = base64.b64encode(payload[offset:offset + 49152]).decode()
        command(guard + f'printf %s {shlex.quote(chunk)} | base64 -d >>{stage}/trial.tar.gz\n')
    script = guard + f'[[ $(sha256sum {stage}/trial.tar.gz | cut -d" " -f1) == {manifest["sha256"]} ]]\n'
    script += 'install -d -m 0755 /opt/vegas-gpu\nmkdir /opt/vegas-gpu/mesa-kbase-staging\n'
    script += f'tar -xzf {stage}/trial.tar.gz -C /opt/vegas-gpu/mesa-kbase-staging --no-same-owner\n'
    for r in manifest['files']:
        script += f'[[ $(sha256sum /opt/vegas-gpu/mesa-kbase-staging/{r["path"]} | cut -d" " -f1) == {r["sha256"]} ]]\n'
    script += f'chmod 0755 /opt/vegas-gpu/mesa-kbase-staging\nmv /opt/vegas-gpu/mesa-kbase-staging {prefix}\nsync\n'
    command(script)
    print('Installed isolated /opt/vegas-gpu/mesa-kbase. No desktop environment or system loader settings changed.')


if __name__ == '__main__':
    main()
