# Moto G Power 2025 native Omarchy

The owned Motorola XT2515-1 (vegas) now boots Arch Linux ARM from its microSD
and runs the real Omarchy ARM desktop with Hyprland, Quickshell, Foot and an
on-screen keyboard. Persistent boot and a user-requested reboot back into
Linux are verified. The [phone UI and shutdown update](notes/phone-polish-20261007.md)
is installed: larger five-row keys, bar safe insets and clean live config checks
pass on the phone. One shutdown with USB disconnected, a minute staying off,
and normal power-on back into Omarchy passed; saved power logs confirm it.

The directory retains its earlier 2026 name; the actual target is the 2025.

## Working setup

See [native Omarchy use and Android recovery](notes/native-omarchy-20261006.md)
for the current image, exact checksums, architecture, USB access and limits.
[Status](notes/status.md) leads with the current result; older entries are
historical. [Bring-up notes](notes/linux-bringup-20261006.md) record experiments.

Verified on this phone:

- All 58 desktop transfer stages and 69,374 payload file hashes.
- Native Arch PID 1 and the ext4 microSD root, with a 1080×2388 DRM display.
- Omarchy bar/background, Foot, on-screen keyboard and clean live Hyprland config.
- Synthetic Ilitek input clicking a keyboard key and typing into Foot.
- The Keys button hiding and restoring the keyboard through that same input path.
- Virtual keyboard input executing a command in the terminal.
- Persistent boot beyond the test deadline and a normal user reboot into Linux.
- Exact stock Android recovery through fastboot after the corrected desktop test.

The stock kernel requires a custom Bash boot supervisor. Graphics use software
rendering. The user confirmed physical typing. Fast typing, radios/audio, suspend, reliable battery
percentage and sustained charging remain unverified. The 40 C temperature
guard remains active; 29–30 C was observed during the verified runs.

## Reproduce a native boot from recovered Android

The microSD must contain the verified desktop and phone overrides. From this
directory, with the original W1VES36H.10-12-1 Android build in slot B and USB
debugging connected:

```bash
python scripts/build-vegas-diagnostic.py --desktop
python scripts/try-vegas-diagnostic.py --usb-network --native-desktop --webcam-ser8
```

The bounded test restores stock init_boot_b when it finishes. Add
--keep-desktop only when a checked desktop should remain running. The runner
checks the phone model, exact build, slot, unlocked state and image hashes.
The preserved working image and manifest are under ignored
artifacts/vegas-linux-bringup/native-desktop/.

Linux recovery replaces only init_boot_b with the preserved matching stock
image. The Valhalla LK and unlocked bootloader stay in place. Use the exact
[recovery instructions](notes/native-omarchy-20261006.md#return-to-stock-android).

## Source and history

Phone overrides are in port/vegas/root; diagnostic/bootstrap code is in
bringup/ and scripts/. The desktop payload is the pinned
[omarchy-android ARM port](https://github.com/BlackFireAlex/omarchy-android).
Host desktop configuration was not changed.

[Valhalla unlock](notes/valhalla-20261006.md),
[model/source research](notes/power-2025.md),
[original Claude handoff](notes/handoff.md), and
[Arch ARM VM preparation](notes/arm-vm.md) preserve earlier work.

## Local data

private/, firmware/, backups/, artifacts/, vm/, .venv/ and third_party/ are
ignored by Git. Surveys and images can contain device identities; keep those
local. Matching firmware and 17 verified unique/boot partition readbacks are
preserved. This repository has no remote and no new commit was created.
