#!/usr/bin/env python3
"""Build a diagnostic init_boot image; never communicates with the phone."""
import hashlib
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
STOCK = ROOT / 'firmware/candidates/TMO-W1VES36H.10-12-1'
OUT = ROOT / 'artifacts/vegas-linux-bringup/diagnostic'
BUSYBOX = ROOT / 'artifacts/vegas-linux-bringup/busybox/usr/bin/busybox'


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--touch', action='store_true', help='Include checked matching Ilitek touchscreen modules')
    parser.add_argument('--desktop', action='store_true', help='Boot the verified SD desktop root with its native init')
    parser.add_argument('--runtime-seconds', type=int, choices=[180, 360], default=180, help='Bounded diagnostic runtime with the temperature guard')
    args = parser.parse_args()
    if args.desktop:
        args.touch = True
        progress = json.loads((ROOT / 'private/linux-bringup/desktop-progress.json').read_text())
        if not progress.get('verified'):
            raise SystemExit('Desktop root must pass file and executable checks before boot image creation')
    xml = ET.parse(STOCK / 'flashfile.xml')
    for name in ['boot.img', 'init_boot.img', 'vendor_boot.img', 'dtbo.img']:
        expected = next(n.attrib['MD5'] for n in xml.iter()
                        if n.attrib.get('filename') == name)
        if hashlib.md5((STOCK / name).read_bytes()).hexdigest() != expected:
            raise SystemExit(f'Stock checksum mismatch: {name}')
    run(['gpg', '--homedir', ROOT / 'vm/gnupg', '--verify',
         BUSYBOX.parents[2] / 'busybox-1.36.1-4-aarch64.pkg.tar.xz.sig',
         BUSYBOX.parents[2] / 'busybox-1.36.1-4-aarch64.pkg.tar.xz'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    OUT.mkdir(parents=True, exist_ok=True)
    staging = OUT / 'root'
    if staging.exists():
        shutil.rmtree(staging)
    (staging / 'bin').mkdir(parents=True)
    for name in ['proc', 'sys', 'dev', 'run', 'tmp', 'root']:
        (staging / name).mkdir()
    shutil.copy2(BUSYBOX, staging / 'bin/busybox')
    shutil.copy2(ROOT / 'bringup/init', staging / 'init')
    (staging / 'boot-runtime').write_text(str(args.runtime_seconds) + '\n')
    if args.desktop:
        (staging / 'boot-desktop').write_text('1\n')
        (staging / 'boot-card-cid').write_text(progress['cid'] + '\n')
    (staging / 'init').chmod(0o755)
    (staging / 'bin/sh').symlink_to('busybox')
    (staging / 'www/cgi-bin').mkdir(parents=True)
    for name in ['read-partition', 'drm-query', 'drm-pattern', 'status', 'return-fastboot', 'card-report', 'card-prepare', 'card-chunk', 'card-verify', 'storage-bench', 'super-range', 'hardware-report', 'card-exec', 'desktop-verify', 'desktop-chunk']:
        target = staging / 'www/cgi-bin' / name
        shutil.copy2(ROOT / 'bringup' / name, target)
        target.chmod(0o755)
    for name in ['card-guard', 'card-close']:
        shutil.copy2(ROOT / 'bringup' / name, staging / 'bin' / name)
        (staging / 'bin' / name).chmod(0o755)
    modules = ROOT / 'artifacts/vegas-linux-bringup/vendor_boot/root/lib/modules'
    dependencies = {}
    for line in (modules / 'modules.dep').read_text().splitlines():
        name, deps = line.split(':', 1)
        dependencies[Path(name).name] = [Path(d).name for d in deps.split()]
    ordered, visiting, done = [], set(), set()
    def visit(name):
        if name in done:
            return
        if name in visiting:
            raise SystemExit(f'Module dependency cycle: {name}')
        if not (modules / name).is_file():
            raise SystemExit(f'Missing vendor module: {name}')
        visiting.add(name)
        for dep in dependencies.get(name, []):
            visit(dep)
        visiting.remove(name)
        done.add(name)
        ordered.append(name)
    for name in (modules / 'modules.load').read_text().splitlines():
        if name.strip():
            visit(name.strip())
    # Android loads these later from vendor_dlkm; they are also present in the
    # vendor ramdisk. Without them RESTART2("bootloader") reboots Linux again.
    for name in ['reboot-mode.ko', 'syscon-reboot-mode.ko']:
        visit(name)
    if args.touch:
        extra = ROOT / 'artifacts/vegas-linux-bringup/vendor_dlkm'
        candidates = json.loads((extra / 'touch-candidates.json').read_text())
        expected = subprocess.check_output(['modinfo', '-F', 'vermagic', modules / 'mediatek-drm.ko'], text=True).strip()
        (staging / 'lib/modules').mkdir(parents=True)
        for name in ['sensors_class.ko', 'ilitek_v3_mmi.ko']:
            source = extra / 'lib/modules' / name
            record = candidates['modules'][name]
            if hashlib.sha256(source.read_bytes()).hexdigest() != record['sha256'] or record['vermagic'] != expected:
                raise SystemExit('Touch module provenance mismatch')
            for dependency in record['depends'].split(','):
                if dependency and dependency + '.ko' not in ordered:
                    raise SystemExit('Touch module dependency not already loaded: ' + dependency)
            shutil.copy2(source, staging / 'lib/modules' / name)
            ordered.append(name)
        firmware = candidates.get('firmware')
        if firmware:
            source = ROOT / 'artifacts/vegas-linux-bringup' / firmware['file']
            if hashlib.sha256(source.read_bytes()).hexdigest() != firmware['sha256']:
                raise SystemExit('Touch firmware provenance mismatch')
            (staging / 'lib/firmware').mkdir()
            shutil.copy2(source, staging / 'lib/firmware/ILITEK_FW_77600')
    (staging / 'module-order').write_text('\n'.join(ordered) + '\n')
    tools = ROOT / 'artifacts/vegas-linux-bringup/arm-tools'
    drm_tools = (tools / 'modetest-minimal').is_file()
    if drm_tools:
        shutil.copy2(tools / 'modetest-minimal', staging / 'bin/modetest')
        (staging / 'lib').mkdir(exist_ok=True)
        for name in ['libc.so.6', 'libm.so.6', 'ld-linux-aarch64.so.1']:
            shutil.copy2(ROOT / 'vm/rootfs/usr/lib' / name, staging / 'lib' / name)
        shutil.copy2(tools / 'usr/lib/libdrm.so.2', staging / 'lib/libdrm.so.2')
    card = ROOT / 'artifacts/vegas-linux-bringup/card-transfer'
    if (card / 'manifest.json').exists():
        (staging / 'lib').mkdir(exist_ok=True)
        (staging / 'usr').mkdir()
        (staging / 'usr/lib').symlink_to('../lib')
        (staging / 'etc').mkdir()
        for name in ['mke2fs', 'e2fsck']:
            shutil.copy2(ROOT / 'vm/rootfs/usr/bin' / name, staging / 'bin' / name)
        for name in ['libext2fs.so.2', 'libcom_err.so.2', 'libblkid.so.1', 'libuuid.so.1', 'libe2p.so.2', 'libc.so.6', 'ld-linux-aarch64.so.1']:
            shutil.copy2(ROOT / 'vm/rootfs/usr/lib' / name, staging / 'lib' / name)
        shutil.copy2(ROOT / 'vm/rootfs/etc/mke2fs.conf', staging / 'etc/mke2fs.conf')
        shutil.copy2(card / 'card-layout.mbr', staging / 'card-layout.mbr')
        shutil.copy2(card / 'files.sha256.gz', staging / 'card-files.sha256.gz')
    run(['clang', '--target=aarch64-linux-gnu', '-fuse-ld=lld', '-nostdlib',
         '-static', '-Wl,--build-id=none', ROOT / 'bringup/to-bootloader.S',
         '-o', staging / 'bin/to-bootloader'])
    if args.desktop:
        run(['clang', '--target=aarch64-linux-gnu', '-fuse-ld=lld', '-nostdlib',
             '-static', '-Wl,--build-id=none', ROOT / 'bringup/desktop-watchdog.S',
             '-o', staging / 'bin/desktop-watchdog'])
    run(['qemu-aarch64-static', staging / 'bin/busybox', 'sh', '-n', staging / 'init'])
    files = ['.'] + sorted(str(p.relative_to(staging)) for p in staging.rglob('*'))
    archive = run(['cpio', '--null', '-o', '--format=newc', '--owner=0:0',
                   '--reproducible'], cwd=staging,
                  input=('\0'.join(files) + '\0').encode(), capture_output=True).stdout
    (OUT / 'ramdisk.cpio').write_bytes(archive)
    with (OUT / 'ramdisk.lz4').open('wb') as f:
        run(['lz4', '-l', '-9', '-c', OUT / 'ramdisk.cpio'], stdout=f)
    image = OUT / 'init_boot.vegas-usb-shell.img'
    run(['mkbootimg', '--header_version', '4', '--ramdisk', OUT / 'ramdisk.lz4',
         '--output', image])
    if image.stat().st_size > 0x800000:
        raise SystemExit('Image exceeds the observed init_boot partition size')
    run(['unpack_bootimg', '--boot_img', image, '--out', OUT / 'check'],
        stdout=subprocess.DEVNULL)
    assert (OUT / 'check/ramdisk').read_bytes() == (OUT / 'ramdisk.lz4').read_bytes()
    manifest = {
        'model': 'XT2515-1', 'codename': 'vegas', 'build': 'W1VES36H.10-12-1',
        'image': image.name, 'image_bytes': image.stat().st_size,
        'sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
        'stock_init_boot_sha256': hashlib.sha256((STOCK / 'init_boot.img').read_bytes()).hexdigest(),
        'expected_slot': 'b', 'boot_partition_changes': ['init_boot_b'],
        'runtime_limit_seconds': args.runtime_seconds, 'battery_guard_decicelsius': 400,
        'mounts_internal_phone_storage': False, 'tested_on_phone': False,
        'card_provision_tools_included': (card / 'manifest.json').exists(),
        'desktop_installed': args.desktop,
        'desktop_overlay_sha256': hashlib.sha256((ROOT / 'artifacts/vegas-linux-bringup/desktop-overlay.tar.gz').read_bytes()).hexdigest() if args.desktop else None,
        'drm_tools_included': drm_tools,
        'touch_modules_included': args.touch,
        'boots_desktop_root': args.desktop,
        'usb_ecm_link': '192.168.77.1/30',
    }
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
