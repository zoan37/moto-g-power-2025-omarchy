#!/usr/bin/env python3
"""One bounded init_boot_b experiment, followed by exact stock restoration."""
import glob
import argparse
import base64
import datetime
import concurrent.futures
import gzip
import ipaddress
import hashlib
import json
import os
from pathlib import Path
import select
import re
import subprocess
import termios
import time
import zlib
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'artifacts/vegas-linux-bringup/diagnostic'
LOG = ROOT / 'private/linux-bringup' / ('diagnostic-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
STOCK = ROOT / 'backups/vegas-W1VES36H.10-12-1/init_boot.stock.img'
BACKUPS = False
DISPLAY_PATTERN = False
USB_NETWORK = False
PROVISION_CARD = False
DEPLOY_DESKTOP = False
CARD_PENDING = False
STORAGE_BENCH = False
SUPER_METADATA = False
VENDOR_DRIVERS = False
VENDOR_FIRMWARE = False
WEBCAM_SER8 = False
CARD_SCRIPT = None
CARD_OVERLAY = None
NATIVE_DESKTOP = False
KEEP_DESKTOP = False
NATIVE_SCRIPT = None
RUNTIME_LIMIT = 180


def native_desktop_probe(opener, base):
    script = b'''export PATH=/opt/omarchy-android/hyprland/bin:/usr/local/bin:/usr/bin
echo NATIVE_DESKTOP_INSPECTION
cat /proc/1/comm
findmnt -n -o SOURCE,FSTYPE /
runuser -u omarchy -- env XDG_RUNTIME_DIR=/run/user/1000 LD_LIBRARY_PATH=/opt/omarchy-android/aquamarine/lib hyprctl -i 0 -j monitors
echo CONFIG_RELOAD
runuser -u omarchy -- env XDG_RUNTIME_DIR=/run/user/1000 LD_LIBRARY_PATH=/opt/omarchy-android/aquamarine/lib hyprctl -i 0 reload
echo CONFIG_ERRORS_BEGIN
runuser -u omarchy -- env XDG_RUNTIME_DIR=/run/user/1000 LD_LIBRARY_PATH=/opt/omarchy-android/aquamarine/lib hyprctl -i 0 configerrors
echo CONFIG_ERRORS_END
runuser -u omarchy -- env XDG_RUNTIME_DIR=/run/user/1000 LD_LIBRARY_PATH=/opt/omarchy-android/aquamarine/lib hyprctl -i 0 layers
runuser -u omarchy -- env XDG_RUNTIME_DIR=/run/user/1000 LD_LIBRARY_PATH=/opt/omarchy-android/aquamarine/lib hyprctl -i 0 clients
tail -n 70 /home/omarchy/.local/state/vegas/weston.log
tail -n 40 /var/log/vegas/desktop.log
tail -n 12 /home/omarchy/.local/state/vegas/touch.log
tail -n 25 /var/log/vegas/init.log
tail -n 20 /var/log/vegas/seatd.log
'''
    if NATIVE_SCRIPT:
        script = NATIVE_SCRIPT.read_bytes()
    deadline = time.monotonic() + 65
    while True:
        request = urllib.request.Request(base + 'command', data=script, method='POST')
        with opener.open(request, timeout=30) as response:
            result = response.read()
        (LOG / 'native-desktop.log').write_bytes(result)
        errors = re.search(rb'CONFIG_ERRORS_BEGIN\s*\n(.*?)CONFIG_ERRORS_END', result, re.S)
        clean_config = errors is not None and not errors[1].strip()
        if NATIVE_SCRIPT or (re.search(rb'"width"\s*:\s*1080', result)
                and re.search(rb'"height"\s*:\s*2388', result)
                and re.search(rb'a:\s*1,\s*namespace:\s*omarchy-bar', result)
                and b'namespace: wvkbd' in result and b'class: foot' in result
                and b'Ilitek Wayland pointer ready:' in result and clean_config):
            break
        if time.monotonic() > deadline:
            print('Native desktop startup incomplete; private logs saved.', flush=True)
            if WEBCAM_SER8:
                capture_webcam_pattern()
            return False
        time.sleep(3)
    print('Native Arch desktop inspection completed.', flush=True)
    capture_native_screenshot(opener, base)
    if WEBCAM_SER8:
        capture_webcam_pattern()
    if KEEP_DESKTOP:
        with opener.open(urllib.request.Request(base + 'command',
                data=b'touch /etc/vegas-persistent-boot; sync\n', method='POST'), timeout=5) as response:
            if b'NATIVE_COMMAND_STATUS:0' not in response.read():
                raise RuntimeError('Persistent desktop marker failed')
    return True


def capture_native_screenshot(opener, base):
    script = b'''runuser -u omarchy -- env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 timeout 8 grim /run/user/1000/vegas-desktop.png
if test -s /run/user/1000/vegas-desktop.png; then
echo PNG_BEGIN
base64 /run/user/1000/vegas-desktop.png
echo PNG_END
fi
'''
    try:
        with opener.open(urllib.request.Request(base + 'command', data=script, method='POST'), timeout=15) as response:
            result = response.read()
        match = re.search(rb'PNG_BEGIN\n(.*?)PNG_END', result, re.S)
        if match:
            png = base64.b64decode(match[1])
            if png.startswith(b'\x89PNG\r\n\x1a\n'):
                (LOG / 'native-desktop.png').write_bytes(png)
                print('Native compositor screenshot captured privately.', flush=True)
                return
        (LOG / 'screenshot-error.log').write_bytes(result[-4096:])
    except Exception as error:
        (LOG / 'screenshot-error.log').write_text(str(error))


def capture_webcam_pattern():
    """Two private stills from the user's phone-facing SER8 camera; no audio."""
    for index in range(2):
        time.sleep(2 if index == 0 else 1)
        try:
            result = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
                'ser8', 'timeout 7 ffmpeg -hide_banner -loglevel error -f v4l2 '
                '-i /dev/video0 -frames:v 1 -f image2pipe -c:v mjpeg -'],
                capture_output=True, timeout=10)
            (LOG / f'webcam-{index}.log').write_bytes(result.stderr)
            if result.returncode == 0 and result.stdout.startswith(b'\xff\xd8'):
                (LOG / f'webcam-{index}.jpg').write_bytes(result.stdout)
                print('Private webcam still captured during display test.', flush=True)
        except subprocess.TimeoutExpired:
            print('Webcam capture timed out; phone restoration will continue.', flush=True)


