#!/usr/bin/env python3
"""Prepare stock GPU modules and a one-boot probe; never loads or flashes them."""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parent.parent
BASE = ROOT / 'artifacts/vegas-linux-bringup'
OUT = BASE / 'gpu-stock-probe'


def field(path, name):
    return subprocess.check_output(['modinfo', '-F', name, path], text=True).strip()


def main():
    # These directories contain modules extracted from this device's original
    # vendor_boot and read-only vendor_dlkm_b, not another model's firmware.
    paths, ambiguous = {}, set()
    for directory in [BASE / 'vendor_boot/root/lib/modules', BASE / 'vendor_dlkm/lib/modules']:
        for path in directory.glob('*.ko'):
            name = field(path, 'name').replace('-', '_')
            old = paths.get(name)
            if old and old.read_bytes() != path.read_bytes():
                ambiguous.add(name)
            paths[name] = path
    expected = field(paths['mediatek_drm'], 'vermagic')
    ordered, visiting, done = [], set(), set()

    def visit(name):
        name = name.replace('-', '_')
        if name in done:
            return
        if name in visiting or name not in paths:
            raise SystemExit('Invalid dependency graph: ' + name)
        if name in ambiguous:
            raise SystemExit('Ambiguous module sources: ' + name)
        visiting.add(name)
        path = paths[name]
        if field(path, 'vermagic') != expected:
            raise SystemExit('Module ABI differs from proven display modules: ' + name)
        for dep in field(path, 'depends').split(','):
            if dep:
                visit(dep)
        visiting.remove(name)
        done.add(name)
        ordered.append((name, path))

    # fhctl is a device-tree supplier of gpufreq, although no direct symbol
    # dependency exposes that edge in modinfo.
    for target in ['fhctl', 'mtk_gpufreq_mt6835', 'mali_mgm_mt6835',
                   'mali_prot_alloc_mt6835', 'mali_kbase_mt6835']:
        visit(target)
    OUT.mkdir(parents=True, exist_ok=True)
    records = []
    archive = OUT / 'modules.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        for name, path in ordered:
            data = path.read_bytes()
            info = tarfile.TarInfo(path.name)
            info.size = len(data)
            info.mode = 0o600
            tar.addfile(info, io.BytesIO(data))
            records.append({'name': name, 'file': path.name,
                            'source': str(path.relative_to(ROOT)),
                            'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                            'depends': field(path, 'depends')})
    manifest = {'vermagic': expected, 'archive': archive.name,
                'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
                'bytes': archive.stat().st_size, 'modules': records,
                'scope': 'one-boot stock module probe; no automatic loading or boot-image changes'}
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Checked {len(records)} dependency-ordered original modules; archive {archive.stat().st_size} bytes.')


if __name__ == '__main__':
    main()
