#!/usr/bin/env python3
"""Build an isolated, hash-locked MT6835 r48 trial; never installs or flashes it.

The caller supplies their existing libmali binary. No vendor binary is tracked
or downloaded by this script. Copy the resulting directory to a trial prefix
on the phone and run run-probes.sh as root to validate it without DRM master.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parent.parent
BASE = ROOT / 'artifacts/vegas-linux-bringup'
SOURCE_SHA = 'd3a44c6c5b897ec5d0b756670cb0f3af2af42f12cd8dba67091e3dce9e36868f'
PATCH_SHA = 'bdf616f77db6ddeb3954e8a9117fe83dda3f6188cf4df3eda9bda19b4f81f40c'
OFFSET, BEFORE, AFTER = 0x171a444, 0x52800021, 0x52800061


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('libmali', type=Path)
    parser.add_argument('--out', type=Path, default=BASE / 'libmali-reproducible-trial')
    parser.add_argument('--cc', type=Path, default=BASE / 'aquamarine-damage/toolchain/usr/bin/aarch64-linux-gnu-gcc')
    parser.add_argument('--sysroot', type=Path, default=ROOT.parent / 'fire-hd8-omarchy/working/desktop/rootfs')
    parser.add_argument('--mesa-prefix', type=Path, default=BASE / 'mesa-kbase-build/install/opt/vegas-gpu/mesa-kbase')
    args = parser.parse_args()
    data = args.libmali.read_bytes()
    if sha(data) != SOURCE_SHA:
        parser.error('Unexpected source SHA256; refusing to patch another driver.')
    if struct.unpack_from('<I', data, OFFSET)[0] != BEFORE:
        parser.error('Unexpected product-check instruction.')
    patched = bytearray(data)
    struct.pack_into('<I', patched, OFFSET, AFTER)
    if sha(patched) != PATCH_SHA:
        parser.error('Patched SHA256 mismatch.')
    out = args.out.resolve()
    if out.exists():
        parser.error('Output already exists; choose a fresh directory.')
    if out == ROOT or ROOT not in out.parents or 'artifacts' not in out.relative_to(ROOT).parts:
        parser.error('Output must be inside this repository\'s artifacts directory.')
    (out / 'lib').mkdir(parents=True)
    (out / 'lib/libmali.so.0.48.0').write_bytes(patched)
    for name in ['libmali.so', 'libmali.so.0', 'libEGL.so', 'libEGL.so.1',
                 'libGLESv2.so', 'libGLESv2.so.2', 'libGLESv1_CM.so',
                 'libGLESv1_CM.so.1', 'libgbm.so', 'libgbm.so.1']:
        (out / 'lib' / name).symlink_to('libmali.so.0.48.0')
    sdk = args.sysroot.resolve()
    mesa = args.mesa_prefix.resolve()
    common = [str(args.cc.resolve()), '--sysroot=' + str(sdk), '-O2', '-Wall', '-Wextra',
              '-Werror', '-L' + str(sdk / 'usr/lib'), '-L' + str(mesa / 'lib'),
              '-Wl,-rpath-link=' + str(sdk / 'usr/lib'),
              '-Wl,-rpath-link=' + str(mesa / 'lib')]
    subprocess.run(common + ['-shared', '-fPIC', str(ROOT / 'bringup/libmali-jm-compat.c'),
                              '-ldl', '-pthread', '-o', str(out / 'libmali-jm-compat.so')], check=True)
    for name in ['gpu-libmali-probe', 'gpu-hypr-probe', 'gpu-fence-probe']:
        subprocess.run(common + ['-I' + str(mesa / 'include'), str(ROOT / 'bringup' / (name + '.c')),
                                  '-lEGL', '-lGLESv2', '-lgbm', '-o', str(out / name)], check=True)
    shutil.copy2(ROOT / 'bringup/run-libmali-probes.sh', out / 'run-probes.sh')
    (out / 'run-probes.sh').chmod(0o755)
    files = {str(p.relative_to(out)): sha(p.read_bytes()) for p in out.rglob('*')
             if p.is_file() and not p.is_symlink()}
    manifest = {'scope': 'isolated MT6835 r48 trial; no default-driver or boot changes',
                'source_sha256': SOURCE_SHA, 'patched_sha256': PATCH_SHA,
                'patch': {'offset': hex(OFFSET), 'before': hex(BEFORE), 'after': hex(AFTER)},
                'normalized_gpu_product': '0x09000003',
                'files': files,
                'symlinks': {str(p.relative_to(out)): str(p.readlink())
                             for p in out.rglob('*') if p.is_symlink()}}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Prepared isolated trial: ' + str(out))


if __name__ == '__main__':
    main()
