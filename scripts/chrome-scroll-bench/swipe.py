#!/usr/bin/env python3
"""Inject one finger swipe into the real Ilitek touchscreen evdev node.

usage: swipe.py X Y0 Y1 DURATION_MS   (screen pixels, portrait 1080x2388)
Events go through the kernel input device, libinput, Hyprland and the client.
"""
import fcntl, glob, os, struct, sys, time

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
SYN_REPORT, BTN_TOUCH = 0, 0x14a
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x2f, 0x35, 0x36, 0x39

def absinfo(fd, code):
    req = (2 << 30) | (24 << 16) | (ord('E') << 8) | (0x40 + code)
    buf = fcntl.ioctl(fd, req, bytes(24))
    return struct.unpack('6i', buf)  # value, min, max, fuzz, flat, res

def find():
    for path in sorted(glob.glob('/dev/input/event*')):
        fd = os.open(path, os.O_RDWR)
        name = fcntl.ioctl(fd, (2 << 30) | (128 << 16) | (ord('E') << 8) | 0x06, bytes(128)).split(b'\0')[0]
        if name == b'ILITEK_TDDI':
            return fd
        os.close(fd)
    raise SystemExit('no ILITEK_TDDI')

x, y0, y1, ms = (int(v) for v in sys.argv[1:5])
fd = find()
_, xmin, xmax, *_ = absinfo(fd, ABS_MT_POSITION_X)
_, ymin, ymax, *_ = absinfo(fd, ABS_MT_POSITION_Y)
sx = lambda px: xmin + px * (xmax - xmin) // 1079
sy = lambda py: ymin + py * (ymax - ymin) // 2387
def emit(t, c, v):
    now = time.time()
    os.write(fd, struct.pack('qqHHi', int(now), int((now % 1) * 1e6), t, c, v))
def sync():
    emit(EV_SYN, SYN_REPORT, 0)

emit(EV_ABS, ABS_MT_SLOT, 0); emit(EV_ABS, ABS_MT_TRACKING_ID, 4321)
emit(EV_ABS, ABS_MT_POSITION_X, sx(x)); emit(EV_ABS, ABS_MT_POSITION_Y, sy(y0))
emit(EV_KEY, BTN_TOUCH, 1); sync()
steps = max(2, ms // 8)  # 125 Hz report rate
start = time.monotonic()
for i in range(1, steps + 1):
    target = start + i * ms / 1000 / steps
    while time.monotonic() < target:
        time.sleep(0.0005)
    emit(EV_ABS, ABS_MT_POSITION_Y, sy(y0 + (y1 - y0) * i // steps)); sync()
emit(EV_ABS, ABS_MT_TRACKING_ID, -1); emit(EV_KEY, BTN_TOUCH, 0); sync()
os.close(fd)
print('swiped', steps, 'steps')