def provision_card(opener, base, report):
    """Prepare only the identified removable SD; resume checked tar stages."""
    global CARD_PENDING
    transfer = ROOT / ('artifacts/vegas-linux-bringup/desktop-transfer' if DEPLOY_DESKTOP else 'artifacts/vegas-linux-bringup/card-transfer')
    manifest_path = transfer / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    match = re.search(rb'^CARD_CID:([0-9a-f]{32})$', report, re.M)
    if not match or b'CARD_TYPE:SD' not in report or b'\n32010928128\n' not in report:
        raise RuntimeError('The expected 32 GB removable SD was not identified')
    cid = match[1].decode()
    progress_path = ROOT / ('private/linux-bringup/desktop-progress.json' if DEPLOY_DESKTOP else 'private/linux-bringup/card-progress.json')
    plan_hash = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
    progress = json.loads(progress_path.read_text()) if progress_path.exists() else {
        'cid': cid, 'plan_sha256': plan_hash, 'prepared': DEPLOY_DESKTOP, 'done': [], 'verified': False}
    if progress['cid'] != cid or progress['plan_sha256'] != plan_hash:
        raise RuntimeError('Card identity or prepared archive plan changed; no card write attempted')

    def save():
        next_path = progress_path.with_suffix('.next')
        next_path.write_text(json.dumps(progress, indent=2) + '\n')
        next_path.replace(progress_path)

    def uptime():
        with opener.open(base + 'status', timeout=5) as response:
            status = response.read()
        match = re.search(rb'^UPTIME:([0-9.]+)$', status, re.M)
        if not match:
            raise RuntimeError('Native boot duration unavailable')
        return float(match[1])

    save()
    if not progress['prepared']:
        request = urllib.request.Request(base + 'card-prepare?' + cid, data=b'', method='POST')
        with opener.open(request, timeout=100) as response:
            result = response.read()
        (LOG / 'card-prepare.log').write_bytes(result)
        if b'CARD_PREPARED' not in result or b'CARD_ERROR:' in result:
            raise RuntimeError('Card preparation failed; details saved privately')
        progress['prepared'] = True
        save()
        print('Confirmed Samsung microSD repartitioned and formatted as vegas-arch.', flush=True)
    for index, chunk in enumerate(manifest['chunks']):
        if index in progress['done']:
            continue
        reserve = max(140, chunk.get('unpacked_bytes', 0) / (2 * 1024 ** 2) + 30) if DEPLOY_DESKTOP else 70
        if uptime() > min(RUNTIME_LIMIT - 70, RUNTIME_LIMIT - 5 - reserve):
            CARD_PENDING = True
            return True
        data = (transfer / chunk['file']).read_bytes()
        if hashlib.sha256(data).hexdigest() != chunk['sha256']:
            raise RuntimeError('Prepared transfer checksum changed')
        endpoint = 'desktop-chunk' if DEPLOY_DESKTOP else 'card-chunk'
        request = urllib.request.Request(base + endpoint + '?' + cid + ':' + chunk['sha256'],
                                         data=data, method='POST')
        with opener.open(request, timeout=130 if DEPLOY_DESKTOP else 65) as response:
            result = response.read()
        (LOG / f'card-chunk-{index:03d}.log').write_bytes(result)
        if ('CARD_CHUNK_OK:' + chunk['sha256']).encode() not in result or b'CARD_ERROR:' in result:
            raise RuntimeError(f'Card extraction failed at stage {index}; details saved privately')
        progress['done'].append(index)
        save()
        print(f'Arch card archive stage {len(progress["done"])}/{len(manifest["chunks"])} checked and synced.', flush=True)
    if not progress['verified']:
        verification_budget = 210 if DEPLOY_DESKTOP else 90
        if uptime() > RUNTIME_LIMIT - verification_budget:
            CARD_PENDING = True
            return True
        endpoint = 'desktop-verify' if DEPLOY_DESKTOP else 'card-verify'
        request = base + endpoint + '?' + cid
        if DEPLOY_DESKTOP:
            checksums = (transfer / 'files.sha256.gz').read_bytes()
            request = urllib.request.Request(request + ':' + hashlib.sha256(checksums).hexdigest(),
                                             data=checksums, method='POST')
        with opener.open(request, timeout=200 if DEPLOY_DESKTOP else 85) as response:
            result = response.read()
        (LOG / 'card-verify.log').write_bytes(result)
        if b'CARD_VERIFIED' not in result or b'CARD_ERROR:' in result:
            raise RuntimeError('Installed Arch file/filesystem/userspace verification failed; details saved privately')
        progress['verified'] = True
        save()
        print('All installed Arch files verified; native ARM shell and filesystem check passed.', flush=True)
    CARD_PENDING = False
    return True


