#!/usr/bin/env python3
"""Check prepared Valhalla artifacts against an attached phone; never write to it."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

PROJECT = Path(__file__).resolve().parents[1]


def run(args):
    return subprocess.run(args, capture_output=True, text=True, check=True, timeout=20).stdout


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(8 * 1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="W1VES36H.10-12-9-22")
    args = parser.parse_args()
    if Path(args.build).name != args.build or args.build in (".", ".."):
        parser.error("--build must be a build identifier, not a path")
    firmware = PROJECT / "firmware" / args.build
    artifacts = PROJECT / "artifacts" / f"vegas-{args.build}"
    problems = []
    patch = json.loads((artifacts / "manifest.json").read_text())
    if sha256(firmware / "lk.img") != patch["stock_lk_sha256"]:
        problems.append("Stock LK hash differs from the analyzed image")
    if sha256(artifacts / "lk.unlock-serial.img") != patch["patched_lk_sha256"]:
        problems.append("Patched LK hash differs from the verified image")
    if patch.get("image_verifier_result") != "VALID":
        problems.append("Local patched-image verification has not passed")
    package_manifest = firmware / "full-package-manifest.json"
    if not package_manifest.exists():
        problems.append("Full stock recovery archive has not finished verification")
    else:
        package = json.loads(package_manifest.read_text())
        if Path(package["name"]).name != package["name"]:
            raise ValueError("Unexpected package filename")
        if sha256(firmware / package["name"]) != package["sha256"]:
            problems.append("Recovery archive hash differs from the verified download")
    devices = [line.split()[0] for line in run(["adb", "devices"]).splitlines()
               if len(line.split()) == 2 and line.split()[1] == "device"]
    if len(devices) != 1:
        problems.append("Connect exactly one phone with authorized USB debugging")
    else:
        base = ["adb", "-s", devices[0], "shell", "getprop"]
        sku = run(base + ["ro.boot.hardware.sku"]).strip()
        board = run(base + ["ro.product.device"]).strip()
        fingerprint = run(base + ["ro.build.fingerprint"]).strip()
        print(f"Phone: {sku}, {board}")
        print(f"Installed fingerprint: {fingerprint}")
        if sku != "XT2515-1" or board != "vegas":
            problems.append("Prepared images target the retail XT2515-1 Vegas")
        metadata = list(firmware.glob("*.info.txt"))
        if len(metadata) != 1:
            raise ValueError("Expected one firmware build metadata file")
        expected = [line.split(":", 1)[1].strip() for line in metadata[0].read_text().splitlines()
                    if line.strip().startswith("Build Fingerprint:")]
        if len(expected) != 1 or fingerprint != expected[0]:
            problems.append("Firmware fingerprint does not match the attached phone")
    for problem in problems:
        print(f"NOT READY: {problem}")
    if problems:
        return 2
    print("Artifact hashes and phone fingerprint match. Device acceptance, boot and low-level recovery remain unverified.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"NOT READY: {error}")
        raise SystemExit(2)
