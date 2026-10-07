#!/usr/bin/env python3
"""Package only the reviewed phone overrides for the /omarchy SD root."""
import hashlib
import argparse
import json
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'port/vegas/root'
OUT = ROOT / 'artifacts/vegas-linux-bringup/desktop-overlay.tar.gz'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=OUT,
                        help='Use a separate archive for an untested candidate')
    output = parser.parse_args().output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, 'w:gz', format=tarfile.PAX_FORMAT) as archive:
        for path in sorted(SOURCE.rglob('*')):
            relative = path.relative_to(SOURCE)
            info = archive.gettarinfo(str(path), 'omarchy/' + relative.as_posix())
            user = relative.parts[:2] == ('home', 'omarchy')
            info.uid = info.gid = 1000 if user else 0
            info.uname = info.gname = 'omarchy' if user else 'root'
            if info.isfile():
                with path.open('rb') as stream:
                    signature = stream.read(4)
                    stream.seek(0)
                    if signature.startswith(b'#!') or signature == b'\x7fELF':
                        info.mode = 0o755
                    archive.addfile(info, stream)
            else:
                archive.addfile(info)
    print(json.dumps({'file': str(output), 'bytes': output.stat().st_size,
                      'sha256': hashlib.sha256(output.read_bytes()).hexdigest()}))


if __name__ == '__main__':
    main()
