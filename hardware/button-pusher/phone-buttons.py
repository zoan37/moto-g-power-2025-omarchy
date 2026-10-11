#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial"]
# ///
"""Drive the Moto G Power 2025 button pusher (Pico + 3 servos) from the laptop.

Examples:
  phone-buttons.py ping
  phone-buttons.py press power              # 200 ms tap
  phone-buttons.py press voldown 150
  phone-buttons.py hold power+voldown 10    # seconds
  phone-buttons.py fastboot --until-fastboot
  phone-buttons.py powercycle               # hold Power 12 s (forced restart)
  phone-buttons.py menu next|prev|select    # Motorola bootloader menu navigation
  phone-buttons.py calibrate power          # interactive angle tuning
  phone-buttons.py snap screen.jpg          # webcam still of the phone screen

Port: $PHONE_BUTTONS_PORT, else /dev/phone-buttons (udev symlink), else the
only USB serial device with the Raspberry Pi vendor ID 2e8a.
Exit status: 0 ok, 1 command failed / timed out, 2 usage or connection error.
"""

import argparse
import fcntl
import os
import shutil
import subprocess
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is missing: `sudo pacman -S python-pyserial` or `pip install --user pyserial`")

RPI_VID = 0x2E8A
BUTTONS = ("power", "voldown", "volup")
ALIASES = {
    "power": "power", "pwr": "power", "p": "power",
    "voldown": "voldown", "vd": "voldown", "down": "voldown", "vol-": "voldown",
    "volup": "volup", "vu": "volup", "up": "volup", "vol+": "volup",
}


def die(msg, code=2):
    print(f"phone-buttons: {msg}", file=sys.stderr)
    sys.exit(code)


def norm_btns(spec):
    names = []
    for part in spec.lower().split("+"):
        if part not in ALIASES:
            die(f"unknown button '{part}' (use power, voldown, volup, joined with '+')")
        if ALIASES[part] not in names:
            names.append(ALIASES[part])
    return names


def find_port():
    env = os.environ.get("PHONE_BUTTONS_PORT")
    if env:
        return env
    if os.path.exists("/dev/phone-buttons"):
        return "/dev/phone-buttons"
    found = [p.device for p in list_ports.comports() if p.vid == RPI_VID]
    if len(found) == 1:
        return found[0]
    if not found:
        die("no Pico found (vendor 2e8a). Is it plugged in and running MicroPython? Set PHONE_BUTTONS_PORT.")
    die(f"several Pico serial ports {found}; set PHONE_BUTTONS_PORT")


class Pusher:
    def __init__(self, port, verbose=False):
        lock_dir = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
        self.lock = open(os.path.join(lock_dir, "phone-buttons.lock"), "w")
        fcntl.flock(self.lock, fcntl.LOCK_EX)  # one client at a time
        self.verbose = verbose
        try:
            self.ser = serial.Serial(port, 115200, timeout=0.05)
        except serial.SerialException as e:
            die(f"cannot open {port}: {e}")
        # pyserial asserts DTR on open; MicroPython only sends output while DTR is set.
        self.events = []
        self._buf = b""
        time.sleep(0.05)
        self.ser.reset_input_buffer()
        try:
            self.cmd("ping", timeout=2)
        except RuntimeError:
            die(f"{port} did not answer 'ping'. Is firmware/main.py installed on the Pico? "
                "(mpremote connect PORT cp firmware/main.py :main.py + reset)")

    def _readline(self, deadline):
        while time.monotonic() < deadline:
            nl = self._buf.find(b"\n")
            if nl >= 0:
                line, self._buf = self._buf[:nl], self._buf[nl + 1:]
                line = line.decode(errors="replace").strip()
                if self.verbose and line:
                    print(f"<< {line}", file=sys.stderr)
                return line
            self._buf += self.ser.read(256)
        return None

    def cmd(self, line, timeout=3.0):
        if self.verbose:
            print(f">> {line}", file=sys.stderr)
        self.ser.write((line + "\n").encode())
        deadline = time.monotonic() + timeout
        while True:
            resp = self._readline(deadline)
            if resp is None:
                raise RuntimeError(f"timeout waiting for reply to '{line}'")
            if resp.startswith("EVT "):
                self.events.append(resp)
            elif resp.startswith("OK"):
                return resp[2:].strip()
            elif resp.startswith("ERR"):
                raise RuntimeError(resp)
            # anything else (REPL noise) is ignored

    def wait_up(self, names, timeout):
        pending = set(names)
        deadline = time.monotonic() + timeout
        while pending:
            for ev in list(self.events):
                parts = ev.split()
                if len(parts) == 3 and parts[1] == "up" and parts[2] in pending:
                    pending.discard(parts[2])
                    self.events.remove(ev)
            if not pending:
                break
            line = self._readline(deadline)
            if line is None:
                raise RuntimeError(f"timeout waiting for release of {sorted(pending)}")
            if line.startswith("EVT "):
                self.events.append(line)

    def press(self, names, ms):
        self.events.clear()
        self.cmd(f"press {'+'.join(names)} {int(ms)}")
        self.wait_up(names, ms / 1000 + 3)

    def down(self, names, max_s):
        self.events.clear()
        return self.cmd(f"down {'+'.join(names)} {int(max_s * 1000)}")

    def up(self, names):
        self.cmd(f"up {'+'.join(names)}")
        self.wait_up(names, 3)


