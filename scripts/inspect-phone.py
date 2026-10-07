#!/usr/bin/env python3
"""Read-only Android/bootloader survey; save raw identifiers in ignored private storage."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]


def run(command, timeout=20):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        return {"returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr}
    except subprocess.TimeoutExpired:
        return {"returncode": 124, "stdout": "", "stderr": "Timed out"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("adb", "fastboot"))
    args = parser.parse_args()
    os.umask(0o077)
    listing = run([args.mode, "devices"])
    devices = []
    for line in listing["stdout"].splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] == ("device" if args.mode == "adb" else "fastboot"):
            devices.append(fields[0])
    if listing["returncode"] or len(devices) != 1:
        print("Connect exactly one authorized phone in the selected mode. No survey performed.", file=sys.stderr)
        if args.mode == "adb":
            print("Enable USB debugging and accept the computer's RSA prompt on the phone.", file=sys.stderr)
        return 2

    base = [args.mode, "-s", devices[0]]
    if args.mode == "adb":
        commands = {
            "properties": ["shell", "getprop"],
            "kernel": ["shell", "uname", "-a"],
            "partitions": ["shell", "ls", "-l", "/dev/block/by-name"],
            "kernel_modules": ["shell", "ls", "/vendor/lib/modules"],
        }
    else:
        commands = {"variables": ["getvar", "all"]}
    raw = {name: run(base + command) for name, command in commands.items()}
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    destination = PROJECT / "private" / f"{stamp}-{args.mode}"
    destination.mkdir(parents=True, mode=0o700)
    (destination / "raw.json").write_text(json.dumps(raw, indent=2) + "\n")

    summary = {}
    if args.mode == "adb":
        allowed = {
            "ro.product.model", "ro.product.device", "ro.product.board", "ro.boot.hardware",
            "ro.soc.manufacturer", "ro.soc.model", "ro.build.fingerprint",
            "ro.build.version.release", "ro.build.version.security_patch", "ro.boot.slot_suffix",
            "ro.boot.verifiedbootstate", "ro.boot.flash.locked", "ro.boot.bootloader",
        }
        for line in raw["properties"]["stdout"].splitlines():
            if line.startswith("[") and "]: [" in line and line.endswith("]"):
                key, value = line[1:-1].split("]: [", 1)
                if key in allowed:
                    summary[key] = value
    else:
        allowed = {"product", "version-bootloader", "current-slot", "slot-count", "cid", "securestate", "is-userspace", "unlocked"}
        for line in (raw["variables"]["stdout"] + "\n" + raw["variables"]["stderr"]).splitlines():
            line = line.removeprefix("(bootloader)").strip()
            if ":" in line:
                key, value = line.split(":", 1)
                if key.strip() in allowed:
                    summary[key.strip()] = value.strip()
    (destination / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    print(f"Private survey saved to {destination}")
    failures = [name for name, value in raw.items() if value["returncode"]]
    if failures:
        print(f"Unavailable or permission-limited reads: {', '.join(failures)}")
    return 1 if all(value["returncode"] for value in raw.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