def network_probe():
    global CARD_PENDING
    keep_open = False
    interface = None
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        for p in Path('/sys/class/net').iterdir():
            if (p / 'address').read_text().strip() == '02:00:00:00:77:02':
                interface = p.name
                break
        if interface:
            break
        time.sleep(0.5)
    if not interface:
        raise RuntimeError('USB ECM interface not found')
    profile = 'vegas-linux-' + LOG.name
    created = False
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    base = 'http://192.168.77.1:8080/cgi-bin/'
    try:
        # In-memory, interface-specific profile: no default route or DNS change.
        command(['nmcli', 'connection', 'add', 'save', 'no', 'type', 'ethernet',
                 'ifname', interface, 'con-name', profile, 'autoconnect', 'no',
                 'ipv4.method', 'manual', 'ipv4.addresses', '192.168.77.2/30',
                 'ipv4.never-default', 'yes', 'ipv4.ignore-auto-dns', 'yes',
                 'ipv6.method', 'disabled'])
        created = True
        command(['nmcli', '-w', '12', 'connection', 'up', profile, 'ifname', interface], timeout=16)
        status_deadline = time.monotonic() + (40 if NATIVE_DESKTOP else 10)
        while True:
            try:
                with opener.open(base + 'status', timeout=5) as response:
                    status = response.read()
                if not NATIVE_DESKTOP or b'NATIVE_PID1:' in status:
                    break
            except (OSError, urllib.error.URLError):
                if time.monotonic() >= status_deadline:
                    raise
            if time.monotonic() >= status_deadline:
                raise RuntimeError('Native Arch root did not replace the RAM diagnostic server')
            time.sleep(1)
        (LOG / 'network-status.log').write_bytes(status)
        if b'uid=0' not in status or b'5.15.180-android13' not in status:
            raise RuntimeError('Network endpoint did not report the expected native root kernel')
        print('Native Linux USB Ethernet link verified.', flush=True)
        if NATIVE_DESKTOP:
            result = native_desktop_probe(opener, base)
            keep_open = result and KEEP_DESKTOP
            return result
        with opener.open(base + 'card-report', timeout=10) as response:
            card = response.read()
        (LOG / 'card-report.log').write_bytes(card)
        print('Read-only native microSD report saved.', flush=True)
        with opener.open(base + 'hardware-report', timeout=12) as response:
            hardware = response.read()
        (LOG / 'hardware-report.log').write_bytes(hardware)
        print('Input devices and matching module status saved privately.', flush=True)
        if CARD_OVERLAY:
            cid = re.search(rb'^CARD_CID:([0-9a-f]{32})$', card, re.M)[1].decode()
            data = CARD_OVERLAY.read_bytes()
            request = urllib.request.Request(base + 'card-chunk?' + cid + ':' + hashlib.sha256(data).hexdigest(),
                                             data=data, method='POST')
            with opener.open(request, timeout=30) as response:
                result = response.read()
            (LOG / 'card-overlay.log').write_bytes(result)
            if b'CARD_CHUNK_OK:' not in result:
                raise RuntimeError('Phone configuration overlay failed')
            print('Checked phone configuration overlay installed.',flush=True)
        if CARD_SCRIPT:
            cid = re.search(rb'^CARD_CID:([0-9a-f]{32})$', card, re.M)[1].decode()
            script = CARD_SCRIPT.read_bytes()
            if not 0 < len(script) <= 131072:
                raise RuntimeError('Development script exceeds endpoint limit')
            request = urllib.request.Request(base + 'card-exec?' + cid, data=script, method='POST')
            with opener.open(request, timeout=120) as response:
                result = response.read()
            (LOG / 'card-command.log').write_bytes(result)
            if b'CARD_COMMAND_STATUS:0' not in result:
                raise RuntimeError('Native Arch command failed; output saved privately')
            print('Native Arch development command completed.', flush=True)
        if PROVISION_CARD or DEPLOY_DESKTOP:
            return provision_card(opener, base, card)
        if STORAGE_BENCH:
            with opener.open(base + 'storage-bench', timeout=65) as response:
                bench = response.read()
            (LOG / 'storage-bench.log').write_bytes(bench)
            for section in bench.split(b'BENCH:')[1:]:
                kind = section.splitlines()[0]
                match = re.search(rb'copied,\s+([0-9.]+) seconds', section)
                if not match:
                    continue
                elapsed = float(match[1])
                if elapsed > 0:
                    print(f'{kind.decode()} direct sequential read: {64 / elapsed:.1f} MiB/s (64 MiB / {elapsed:.2f}s).', flush=True)
        if SUPER_METADATA or VENDOR_DRIVERS or VENDOR_FIRMWARE:
            with opener.open(base + 'super-range?0:4096', timeout=15) as response:
                total = int(response.headers['X-Super-Bytes'])
                head = response.read()
            if len(head) != 2 * 1024 ** 2 or not len(head) < total <= 64 * 1024 ** 3:
                raise RuntimeError('Unexpected firmware metadata size')
            destination = ROOT / 'artifacts/vegas-linux-bringup/super'
            destination.mkdir(exist_ok=True)
            (destination / 'metadata-head.img').write_bytes(head)
            (destination / 'metadata-source.json').write_text(json.dumps({
                'super_bytes': total, 'head_bytes': len(head),
                'head_sha256': hashlib.sha256(head).hexdigest(), 'attempt': LOG.name}, indent=2) + '\n')
            print('Read-only super metadata captured for matching vendor drivers.', flush=True)
            if VENDOR_DRIVERS or VENDOR_FIRMWARE:
                # liblp verifies the header/table checksums before we trust an extent.
                table = subprocess.run(['lpdump', '-s', '1', str(destination / 'metadata-head.img')],
                                       check=True, capture_output=True, text=True).stdout
                name = 'vendor_b' if VENDOR_FIRMWARE else 'vendor_dlkm_b'
                part = re.search(r'Name: ' + name + r'\n(.*?)(?=------------------------)', table, re.S)
                extents = re.findall(r'^\s+(\d+) \.\. (\d+) linear super (\d+)\s*$', part[1], re.M) if part else []
                if not extents or len(extents) != len(re.findall(r'linear super', part[1])):
                    raise RuntimeError('Expected checked linear firmware extents')
                sectors = 0
                for begin, end, start in extents:
                    begin, end, start = int(begin), int(end), int(start)
                    if begin != sectors or end < begin or (start + end - begin + 1)*512 > total:
                        raise RuntimeError('Firmware extent outside bounds')
                    sectors = end + 1
                if not 0 < sectors * 512 <= 1024 * 1024 ** 2:
                    raise RuntimeError('Firmware partition exceeds bound')
                image = destination / (name + '.img')
                partial = image.with_suffix('.partial')
                plan_path = destination / (name + '-read-plan.json')
                plan = {'metadata_sha256':hashlib.sha256(head).hexdigest(),'extents':extents}
                if partial.exists() and (not plan_path.exists() or json.loads(plan_path.read_text()) != plan):
                    raise RuntimeError('Firmware read plan changed')
                plan_path.write_text(json.dumps(plan) + '\n')
                completed = partial.stat().st_size // 512 if partial.exists() else 0
                if partial.exists() and partial.stat().st_size % 512:
                    raise RuntimeError('Incomplete firmware read checkpoint')
                with partial.open('ab') as output:
                    for begin, end, start in extents:
                        begin, end, start = int(begin), int(end), int(start)
                        offset = max(begin, completed)
                        while offset <= end:
                            with opener.open(base + 'status', timeout=5) as response:
                                uptime = re.search(rb'^UPTIME:([0-9.]+)$', response.read(), re.M)
                            if not uptime or float(uptime[1]) > 105:
                                output.flush(); os.fsync(output.fileno())
                                CARD_PENDING = True
                                return True
                            count = min(16384, end - offset + 1)
                            with opener.open(base + f'super-range?{start + offset - begin}:{count}', timeout=15) as response:
                                chunk = response.read()
                                if int(response.headers['X-Super-Bytes']) != total or len(chunk) != count * 512:
                                    raise RuntimeError('Incomplete firmware read')
                            output.write(chunk); output.flush()
                            offset += count
                        print(f'Read-only {name} extent saved ({offset*512} bytes).', flush=True)
                with partial.open('rb') as source:
                    digest = hashlib.file_digest(source, 'sha256')
                partial.replace(image)
                (destination / 'vendor-driver-source.json').write_text(json.dumps({
                    'partition': name, 'slot': 1, 'extents': extents,
                    'sectors': sectors, 'sha256': digest.hexdigest(), 'attempt': LOG.name,
                    'snapshot_overlay_applied': False}, indent=2) + '\n')
                CARD_PENDING = False
                print(f'Read-only firmware partition captured ({sectors * 512} bytes).', flush=True)
        if BACKUPS:
            destination = ROOT / 'backups/vegas-device-readbacks'
            destination.mkdir(mode=0o700, exist_ok=True)
            manifest_path = destination / 'manifest.json'
            for name in ['nvram', 'nvcfg', 'nvdata', 'persist', 'prodpersist',
                         'protect1', 'protect2', 'proinfo', 'cid', 'utags', 'utagsBackup',
                         'lk_b', 'boot_b', 'vendor_boot_b', 'dtbo_b', 'vbmeta_b', 'vbmeta_system_b']:
                records = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
                if name in records:
                    saved = destination / records[name]['file']
                    if saved.is_file() and hashlib.sha256(saved.read_bytes()).hexdigest() == records[name]['gzip_sha256']:
                        continue
                filename = name + '.img.gz'
                partial = destination / (filename + '.partial')
                raw_hash, packed_hash = hashlib.sha256(), hashlib.sha256()
                raw_size = packed_size = 0
                decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
                with opener.open(base + 'read-partition?' + name, timeout=25) as response, partial.open('wb') as f:
                    expected_size = int(response.headers['X-Raw-Bytes'])
                    expected_hash = response.headers['X-Raw-SHA256']
                    if response.headers['X-Partition'] != name or not 0 < expected_size <= 128 * 1024 * 1024:
                        raise RuntimeError('Unexpected partition metadata')
                    while chunk := response.read(1024 * 1024):
                        packed_size += len(chunk)
                        if packed_size > 130 * 1024 * 1024:
                            raise RuntimeError('Compressed partition exceeds bound')
                        f.write(chunk)
                        packed_hash.update(chunk)
                        raw = decoder.decompress(chunk)
                        raw_size += len(raw)
                        if raw_size > expected_size:
                            raise RuntimeError('Decoded partition exceeds expected size')
                        raw_hash.update(raw)
                if not decoder.eof or raw_size != expected_size or raw_hash.hexdigest() != expected_hash:
                    raise RuntimeError(f'Network backup failed integrity check: {name}')
                partial.replace(destination / filename)
                records[name] = {'partition': name, 'raw_bytes': raw_size,
                                 'raw_sha256': raw_hash.hexdigest(), 'gzip_sha256': packed_hash.hexdigest(),
                                 'file': filename, 'source': 'Native Linux over isolated USB ECM', 'attempt': LOG.name}
                manifest_path.write_text(json.dumps(records, indent=2) + '\n')
                print(f'Verified USB network backup: {name} ({raw_size} bytes)', flush=True)
        with opener.open(base + 'drm-query', timeout=16) as response:
            query = response.read()
        (LOG / 'drm-network-query.log').write_bytes(query)
        print(f'Native DRM query received: {len(query)} bytes.', flush=True)
        if DISPLAY_PATTERN and b'DSI-1' in query:
            print('Starting an eight-second DRM pattern over USB networking.', flush=True)
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                if WEBCAM_SER8:
                    pool.submit(capture_webcam_pattern)
                with opener.open(base + 'drm-pattern', timeout=16) as response:
                    pattern = response.read()
            (LOG / 'drm-network-pattern.log').write_bytes(pattern)
            print('DRM pattern result saved.', flush=True)
        return True
    finally:
        if created and not keep_open:
            try:
                with opener.open(base + 'return-fastboot', timeout=3) as response:
                    response.read()
            except Exception:
                pass
            command(['nmcli', 'connection', 'delete', profile], timeout=10)


