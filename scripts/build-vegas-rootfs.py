#!/usr/bin/env python3
"""Prepare a regular-file Arch ARM filesystem; never access the phone/card."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
VM = ROOT / 'vm'
OUT = ROOT / 'artifacts/vegas-linux-bringup/arch-rootfs'
SIGNER = '68B3537F39A313B3E574D06777193F152BDBE6A6'


def main():
    archive = VM / 'ArchLinuxARM-aarch64-latest.tar.gz'
    signature = subprocess.run(
        ['gpg', '--homedir', str(VM / 'gnupg'), '--status-fd', '1',
         '--verify', str(archive) + '.sig', str(archive)],
        check=True, capture_output=True, text=True)
    if f'[GNUPG:] VALIDSIG {SIGNER} ' not in signature.stdout:
        raise SystemExit('Unexpected archive signer')
    OUT.mkdir(parents=True, exist_ok=True)
    image = OUT / 'vegas-arch-rootfs.ext4'
    # Exclusive creation prevents following a preexisting symlink or overwriting
    # another image. mkfs operates only on this new regular file, never /dev.
    with image.open('xb') as f:
        f.truncate(8 * 1024 ** 3)
    subprocess.run(
        ['fakeroot', '-i', str(VM / 'fakeroot.state'), 'mkfs.ext4', '-q', '-F',
         '-b', '4096', '-L', 'vegas-arch', '-O', '^orphan_file', '-d', str(VM / 'rootfs'), str(image)],
        check=True)
    check = subprocess.run(['e2fsck', '-fn', str(image)],
                           check=True, capture_output=True, text=True)
    (OUT / 'filesystem-check.txt').write_text(check.stdout + check.stderr)
    digest = hashlib.sha256()
    with image.open('rb') as f:
        while chunk := f.read(8 * 1024 ** 2):
            digest.update(chunk)
    manifest = {
        'image': image.name, 'bytes': image.stat().st_size,
        'sha256': digest.hexdigest(), 'archive_signature_verified': True,
        'archive_signer': SIGNER, 'ownership_source': 'vm/fakeroot.state',
        'filesystem_check_passed': True, 'card_written': False,
        'bootable_on_phone_verified': False, 'desktop_installed': False,
        'purpose': 'Arch ARM userspace for a later microSD partition; retain the Moto stock kernel',
    }
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Prepared and checked an 8 GiB sparse Arch ARM filesystem image. No card written.')


if __name__ == '__main__':
    main()