def fastboot_present():
    fb = shutil.which("fastboot")
    if not fb:
        return False
    try:
        r = subprocess.run([fb, "devices"], capture_output=True, text=True, timeout=3)
    except subprocess.TimeoutExpired:
        return False
    return bool(r.stdout.strip())


def snap(args):
    if not shutil.which("ffmpeg"):
        die("ffmpeg not installed")
    # skip the first frames so auto-exposure settles on the bright screen
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "v4l2"]
    if args.format:
        cmd += ["-input_format", args.format]
    if args.size:
        cmd += ["-video_size", args.size]
    cmd += ["-i", args.device, "-vf", f"select=gte(n\\,{args.skip})", "-frames:v", "1", "-y", args.out]
    r = subprocess.run(cmd, timeout=30)
    if r.returncode != 0:
        die("ffmpeg capture failed", 1)
    print(args.out)


def calibrate(p, btn):
    import json
    cfg = json.loads(p.cmd("config"))
    cur = float(cfg[btn]["rest"])
    print(f"Calibrating {btn}: rest={cfg[btn]['rest']} press={cfg[btn]['press']}\n"
          "  <number>  move to that angle     + / -  step 1 deg   ++ / --  step 0.5 deg\n"
          "  r  use current angle as REST (tip clear of the key, ~1.5 mm back)\n"
          "  p  use current angle as PRESS (key just clicks, plus ~2 deg)\n"
          "  t  test-press 300 ms using saved rest/press      q  quit")
    p.cmd(f"angle {btn} {cur}")
    while True:
        try:
            s = input(f"{btn} @ {cur:.1f}> ").strip()
        except EOFError:
            s = "q"
        if s in ("+", "-", "++", "--"):
            cur += {"+": 1, "-": -1, "++": 0.5, "--": -0.5}[s]
        elif s == "r":
            p.cmd(f"set {btn} rest {cur}")
            print(f"rest = {cur}")
            continue
        elif s == "p":
            p.cmd(f"set {btn} press {cur}")
            print(f"press = {cur}")
            continue
        elif s == "t":
            p.press([btn], 300)
            p.cmd(f"angle {btn} {cur}")
            continue
        elif s == "q":
            p.cmd("detach")
            if input("save to Pico flash? [y/N] ").strip().lower() == "y":
                p.cmd("save")
                print("saved")
            return
        else:
            try:
                cur = float(s)
            except ValueError:
                print("?")
                continue
        cur = max(0.0, min(180.0, cur))
        p.cmd(f"angle {btn} {cur}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: autodetect)")
    ap.add_argument("-v", "--verbose", action="store_true", help="print serial traffic")
    sub = ap.add_subparsers(dest="cmd", required=True)

    for name in ("ping", "status", "version", "config", "release", "detach", "save"):
        sub.add_parser(name)
    s = sub.add_parser("press", help="tap button(s), e.g. press power 200")
    s.add_argument("btns"); s.add_argument("ms", nargs="?", type=int, default=200)
    s = sub.add_parser("hold", help="hold button(s) for N seconds, e.g. hold power+voldown 10")
    s.add_argument("btns"); s.add_argument("seconds", type=float)
    s = sub.add_parser("down", help="press and keep holding (Pico auto-releases after --max s)")
    s.add_argument("btns"); s.add_argument("--max", type=float, default=60)
    s = sub.add_parser("up", help="release button(s)")
    s.add_argument("btns", nargs="?", default="power+voldown+volup")
    s = sub.add_parser("fastboot", help="hold Power+VolDown to reach the bootloader")
    s.add_argument("--seconds", type=float, default=10)
    s.add_argument("--until-fastboot", action="store_true",
                   help="hold until `fastboot devices` lists the phone (max --timeout)")
    s.add_argument("--timeout", type=float, default=30)
    s = sub.add_parser("powercycle", help="hold Power to force a restart")
    s.add_argument("--seconds", type=float, default=12)
    s = sub.add_parser("poweron", help="hold Power briefly to boot a phone that is off")
    s.add_argument("--seconds", type=float, default=3)
    s = sub.add_parser("menu", help="bootloader menu: next=VolDown, prev=VolUp, select=Power")
    s.add_argument("action", choices=("next", "prev", "select"))
    s.add_argument("count", nargs="?", type=int, default=1)
    s = sub.add_parser("angle", help="move one servo to a raw angle (stays powered)")
    s.add_argument("btn"); s.add_argument("deg", type=float)
    s = sub.add_parser("set", help="store rest/press angle in RAM (then `save`)")
    s.add_argument("btn"); s.add_argument("which", choices=("rest", "press")); s.add_argument("deg", type=float)
    s = sub.add_parser("calibrate", help="interactive angle tuning for one button")
    s.add_argument("btn")
    s = sub.add_parser("raw", help="send one raw protocol line")
    s.add_argument("line", nargs="+")
    s = sub.add_parser("snap", help="grab one webcam frame (needs ffmpeg; no Pico needed)")
    s.add_argument("out", nargs="?", default="phone-screen.jpg")
    s.add_argument("--device", default=os.environ.get("PHONE_CAM", "/dev/video0"))
    s.add_argument("--size", default="1280x720")
    s.add_argument("--format", help="v4l2 input format, e.g. mjpeg or yuyv422")
    s.add_argument("--skip", type=int, default=15, help="frames to discard while exposure settles")
    a = ap.parse_args()

    if a.cmd == "snap":
        return snap(a)

    p = Pusher(a.port or find_port(), verbose=a.verbose)
    try:
        if a.cmd in ("ping", "status", "version", "config", "release", "detach", "save"):
            print(p.cmd(a.cmd))
        elif a.cmd == "press":
            p.press(norm_btns(a.btns), a.ms)
        elif a.cmd == "hold":
            p.press(norm_btns(a.btns), a.seconds * 1000)
        elif a.cmd == "down":
            print(p.down(norm_btns(a.btns), a.max))
        elif a.cmd == "up":
            p.up(norm_btns(a.btns))
        elif a.cmd == "fastboot":
            combo = ["power", "voldown"]
            if not a.until_fastboot:
                p.press(combo, a.seconds * 1000)
            else:
                if not shutil.which("fastboot"):
                    die("fastboot not in PATH (needed for --until-fastboot)")
                t0 = time.monotonic()
                p.down(combo, a.timeout)
                try:
                    while time.monotonic() - t0 < a.timeout:
                        if fastboot_present():
                            print(f"fastboot device after {time.monotonic() - t0:.1f}s")
                            break
                        time.sleep(0.5)
                    else:
                        print("no fastboot device before timeout", file=sys.stderr)
                        return 1
                finally:
                    p.up(combo)
        elif a.cmd == "powercycle":
            p.press(["power"], a.seconds * 1000)
        elif a.cmd == "poweron":
            p.press(["power"], a.seconds * 1000)
        elif a.cmd == "menu":
            btn = {"next": "voldown", "prev": "volup", "select": "power"}[a.action]
            for _ in range(a.count):
                p.press([btn], 150)
                time.sleep(0.25)
        elif a.cmd == "angle":
            print(p.cmd(f"angle {norm_btns(a.btn)[0]} {a.deg}"))
        elif a.cmd == "set":
            print(p.cmd(f"set {norm_btns(a.btn)[0]} {a.which} {a.deg}"))
        elif a.cmd == "calibrate":
            calibrate(p, norm_btns(a.btn)[0])
        elif a.cmd == "raw":
            print(p.cmd(" ".join(a.line), timeout=5))
    except RuntimeError as e:
        print(f"phone-buttons: {e}", file=sys.stderr)
        try:
            p.cmd("release")
        except RuntimeError:
            pass
        return 1
    except KeyboardInterrupt:
        p.cmd("release")
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