def read_reply(fd, marker, seconds):
    data = bytearray()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if select.select([fd], [], [], 0.25)[0]:
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            data.extend(chunk)
            if marker in data.replace(b'\r', b''):
                break
    return bytes(data)


def drm_probe(fd, render=False):
    os.write(fd, b"stty -echo; printf 'DRM_%s\\n' BEGIN; timeout 12 /bin/modetest -D /dev/dri/card0 -c -p; printf 'DRM_%s\\n' DONE\n")
    termios.tcdrain(fd)
    reply = read_reply(fd, b'DRM_DONE\n', 16)
    (LOG / 'drm-query.log').write_bytes(reply)
    text = reply.decode(errors='replace').replace('\r', '')
    match = re.search(r'^\s*(\d+)\s+\d+\s+connected\s+DSI-1\b', text, re.M)
    print(f'DRM tool response: {len(reply)} bytes; DSI connector identified={bool(match)}', flush=True)
    if render and match:
        script = (
            "printf 'PATTERN_%s\\n' BEGIN; sh /www/cgi-bin/drm-pattern >/run/drm-pattern.log 2>&1; "
            "echo PATTERN_STATUS:$?; cat /run/drm-pattern.log; printf 'PATTERN_%s\\n' DONE\n"
        )
        print('Starting an eight-second DRM test pattern.', flush=True)
        os.write(fd, script.encode())
        termios.tcdrain(fd)
        reply = read_reply(fd, b'PATTERN_DONE\n', 16)
        (LOG / 'drm-pattern.log').write_bytes(reply)
        print('DRM pattern response saved.', flush=True)


