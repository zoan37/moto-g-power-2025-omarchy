#!/usr/bin/env python3
"""Cross-build the native Ilitek-to-Hyprland pointer bridge."""
import hashlib
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'artifacts/vegas-linux-bringup/touch-pointer'
SYSROOT = ROOT.parent / 'fire-hd8-omarchy/working/desktop/rootfs'
XML = ROOT / 'vendor/protocols/wlr-virtual-pointer-unstable-v1.xml'
EXPECTED = '3ff6d540be0bc5228195bf072bde42117ea17945a5c2061add5d3cf97d6bb524'


def main():
    if hashlib.sha256(XML.read_bytes()).hexdigest() != EXPECTED:
        raise SystemExit('Reviewed virtual-pointer protocol changed')
    BUILD.mkdir(parents=True, exist_ok=True)
    header = BUILD / 'virtual-pointer-client.h'
    protocol = BUILD / 'virtual-pointer-client.c'
    subprocess.run(['wayland-scanner', 'client-header', str(XML), str(header)], check=True)
    subprocess.run(['wayland-scanner', 'private-code', str(XML), str(protocol)], check=True)
    for source, relative in [('touch-pointer.c', 'usr/local/bin/vegas-touch-pointer'),
                             ('virtual-click.c', 'usr/local/libexec/vegas/virtual-click'),
                             ('touch-inject.c', 'usr/local/libexec/vegas/touch-inject')]:
        output = ROOT / 'port/vegas/root' / relative
        temporary = output.with_suffix('.new')
        subprocess.run(['clang', '--target=aarch64-linux-gnu', '--sysroot=' + str(SYSROOT),
            '-fuse-ld=lld', '-nostdlib', '-no-pie', '-O2', '-I' + str(BUILD),
            str(SYSROOT / 'usr/lib/crt1.o'), str(SYSROOT / 'usr/lib/crti.o'),
            str(ROOT / 'bringup' / source), str(protocol),
            '-L' + str(SYSROOT / 'usr/lib'), '-lwayland-client', '-lc',
            str(SYSROOT / 'usr/lib/crtn.o'), '-Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1',
            '-o', str(temporary)], check=True)
        temporary.replace(output)
    print('ARM touch bridge built from the checked protocol and local ARM sysroot.')


if __name__ == '__main__':
    main()
