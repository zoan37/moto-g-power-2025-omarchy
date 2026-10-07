#!/usr/bin/env python3
"""Use the inspected Valhalla checkout with Arch-managed host dependencies."""
import argparse
import importlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]
CHECKOUT = PROJECT / "third_party" / "Valhalla"
sys.path.insert(0, str(CHECKOUT))


def check_host():
    for command in ("adb", "fastboot", "udevadm"):
        if not shutil.which(command):
            raise RuntimeError(f"Missing {command}; install the host packages listed in README.md")
    for module in ("tkinter", "capstone", "customtkinter", "pyasn1"):
        importlib.import_module(module)
    if not Path("/usr/lib/udev/rules.d/51-android.rules").is_file():
        raise RuntimeError("Install android-udev; USB access must be checked with the actual phone")
    if not (CHECKOUT / "val-protocol" / "lk_auto_patch.py").is_file():
        raise RuntimeError("The pinned Valhalla checkout is incomplete")


def tool_class():
    import Valhalla

    class ArchValhalla(Valhalla.ValhallaUnlockTool):
        def check_system(self):
            check_host()
            self.log("Arch host dependencies verified; using packaged android-udev rules.")
            self.log("Python dependencies were installed in the project virtual environment.")
            self.log("USB access and firmware compatibility require the actual phone.")
            self.log(f"Expected stock image: {CHECKOUT / 'lk.img'}")
            self.update_status("Ready for device inspection")

        def install_dependencies(self):
            check_host()
            # The inspected checkout contains its own liblk implementation.
            # Avoid upstream's runtime download of a different liblk revision.
            self.log("Using the pinned checkout and prepared Python environment.")
            return True

        def generate_key(self, serial):
            result = subprocess.run(
                [sys.executable, str(CHECKOUT / "val-protocol" / "lk_keygen.py"),
                 "--secret", Valhalla.SECRET, "--serialno", serial, "--count", "1"],
                capture_output=True, text=True, timeout=30, check=True,
            )
            key = result.stdout.splitlines()[-1]
            if len(key) != 20 or not key.isascii() or not key.isalnum():
                raise RuntimeError("Unexpected key-generator output; inspect it before using it")
            self.unlock_key = key
            self.log("Generated and validated a 20-character unlock key.")
            return key

    return ArchValhalla


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Check host dependencies, without opening the GUI")
    parser.add_argument("--smoke-gui", action="store_true", help="Create and destroy the GUI, without any device operation")
    args = parser.parse_args()
    check_host()
    if args.check:
        print("Host dependencies ready; matching LK and device compatibility must be verified before flashing.")
        return
    import tkinter as tk
    os.chdir(CHECKOUT)
    root = tk.Tk()
    if args.smoke_gui:
        root.withdraw()
    tool = tool_class()(root)
    if args.smoke_gui:
        root.update()
        root.destroy()
        print("Valhalla GUI smoke test passed; no device commands executed.")
    else:
        root.mainloop()


if __name__ == "__main__":
    main()
