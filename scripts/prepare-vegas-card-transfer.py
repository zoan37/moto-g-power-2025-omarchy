#!/usr/bin/env python3
"""Split the verified Arch archive into resumable, ownership-preserving tars."""
import gzip
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'artifacts/vegas-linux-bringup/card-transfer'
SIGNER = '68B3537F39A313B3E574D06777193F152BDBE6A6'


class HashedReader:
    def __init__(self, stream):
        self.stream = stream
        self.hash = hashlib.sha256()

    def read(self, size=-1):
        data = self.stream.read(size)
        self.hash.update(data)
        return data


def main():
    archive = ROOT / 'vm/ArchLinuxARM-aarch64-latest.tar.gz'
    result = subprocess.run(['gpg', '--homedir', str(ROOT / 'vm/gnupg'),
                             '--status-fd', '1', '--verify', str(archive) + '.sig',
                             str(archive)], check=True, capture_output=True, text=True)
    if f'[GNUPG:] VALIDSIG {SIGNER} ' not in result.stdout:
        raise SystemExit('Unexpected archive signer')
    OUT.mkdir(parents=True, exist_ok=True)
    chunks, files = [], []
    output = None
    total = count = 0

    def close_chunk():
        nonlocal output, total, count
        if output is None:
            return
        output.close()
        path = OUT / f'chunk-{len(chunks):03d}.tar.gz'
        chunks.append({'file': path.name, 'bytes': path.stat().st_size,
                       'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                       'unpacked_bytes': total, 'entries': count})
        output, total, count = None, 0, 0

    with tarfile.open(archive, 'r|gz') as source:
        for info in source:
            name = PurePosixPath(info.name)
            if name.is_absolute() or '..' in name.parts or any(c in info.name for c in '\n\r'):
                raise SystemExit('Unexpected archive path')
            if output is not None and total + info.size > 64 * 1024 ** 2 and count:
                close_chunk()
            if output is None:
                output = tarfile.open(OUT / f'chunk-{len(chunks):03d}.tar.gz',
                                      'w:gz', format=tarfile.PAX_FORMAT, compresslevel=1)
            stream = HashedReader(source.extractfile(info)) if info.isfile() else None
            output.addfile(info, stream)
            if stream:
                files.append(f'{stream.hash.hexdigest()}  {info.name}\n')
            total += info.size
            count += 1
    close_chunk()
    with gzip.open(OUT / 'files.sha256.gz', 'wb') as f:
        f.write(''.join(files).encode())
    # One Linux partition from 1 MiB through the end of this exact 32 GB card.
    mbr = bytearray(512)
    mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', 0x83,
                              b'\xfe\xff\xff', 2048, 62521344 - 2048)
    mbr[510:512] = b'\x55\xaa'
    (OUT / 'card-layout.mbr').write_bytes(mbr)
    manifest = {'source': archive.name, 'signer': SIGNER,
                'card_bytes': 32010928128, 'chunks': chunks,
                'regular_files': len(files), 'card_written': False}
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Prepared {len(chunks)} checked archive stages; {len(files)} regular-file hashes.')


if __name__ == '__main__':
    main()
