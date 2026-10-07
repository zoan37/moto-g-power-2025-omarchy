#!/usr/bin/env python3
"""Build upstream ARM64 modetest without Cairo for the small diagnostic ramdisk."""
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'artifacts/vegas-linux-bringup/arm-tools'
SYSROOT = ROOT / 'vm/rootfs'
VERSION = '2.4.134'
SOURCE_SHA256 = 'ac5e74d157830eb8bee44c6a6bf3ad49774ef0dd2a72bdad74a8f20308b52a95'
SIGNER = '68B3537F39A313B3E574D06777193F152BDBE6A6'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    package = f'libdrm-{VERSION}-1-aarch64.pkg.tar.xz'
    for name in [package, package + '.sig']:
        if not (OUT / name).is_file():
            urllib.request.urlretrieve('https://ca.us.mirror.archlinuxarm.org/aarch64/extra/' + name, OUT / name)
    signature = subprocess.run(['gpg', '--homedir', str(ROOT / 'vm/gnupg'),
                                '--status-fd', '1', '--verify', str(OUT / (package + '.sig')),
                                str(OUT / package)], check=True, capture_output=True, text=True)
    if f'[GNUPG:] VALIDSIG {SIGNER} ' not in signature.stdout:
        raise SystemExit('Package signature did not match the Arch Linux ARM signer')
    subprocess.run(['bsdtar', '-xf', str(OUT / package), '-C', str(OUT)], check=True)
    tarball = OUT / f'libdrm-{VERSION}.tar.xz'
    source_url = 'https://dri.freedesktop.org/libdrm/' + tarball.name
    if not tarball.is_file():
        urllib.request.urlretrieve(source_url, tarball)
    if hashlib.sha256(tarball.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise SystemExit('Source archive digest mismatch')
    subprocess.run(['bsdtar', '-xf', str(tarball), '-C', str(OUT)], check=True)
    src = OUT / f'libdrm-{VERSION}'
    # This release interprets -D as a DRM bus ID, even when given /dev/dri/card0.
    # Permit an explicit node so the ramdisk does not depend on udev discovery.
    kms = src / 'tests/util/kms.c'
    original = '\tif (module || device) {\n\t\tfd = drmOpen(module, device);'
    patched = ('\tif (module || device) {\n'
               '\t\tfd = device && device[0] == \'/\' ? '
               'open(device, O_RDWR | O_CLOEXEC) : drmOpen(module, device);')
    content = kms.read_text()
    if content.count(original) != 1:
        raise SystemExit('Unexpected upstream device-opening code; patch not applied')
    kms.write_text(content.replace(original, patched))
    args = ['clang', '--target=aarch64-linux-gnu', '--sysroot=' + str(SYSROOT),
            '-fuse-ld=lld', '-nostdlib', '-no-pie', '-O2', '-D_GNU_SOURCE',
            '-D_FILE_OFFSET_BITS=64', '-DHAVE_SYS_SELECT_H=1', '-DHAVE_CAIRO=0',
            '-Wno-pointer-arith', '-I' + str(OUT / 'usr/include'),
            '-I' + str(OUT / 'usr/include/libdrm'), '-I' + str(src / 'tests'),
            '-I' + str(src), str(SYSROOT / 'usr/lib/crt1.o'), str(SYSROOT / 'usr/lib/crti.o')]
    args += [str(src / 'tests' / name) for name in [
        'modetest/modetest.c', 'modetest/buffers.c', 'modetest/cursor.c',
        'util/format.c', 'util/kms.c', 'util/pattern.c']]
    args += ['-L' + str(OUT / 'usr/lib'), '-L' + str(SYSROOT / 'usr/lib'),
             '-Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1', '-Wl,-rpath,/lib', '-l:libdrm.so.2',
             '-lm', '-lc', str(SYSROOT / 'usr/lib/crtn.o'), '-o', str(OUT / 'modetest-minimal')]
    result = subprocess.run(args, capture_output=True, text=True)
    (OUT / 'build-command.json').write_text(json.dumps(args, indent=2) + '\n')
    (OUT / 'build.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    subprocess.run(['qemu-aarch64-static', '-L', str(SYSROOT), '-E',
                    'LD_LIBRARY_PATH=' + str(OUT / 'usr/lib'), str(OUT / 'modetest-minimal'), '-h'],
                   check=True, capture_output=True)
    manifest = {'libdrm_version': VERSION, 'source_url': source_url,
                'source_sha256': SOURCE_SHA256,
                'modetest_sha256': hashlib.sha256((OUT / 'modetest-minimal').read_bytes()).hexdigest(),
                'cairo_enabled': False, 'package_signature_verified': True, 'arm_help_checked': True}
    manifest['local_change'] = 'Allow -D to open an absolute DRM device node directly'
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Built and checked upstream ARM64 modetest without Cairo.')


if __name__ == '__main__':
    main()