def backup_partition(fd, name, node):
    """Transfer a read-only gzip/base64 stream, then verify its raw SHA-256."""
    script = (
        f"stty -echo; if grep -q '^PARTNAME={name}$' /sys/class/block/{node}/uevent; then "
        f"printf 'PART_BEGIN_%s\\n' {name}; blockdev --getsize64 /dev/{node}; "
        f"sha256sum /dev/{node}; gzip -c /dev/{node} | base64 -w 1024 | "
        "while read -r line; do printf '%s\\n' \"$line\"; usleep 2000; done; "
        f"printf 'PART_END_%s\\n' {name}; fi\n"
    )
    os.write(fd, script.encode())
    termios.tcdrain(fd)
    data = bytearray()
    deadline = time.monotonic() + 50
    end = f'PART_END_{name}\n'.encode()
    while time.monotonic() < deadline:
        if select.select([fd], [], [], 0.25)[0]:
            chunk = os.read(fd, 65536)
            if not chunk:
                raise RuntimeError('USB disconnected during backup')
            data.extend(chunk)
            if len(data) > 160 * 1024 * 1024:
                raise RuntimeError('Backup stream exceeded bound')
            if end in data.replace(b'\r', b''):
                break
    normalized = bytes(data).replace(b'\r', b'')
    start = f'PART_BEGIN_{name}\n'.encode()
    if start not in normalized or end not in normalized:
        raise RuntimeError(f'Incomplete backup stream: {name}')
    body = normalized.split(start, 1)[1].split(end, 1)[0]
    lines = body.splitlines()
    size = int(lines[0])
    expected, source = lines[1].decode().split()
    if source != f'/dev/{node}' or not re.fullmatch('[0-9a-f]{64}', expected):
        raise RuntimeError('Invalid source digest')
    if not 0 < size <= 128 * 1024 * 1024:
        raise RuntimeError('Unexpected partition size')
    packed = base64.b64decode(b''.join(lines[2:]), validate=True)
    (LOG / (name + '.transfer.gz')).write_bytes(packed)
    raw = gzip.decompress(packed)
    actual = hashlib.sha256(raw).hexdigest()
    if len(raw) != size or actual != expected:
        raise RuntimeError(f'Backup checksum/size mismatch: {name}')
    destination = ROOT / 'backups/vegas-device-readbacks'
    destination.mkdir(mode=0o700, exist_ok=True)
    filename = name + '.img.gz'
    (destination / filename).write_bytes(packed)
    record = {'partition': name, 'node': node, 'raw_bytes': size, 'raw_sha256': actual,
              'gzip_sha256': hashlib.sha256(packed).hexdigest(), 'file': filename,
              'source': 'Native Linux root shell, Android not running', 'attempt': LOG.name}
    manifest = destination / 'manifest.json'
    records = json.loads(manifest.read_text()) if manifest.exists() else {}
    records[name] = record
    manifest.write_text(json.dumps(records, indent=2) + '\n')
    print(f'Verified native partition backup: {name} ({size} bytes)', flush=True)


