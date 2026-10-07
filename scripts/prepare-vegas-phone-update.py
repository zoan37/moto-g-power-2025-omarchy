#!/usr/bin/env python3
"""Package only the shutdown and phone UI candidates, preserving tested artifacts."""
import hashlib
import json
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'port/vegas/root'
OUT = ROOT / 'artifacts/vegas-linux-bringup/phone-polish-candidate'
FILES = [
    'usr/local/libexec/vegas/native-init', 'usr/local/libexec/vegas/power-transition',
    'usr/local/libexec/vegas/power-source-state', 'usr/local/libexec/vegas/power-boot-report',
    'usr/local/bin/vegas-power', 'usr/local/bin/omarchy-system-shutdown',
    'usr/local/bin/vegas-keyboard', 'usr/local/bin/wvkbd-mobintl',
    'usr/local/bin/vegas-toggle-keyboard', 'usr/local/bin/vegas-refresh',
    'usr/local/bin/vegas-desktop', 'home/omarchy/.config/hypr/autostart.lua',
    'home/omarchy/.config/foot/foot.ini', 'home/omarchy/.config/omarchy/shell.json',
]


def main():
    files = FILES + [p.relative_to(SOURCE).as_posix() for p in sorted(
        (SOURCE / 'home/omarchy/.config/omarchy/plugins/vegas-bar').iterdir()) if p.is_file()]
    OUT.mkdir(parents=True, exist_ok=True)
    archive = OUT / 'update.tar.gz'
    records = []
    with tarfile.open(archive, 'w:gz', format=tarfile.PAX_FORMAT) as out:
        for name in files:
            path = SOURCE / name
            assert path.is_file() and not path.is_symlink()
            content = path.read_bytes()
            info = out.gettarinfo(str(path), name)
            user = name.startswith('home/omarchy/')
            info.uid = info.gid = 1000 if user else 0
            info.uname = info.gname = 'omarchy' if user else 'root'
            info.mode = 0o755 if content.startswith((b'#!', b'\x7fELF')) else 0o644
            with path.open('rb') as stream:
                out.addfile(info, stream)
            records.append({'path': name, 'sha256': hashlib.sha256(content).hexdigest(),
                            'uid': info.uid, 'gid': info.gid, 'mode': format(info.mode, '04o')})
    manifest = {'status': 'prepared; not installed or hardware-verified',
                'tested_on_phone': False, 'archive': archive.name,
                'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
                'files': records, 'shutdown_checks': 'fake sysfs and recording backends passed',
                'keyboard_checks': 'ARM help and phone layout list passed',
                'bar_checks': 'QML parse passed; visual fit pending',
                'foot_checks': 'local config check passed; native check pending',
                'hyprland_checks': 'Lua syntax passed; live reload/configerrors pending',
                'refresh_default_mhz': 30000, 'temperature_guard_c': 40,
                'requires_native_reboot': True, 'boot_partition_changes': False}
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Prepared {len(records)} update files ({archive.stat().st_size} bytes); phone test pending.')


if __name__ == '__main__':
    main()
