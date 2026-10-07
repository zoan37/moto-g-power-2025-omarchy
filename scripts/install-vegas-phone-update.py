#!/usr/bin/env python3
"""Install a checked candidate over the native USB link with per-file rollback."""
import argparse
import base64
import hashlib
import json
from pathlib import Path, PurePosixPath
import secrets
import shlex
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = ROOT / 'artifacts/vegas-linux-bringup/phone-polish-candidate'
URL = 'http://192.168.77.1:8080/cgi-bin/command'


def validate():
    manifest = json.loads((PACKAGE / 'manifest.json').read_text())
    archive = PACKAGE / manifest['archive']
    payload = archive.read_bytes()
    if hashlib.sha256(payload).hexdigest() != manifest['sha256']:
        raise SystemExit('Candidate archive checksum mismatch')
    records = manifest['files']
    with tarfile.open(archive) as tar:
        members = tar.getmembers()
        if len(members) != len(records) or len(set(r['path'] for r in records)) != len(records):
            raise SystemExit('Candidate file list mismatch')
        for member, record in zip(members, records):
            name = PurePosixPath(member.name)
            if (name.is_absolute() or '..' in name.parts or not member.isfile()
                or member.name != record['path']
                or not member.name.startswith(('usr/local/', 'home/omarchy/.config/'))
                or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-/' for c in member.name)):
                raise SystemExit('Unsafe candidate entry')
            digest = hashlib.sha256(tar.extractfile(member).read()).hexdigest()
            if digest != record['sha256'] or member.uid != record['uid'] or member.gid != record['gid'] or format(member.mode, '04o') != record['mode']:
                raise SystemExit('Candidate entry metadata or digest mismatch')
    return manifest, payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', help='Upload and install on the connected native phone')
    parser.add_argument('--reboot', action='store_true', help='Reboot native Linux after installing')
    args = parser.parse_args()
    if args.reboot and not args.apply:
        parser.error('--reboot requires --apply')
    manifest, payload = validate()
    if not args.apply:
        print('Candidate archive, files and ownership checked. Nothing uploaded; use --apply when the native phone is connected.')
        return
    progress = json.loads((ROOT / 'private/linux-bringup/desktop-progress.json').read_text())
    cid = progress['cid']
    if not progress['verified'] or len(cid) != 32 or any(c not in '0123456789abcdefABCDEF' for c in cid):
        raise SystemExit('Missing verified private microSD checkpoint')
    guard = ('set -e\n'
             '[[ $(findmnt -n -o SOURCE /) == "/dev/mmcblk0p1[/omarchy]" ]]\n'
             '[[ $(uname -r) == "5.15.180-android13-8-00021-g46a5565a0982-ab13743836" ]]\n'
             '[[ $(cat /proc/sys/kernel/hostname) == moto-vegas ]]\n'
             f'[[ $(cat /sys/class/block/mmcblk0/device/cid) == {shlex.quote(cid)} ]]\n')
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def post(script):
        body = script.encode()
        if len(body) > 131072:
            raise RuntimeError('CGI request exceeds its reviewed limit')
        with opener.open(urllib.request.Request(URL, body, method='POST'), timeout=85) as response:
            result = response.read().decode(errors='replace')
        if not result.rstrip().endswith('NATIVE_COMMAND_STATUS:0'):
            raise RuntimeError('Native update command failed; inspect the private phone logs')
        return result

    tag = secrets.token_hex(8)
    stage = f'/run/vegas-update-{tag}'
    backup = f'/var/lib/vegas/phone-updates/{tag}'
    post(guard + f'umask 077\nmkdir {stage}\n: >{stage}/update.tar.gz\n')
    for offset in range(0, len(payload), 49152):
        chunk = base64.b64encode(payload[offset:offset + 49152]).decode()
        post(guard + f'printf %s {shlex.quote(chunk)} | base64 -d >>{stage}/update.tar.gz\n')
    lines = [guard, 'umask 077', f'cd {stage}',
             f'[[ $(sha256sum update.tar.gz | cut -d " " -f1) == {manifest["sha256"]} ]]',
             'mkdir payload', 'tar -xzf update.tar.gz -C payload', f'mkdir -p {backup}/files']
    rollback = ['#!/bin/bash', 'set -e', f'cd {backup}']
    # Validate every source/destination before replacing any file. Reject
    # symlinks in the destination and its ancestors; this update has no links.
    for record in manifest['files']:
        name = record['path']; target = '/' + name
        lines += [f'[[ $(sha256sum payload/{name} | cut -d " " -f1) == {record["sha256"]} ]]',
                  f'for ancestor in {target} $(dirname {target}); do',
                  '  while [[ $ancestor != / ]]; do [[ ! -L $ancestor ]]; ancestor=$(dirname "$ancestor"); done', 'done',
                  f'[[ ! -e {target} || -f {target} ]]', f'mkdir -p {backup}/files/{PurePosixPath(name).parent}',
                  f'if [[ -f {target} ]]; then cp -a {target} {backup}/files/{name}; fi']
        rollback += [f'if [[ -f files/{name} ]]; then cp -a --remove-destination files/{name} {target}; else rm -f {target}; fi']
    rollback += ['sync', 'echo "Previous phone files restored; reboot native Linux to apply."']
    text = '\n'.join(rollback) + '\n'
    lines += [f'printf %s {shlex.quote(text)} >{backup}/rollback.sh', f'chmod 0700 {backup}/rollback.sh',
              'committed=0', f'trap \'if (( ! committed )); then bash {backup}/rollback.sh; fi\' EXIT']
    for record in manifest['files']:
        name = record['path']; parent = PurePosixPath(name).parent.as_posix()
        lines += [f'if [[ ! -d /{parent} ]]; then install -d -m 0755 -o {record["uid"]} -g {record["gid"]} /{parent}; fi',
                  f'install -m {record["mode"]} -o {record["uid"]} -g {record["gid"]} payload/{name} /{name}']
    lines += ['bash -n /usr/local/libexec/vegas/native-init /usr/local/libexec/vegas/power-transition /usr/local/bin/vegas-power',
              '/usr/local/bin/wvkbd-mobintl --help >/dev/null 2>&1', 'sync', 'committed=1', 'trap - EXIT',
              f'echo UPDATE_BACKUP:{backup}', f'rm -rf {stage}']
    result = post('\n'.join(lines) + '\n')
    print(result.split('NATIVE_COMMAND_STATUS:')[0].strip())
    print('Candidate installed; its fit, typing latency and cable-disconnected shutdown still need hardware checks.')
    print('Native reboot required to load the updated power-request watcher.')
    if args.reboot:
        post(guard + '(sleep 2; /usr/local/libexec/vegas/power-transition reboot) >/dev/null 2>&1 &\n')
        print('Native reboot scheduled.')


if __name__ == '__main__':
    main()