def command(args, timeout=30):
    result = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    with (LOG / 'commands.jsonl').open('a') as f:
        f.write(json.dumps({'time': time.time(), 'args': args, 'returncode': result.returncode,
                            'stdout': result.stdout, 'stderr': result.stderr}) + '\n')
    if result.returncode:
        raise RuntimeError(result.stderr)
    return result.stdout + result.stderr


def devices(tool):
    text = command([tool, 'devices'], timeout=5)
    return [line.split()[0] for line in text.splitlines()
            if '\t' in line and (tool == 'fastboot' or line.endswith('\tdevice'))]


def serial_probe(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        settings = termios.tcgetattr(fd)
        settings[0] = settings[1] = settings[3] = 0
        settings[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        settings[4] = settings[5] = termios.B115200
        settings[6][termios.VMIN] = 0
        settings[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, settings)
        probe = (
            "\necho VEGAS_USB_SHELL_PROBE; id; uname -a; cat /proc/cmdline; "
            "cat /run/modules.log; cat /proc/modules; cat /proc/partitions; "
            "ls /sys/class/udc; ls /sys/class/drm; ls /sys/class/input; "
            "cat /sys/class/power_supply/battery/temp; "
            "for p in /sys/class/block/*; do echo BLOCK:$p; cat $p/uevent; done; "
            "dmesg; printf 'VEGAS_USB_%s\\n' SHELL_PROBE_DONE\n"
        )
        os.write(fd, probe.encode())
        data = bytearray()
        end = time.monotonic() + 35
        while time.monotonic() < end:
            if select.select([fd], [], [], 0.5)[0]:
                chunk = os.read(fd, 65536)
                if not chunk:
                    break
                data.extend(chunk)
                if b'VEGAS_USB_SHELL_PROBE_DONE\n' in data.replace(b'\r', b''):
                    break
        (LOG / 'usb-shell.log').write_bytes(data)
        verified = b'uid=0' in data and b'Linux' in data and b'VEGAS_USB_SHELL_PROBE_DONE\n' in data.replace(b'\r', b'')
        print(f'USB shell response: {len(data)} bytes; native root shell verified={verified}', flush=True)
        if verified and BACKUPS and not USB_NETWORK:
            # Nodes were mapped from this unit's actual first native GPT survey.
            # Each is checked against the live PARTNAME before any read.
            for name, node in [('nvram', 'sdc5'), ('nvcfg', 'sdc6'), ('nvdata', 'sdc7'),
                               ('persist', 'sdc24'), ('prodpersist', 'sdc25'),
                               ('protect1', 'sdc31'), ('protect2', 'sdc32'),
                               ('proinfo', 'sdc20'), ('cid', 'sdc8'),
                               ('utags', 'sdc9'), ('utagsBackup', 'sdc10')]:
                try:
                    backup_partition(fd, name, node)
                except (OSError, ValueError, RuntimeError, EOFError, zlib.error) as error:
                    (LOG / 'backup-error.txt').write_text(str(error))
                    print(f'Backup stopped: {error}; continuing toward stock restoration.', flush=True)
                    # Stop the foreground PTY pipeline before issuing another
                    # command; otherwise backup output contaminates its reply.
                    os.write(fd, b'\x03')
                    termios.tcdrain(fd)
                    read_reply(fd, b'vegas-linux# ', 2)
                    break
        if verified and not USB_NETWORK:
            drm_probe(fd, render=DISPLAY_PATTERN)
        if verified and not USB_NETWORK:
            os.write(fd, b'/bin/to-bootloader\n')
            termios.tcdrain(fd)
            time.sleep(0.5)
        return verified
    finally:
        os.close(fd)


def main():
    global BACKUPS, DISPLAY_PATTERN, USB_NETWORK, PROVISION_CARD, DEPLOY_DESKTOP, CARD_PENDING, STORAGE_BENCH, SUPER_METADATA, VENDOR_DRIVERS, VENDOR_FIRMWARE, WEBCAM_SER8, CARD_SCRIPT, CARD_OVERLAY, NATIVE_DESKTOP, KEEP_DESKTOP, NATIVE_SCRIPT, RUNTIME_LIMIT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backups', action='store_true', help='Save verified device-specific partition readbacks')
    parser.add_argument('--display-pattern', action='store_true', help='Try an eight-second DRM pattern on the identified DSI connector')
    parser.add_argument('--usb-network', action='store_true', help='Use a temporary isolated USB Ethernet link for backups and DRM')
    parser.add_argument('--provision-card', action='store_true', help='Replace the authorized 32 GB Samsung SD with checked Arch archive stages')
    parser.add_argument('--deploy-desktop', action='store_true', help='Deploy checked Omarchy userspace into a separate SD directory')
    parser.add_argument('--storage-bench', action='store_true', help='Read-only 64 MiB direct sequential comparison')
    parser.add_argument('--super-metadata', action='store_true', help='Capture read-only firmware logical-partition metadata')
    parser.add_argument('--vendor-drivers', action='store_true', help='Read checked vendor_dlkm extent for driver analysis')
    parser.add_argument('--vendor-firmware', action='store_true', help='Read checked vendor partition extents for touchscreen firmware')
    parser.add_argument('--webcam-ser8', action='store_true', help='Capture two private SER8 webcam stills during the display pattern')
    parser.add_argument('--card-script', type=Path, help='Run a local development script in native Arch on the identified SD')
    parser.add_argument('--card-overlay', type=Path, help='Install a prepared checked phone configuration tar on the SD')
    parser.add_argument('--native-desktop', action='store_true', help='Inspect the actual native Arch desktop boot')
    parser.add_argument('--keep-desktop', action='store_true', help='Leave a verified native desktop running')
    parser.add_argument('--native-script', type=Path, help='Native root command for desktop bring-up')
    args = parser.parse_args()
    BACKUPS, DISPLAY_PATTERN, USB_NETWORK = args.backups, args.display_pattern, args.usb_network
    PROVISION_CARD = args.provision_card
    DEPLOY_DESKTOP = args.deploy_desktop
    if DEPLOY_DESKTOP and (PROVISION_CARD or not USB_NETWORK):
        parser.error('--deploy-desktop requires --usb-network and excludes --provision-card')
    STORAGE_BENCH, SUPER_METADATA = args.storage_bench, args.super_metadata
    VENDOR_DRIVERS = args.vendor_drivers
    VENDOR_FIRMWARE = args.vendor_firmware
    WEBCAM_SER8 = args.webcam_ser8
    CARD_SCRIPT = args.card_script
    CARD_OVERLAY = args.card_overlay
    if CARD_OVERLAY and (DEPLOY_DESKTOP or PROVISION_CARD or not USB_NETWORK):
        parser.error('Configuration overlays require USB networking and a separate verified deployment')
    NATIVE_DESKTOP, KEEP_DESKTOP, NATIVE_SCRIPT = args.native_desktop, args.keep_desktop, args.native_script
    if (NATIVE_DESKTOP or KEEP_DESKTOP or NATIVE_SCRIPT) and not USB_NETWORK:
        parser.error('Native desktop options require --usb-network')
    if KEEP_DESKTOP and not NATIVE_DESKTOP:
        parser.error('--keep-desktop requires --native-desktop')
    if KEEP_DESKTOP and NATIVE_SCRIPT:
        parser.error('Persistent boot requires the standard desktop readiness checks')
    if CARD_SCRIPT and not USB_NETWORK:
        parser.error('--card-script requires --usb-network')
    if WEBCAM_SER8 and not (USB_NETWORK and (DISPLAY_PATTERN or NATIVE_DESKTOP)):
        parser.error('--webcam-ser8 requires USB networking and display or desktop testing')
    if (STORAGE_BENCH or SUPER_METADATA or VENDOR_DRIVERS or VENDOR_FIRMWARE) and not USB_NETWORK:
        parser.error('Storage comparison and firmware metadata capture require --usb-network')
    if PROVISION_CARD and not USB_NETWORK:
        parser.error('--provision-card requires --usb-network')
    os.umask(0o077)
    LOG.mkdir(parents=True, exist_ok=True)
    if USB_NETWORK:
        routes = json.loads(command(['ip', '-j', 'route']))
        target = ipaddress.ip_network('192.168.77.0/30')
        for route in routes:
            prefix = route.get('dst', 'default')
            if prefix != 'default' and target.overlaps(ipaddress.ip_network(prefix, strict=False)):
                raise RuntimeError('Diagnostic USB subnet overlaps an existing host route')
    manifest = json.loads((OUT / 'manifest.json').read_text())
    RUNTIME_LIMIT = manifest['runtime_limit_seconds']
    image = OUT / manifest['image']
    for path, expected in [(image, manifest['sha256']), (STOCK, manifest['stock_init_boot_sha256'])]:
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise RuntimeError(f'Checksum mismatch: {path.name}')
    if len(devices('adb')) != 1:
        raise RuntimeError('Exactly one authorized Android phone must be connected')
    for prop, expected in [('ro.product.device', 'vegas'), ('ro.boot.flash.locked', '0'),
                            ('ro.build.fingerprint', 'motorola/vegas_g_sys/vegas:16/W1VES36H.10-12-1/89dab7-701d8:user/release-keys'),
                            ('ro.boot.slot_suffix', '_b')]:
        actual = command(['adb', 'shell', 'getprop', prop]).strip()
        if actual != expected:
            raise RuntimeError(f'Unexpected {prop}: {actual}')
    command(['adb', 'reboot', 'bootloader'])
    deadline = time.monotonic() + 180
    print('Waiting for the phone in fastboot; cable reconnection may be needed.', flush=True)
    identifiers = []
    while time.monotonic() < deadline:
        identifiers = devices('fastboot')
        if identifiers:
            break
        time.sleep(2)
    if len(identifiers) != 1:
        raise RuntimeError('Exactly one fastboot device was not found; no image flashed')
    identity = identifiers[0]
    if 'current-slot: b' not in command(['fastboot', 'getvar', 'current-slot']):
        raise RuntimeError('Slot B is no longer active')
    if 'flashing_unlocked' not in command(['fastboot', 'getvar', 'securestate']):
        raise RuntimeError('Bootloader is not verified unlocked')
    command(['fastboot', 'flash', 'init_boot_b', str(image)], timeout=60)
    (LOG / 'state.json').write_text(json.dumps({'stock_restored': False, 'native_shell_verified': False}))
    command(['fastboot', 'reboot'])
    print('Diagnostic init_boot_b flashed and reboot requested; stock restoration is pending.', flush=True)
    deadline = time.monotonic() + RUNTIME_LIMIT + 180
    probed = False
    verified = False
    card_boots = 1
    while time.monotonic() < deadline:
        if not probed:
            for path in glob.glob('/dev/ttyACM*'):
                # Walk only this tty's USB ancestry to identify our diagnostic gadget.
                ancestors = (Path('/sys/class/tty') / Path(path).name / 'device').resolve()
                is_ours = any((p / 'product').is_file() and
                              (p / 'product').read_text().strip() == 'Vegas native Linux diagnostic'
                              for p in [ancestors, *ancestors.parents])
                if is_ours:
                    time.sleep(1)
                    if not USB_NETWORK:
                        try:
                            verified = serial_probe(path)
                        except (OSError, termios.error) as error:
                            (LOG / 'serial-error.txt').write_text(str(error))
                            print('Serial probe failed; continuing toward stock restoration.', flush=True)
                    probed = True
                    if USB_NETWORK:
                        try:
                            verified = network_probe()
                        except Exception as error:
                            CARD_PENDING = False
                            (LOG / 'network-error.txt').write_text(str(error))
                            print(f'USB network probe stopped: {error}; continuing toward stock restoration.', flush=True)
                    if NATIVE_DESKTOP and KEEP_DESKTOP and verified:
                        (LOG / 'state.json').write_text(json.dumps({'stock_restored':False,
                            'native_shell_verified':True,'native_desktop_kept_running':True},indent=2))
                        print('Verified native Omarchy desktop left running with its temperature guard.',flush=True)
                        return
                    break
        current = devices('fastboot')
        if current:
            if current != [identity]:
                raise RuntimeError('Fastboot identity changed; refusing to write')
            if CARD_PENDING and card_boots < (16 if DEPLOY_DESKTOP else 8):
                card_boots += 1
                print(f'Resuming checked card transfers in bounded native boot {card_boots}.', flush=True)
                command(['fastboot', 'reboot'])
                probed = False
                deadline = time.monotonic() + RUNTIME_LIMIT + 180
                continue
            command(['fastboot', 'flash', 'init_boot_b', str(STOCK)], timeout=60)
            (LOG / 'state.json').write_text(json.dumps({'stock_restored': True,
                                                       'native_shell_verified': verified}, indent=2))
            command(['fastboot', 'reboot'])
            print(f'Stock init_boot_b restored; Android reboot requested. Native shell verified={verified}', flush=True)
            return
        time.sleep(2)
    raise RuntimeError('Stock restoration pending: return phone to fastboot and reconnect USB')


if __name__ == '__main__':
    main()
