#!/usr/bin/env python3
"""Prepare a verified ARM Omarchy payload for a separate directory on the SD."""
import gzip, hashlib, json, subprocess, tarfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent
FIRE = ROOT.parent / 'fire-hd8-omarchy'
BUNDLE = FIRE / 'working/desktop/bundle'
OUT = ROOT / 'artifacts/vegas-linux-bringup/desktop-transfer'
SIGNER = '68B3537F39A313B3E574D06777193F152BDBE6A6'


class HashedReader:
    def __init__(self, stream):
        self.stream, self.hash = stream, hashlib.sha256()
    def read(self, size=-1):
        data = self.stream.read(size)
        self.hash.update(data)
        return data


def main():
    archive = BUNDLE / 'rootfs.tar.xz'
    expected = 'dbee00c0cb41f6b213c153ff4dc1c0898ba99dd0543ac13cef1601820366e2c5'
    with archive.open('rb') as source:
        assert hashlib.file_digest(source, 'sha256').hexdigest() == expected
    OUT.mkdir(parents=True, exist_ok=True)
    if (OUT / 'manifest.json').exists():
        raise SystemExit('Prepared transfer exists; refusing to change a resumable plan')
    chunks, files = [], {}
    output, total, count = None, 0, 0
    def close():
        nonlocal output, total, count
        if output is None: return
        output.close()
        p = OUT / f'chunk-{len(chunks):03d}.tar.gz'
        chunks.append({'file':p.name,'bytes':p.stat().st_size,
                       'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),
                       'unpacked_bytes':total,'entries':count})
        print(f'Prepared desktop stage {len(chunks)}', flush=True)
        output,total,count = None,0,0
    def add(info, source):
        nonlocal output,total,count
        name = PurePosixPath(info.name)
        if name.is_absolute() or '..' in name.parts or any(c in info.name for c in '\n\r'):
            raise RuntimeError('Unsafe archive path')
        if info.isdev(): return
        if output and total + info.size > 64*1024**2 and count: close()
        if output is None:
            output = tarfile.open(OUT / f'chunk-{len(chunks):03d}.tar.gz', 'w:gz',
                                  format=tarfile.PAX_FORMAT, compresslevel=1)
        stream = HashedReader(source.extractfile(info)) if info.isfile() else None
        output.addfile(info, stream)
        if stream: files[str(PurePosixPath(info.name))] = stream.hash.hexdigest()
        total += info.size
        count += 1
    with tarfile.open(archive,'r|xz') as source:
        for info in source: add(info, source)
    package_manifest = json.loads((FIRE / 'records/desktop-package-manifest.json').read_text())
    for package in package_manifest['packages']:
        path = FIRE / 'working/desktop/downloads' / package['filename']
        with path.open('rb') as stream:
            assert hashlib.file_digest(stream,'sha256').hexdigest() == package['sha256']
        sig = subprocess.run(['gpg','--homedir',str(ROOT/'vm/gnupg'),'--status-fd','1',
            '--verify',str(path)+'.sig',str(path)],check=True,capture_output=True,text=True)
        assert f'[GNUPG:] VALIDSIG {SIGNER} ' in sig.stdout
        with tarfile.open(path,'r|xz') as source:
            for info in source:
                if not info.name.startswith('.'): add(info,source)
    close()
    with gzip.open(OUT/'files.sha256.gz','wb') as stream:
        stream.write(''.join(f'{digest}  {name}\n' for name,digest in sorted(files.items())).encode())
    manifest={'source':str(archive),'source_sha256':expected,'bundle_version':'0.1.1',
              'destination_on_card':'/omarchy','chunks':chunks,'regular_files':len(files),
              'extra_packages':package_manifest['packages'],'booted':False}
    (OUT/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(f'Prepared {len(chunks)} stages and {len(files)} final file checksums.',flush=True)

if __name__ == '__main__': main()
