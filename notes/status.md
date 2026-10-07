# Native Omarchy phone UI update — 2026-10-07

The Moto G Power 2025 XT2515-1 boots native Arch Linux ARM from microSD with
real Omarchy, Hyprland, Foot and an on-screen keyboard. All 69,374 original
desktop payload hashes matched; persistent boot, native reboot, actual touch
input and stock Android recovery were verified previously.

The phone update is installed. The larger five-row keyboard, bottom safe
space, camera/corner bar insets, live reload and empty configerrors pass on
the phone. Synthetic contacts through the real Ilitek input device type into
Foot and toggle Keys. Reproducible builders include the two live startup fixes.
The original working image/overlay and Android recovery hashes are preserved.

Cable-aware shutdown rejects connected root requests and opens a desktop
unplug prompt. The user unplugged USB; a 65-second webcam recording showed a
continuously black screen with no boot logo. After the user powered on and
reconnected USB, native Omarchy returned with clean config checks. The saved
previous-boot record confirms ACTION:poweroff with all external sources offline.
This establishes one successful shutdown/power-on cycle, rather than only a
screen-off observation. Repeated cycles and shutdown with the cable already
absent remain additional checks. The phone is currently running native Linux
and connected over USB. Temperature was 23–24 C and the 40 C guard remains active.

Keyboard sizing is accepted by the user, who still reports delayed text feedback.
Instrumented taps reached Foot's PTY in roughly 2–5 ms, but the subsequent
Wayland frame callback took roughly 110 ms. That callback is not a measurement
of panel illumination. A 60 Hz setting, governor change and partial-repaint
experiment did not materially resolve the delay; the original full-resolution,
30 Hz software-rendered desktop and Aquamarine library were restored.

Original stock GPU modules now bind successfully during a manual trial and
expose /dev/mali0. Properties identify Mali-G57 with two cores and JM API 11.38.
An isolated Mesa/Kbase EGL test still fails to create a rendering screen;
hardware-accelerated Omarchy is not established. A double free in the trial's
initialization failure path was fixed and tested: failure now returns cleanly.
Additional diagnostics are built locally, including a direct context probe
that submits no GPU jobs. See [GPU progress and next tests](gpu-bringup-20261007.md).

Live access is currently blocked by the execution environment's network policy.
The user selected Full access, but the immediate retry still returned
Operation not permitted. Check effective permissions after the session refresh;
do not infer a phone or USB fault from this denial. Radios, audio and other
phone hardware support remain unverified. See [phone update details](phone-polish-20261007.md) and
[working setup/recovery](native-omarchy-20261006.md). Older entries below are
historical and superseded by this result.

# Verified native Arch userspace and display — 2026-10-06

The 32 GB Samsung microSD is now a single vegas-arch ext4 partition containing
the signed Arch Linux ARM base. All 33,457 regular-file hashes matched; native
Arch bash/pacman/systemd-version commands ran from the card in a chroot under
the stock Linux kernel. A clean unmounted filesystem check passed. This is not
yet an Arch systemd PID 1 boot, Hyprland session or Omarchy installation.

Stock init_boot_b was restored. Fresh ADB verified Android's original build,
slot B, completed boot and unlocked bootloader. Screen lock None is active and
ADB reconnected without manual unlocking. The read-only 64 MiB benchmark measured internal storage at 326.2 MiB/s and
microSD at 81.9 MiB/s. A corrected atomic display test committed 1080×2388
at 60 Hz without errors. A repeat experiment captured two SER8 webcam frames
showing the full color-bar pattern: native display output is visually verified.
Stock vendor modules were extracted for the next touchscreen probe. SER8 SSH
and camera captures work after moving the webcam to another USB port. Older pending card/unlock
entries below are historical.

# Current continuation — 2026-10-06

The user authorized replacing the Pi installation on the inserted 32 GB Samsung
microSD and requested using the phone as its reader. Android was freshly
verified on its original build and slot B. The new one-partition MBR was written
from native Linux; formatting failed because BusyBox ash selected its built-in
mke2fs instead of the staged full formatter. That is corrected with an absolute
path and verified in the same staged BusyBox shell. No Arch archive stages have
yet been copied. Stock init_boot_b was restored after both attempts; Android
unlock/USB reconnection is pending for the corrected run. See
[the bring-up record](linux-bringup-20261006.md#microsd-provisioning-continuation).

The user requested automatic access to Android after reboot. The ADB attempt
did not successfully disable the screen lock; manual Screen lock > None steps
were provided, with credential entry only on the phone.

# Preparation status — 2026-10-03

**Latest Linux result, 2026-10-06:** Native Linux diagnostic ramdisk booted on the phone and provided a root USB shell. Stock kernel/modules detected internal UFS and 1080×2388 DRM panel modes. Touch and rendering are not yet working/verified. Only `init_boot_b` was temporarily replaced; it was restored and fresh ADB verified Android boot completed on the original build/slot with the bootloader still unlocked. See [first native Linux boot](linux-bringup-20261006.md). Earlier no-phone/locked/candidate entries below are historical. Arch ARM and Omarchy remain later milestones.

**USB/backup follow-up:** Native USB Ethernet is working. Seventeen device-specific and boot-chain partition readbacks were transferred and verified, including byte-identical current-build boot/vendor_boot/dtbo images. Display-tool loading and resource queries now work on hardware; the atomic pattern requires an explicit plane and the corrected command is prepared. No rendered pattern or desktop has yet been demonstrated. Android restoration after the loader-fix experiment was freshly verified. The user inserted a 32 GB Samsung microSD containing a Pi installation; native Linux also read its partition/filesystem metadata without mounting it. Contents need resolving before any card write.

**Latest physical state:** The card/resource-query experiment restored stock init_boot_b and requested Android reboot successfully. ADB has not yet reappeared for fresh verification; USB reconnection was requested. The new plane-inclusive display test is prepared but has not run. Card data preferences remain pending and no card write has occurred.

**Latest device result, 2026-10-06:** The user authorized proceeding with Valhalla. The current-build candidate was flashed only to `lk_b`, booted into fastboot, and unlocked the phone after its on-screen confirmation. A fresh post-restart read verified `securestate: flashing_unlocked`, slot B and unchanged MBM. Android boot was subsequently verified over ADB: `flash.locked=0`, verified boot `orange`, vbmeta device state `unlocked`, and boot completed `1`. The original `W1VES36H.10-12-1` build and slot B remain unchanged. No Android upgrade or custom OS installation was performed. Earlier locked/unchanged-phone entries below are historical. See [the complete record](valhalla-20261006.md).

**Update 2026-10-06:** The user's XT2515-1 is connected and surveyed. Build is `W1VES36H.10-12-1`; bootloader remains locked. Valhalla's patched LK, backups and the full verified 5.9 GB Lenovo recovery archive are prepared for the newer `W1VES36H.10-12-9-22` build. The read-only preflight passes artifact checks but blocks on the firmware mismatch. No firmware was flashed or upgraded. See [Valhalla continuation](valhalla-20261006.md) for results and remaining requirements. The original preparation record below is historical.

**Subsequent mismatch research:** Found and locally patched an exact-build `W1VES36H.10-12-1` candidate from the TMO archive. Its fingerprint, MBM and signing CID match the phone. Previous shared-build retail/TMO LK files are byte-identical, but current cross-channel applicability and low-level recovery remain unverified. Candidate files are quarantined; the phone is unchanged. See [the detailed findings](valhalla-20261006.md#firmware-mismatch-investigation).

Current target: **Moto G Power 2025**, following the user's explicit request. The original workspace directory name is retained. No phone is attached and purchase/ownership is not confirmed. An Omarchy-inspired Android alternative is under discussion, not an established ROM for this model.

## Completed

- Found and reviewed the relevant Claude phone session. Saved its text in ignored, private storage and wrote [handoff.md](handoff.md), with the original full-log link and corrections.
- Created this local Git workspace without a remote or new commits.
- Installed `android-tools` 37.0.0-5, `android-udev` 20260423-1, `usbutils` 019-1, `tk` 8.6.16-1, `qemu-system-aarch64` 11.1.1-1 and `edk2-aarch64` 202608-1. The package install also supplied Tcl.
- Prepared `.venv` on Python 3.14.7. Installed versions are recorded in `requirements.lock.txt`; `pip check` passed.
- Cloned Valhalla, Val Protocol and Omarchy ARM. Pinned revisions are in `upstream-revisions.json`; upstream sources were not modified.
- Prepared an Arch-specific Valhalla launcher. Host checks and creating/destroying its actual Tk GUI passed without executing device commands. The adapter uses Arch-managed USB rules and prepared dependencies instead of Ubuntu auto-setup.
- Valhalla's existing 19 tests passed. These mock device operations; they do not establish real-phone compatibility.
- The Omarchy ARM install, package-resolution and platform-detection test scripts passed using their fixture environments. This is not a completed desktop installation.
- The Valhalla key generator and adapter produced valid 20-character keys for three synthetic serial numbers. The previously described padding bug was not reproduced with this revision's default alphabet.
- Device-survey checks passed for no device, multiple devices, unauthorized state and identifier filtering. Actual `adb` and `fastboot` surveys stopped cleanly with no phone attached. Raw data destinations and the original transcript are ignored by Git.
- Downloaded the generic Arch Linux ARM image over verified HTTPS from a working mirror and verified its detached signature against the official signing fingerprint. The archive's kernel is `7.1.6-1-aarch64-ARCH`.
- Constructed an 8 GiB sparse ext4 VM disk using archive ownership metadata. Booted Arch Linux ARM, logged into the root shell, verified `uname -m` = `aarch64`, confirmed DHCP address `10.0.2.15` and successful DNS lookup, and observed zero failed systemd units.
- Initialized the guest's pacman keyring and populated the Arch Linux ARM signing key successfully.
- Shut the VM down cleanly. Read-only filesystem checks passed, and the launcher's extracted kernel matches the signed archive's kernel. `scripts/run-arm-vm --check` passed.
- Updated the target from Power 2026 to Power 2025, correcting the expected model family to XT2515 and distinguishing `vegas` source evidence from the earlier provisional 2026 assumptions.
- Downloaded ten Motorola source files pinned to kernel-mtk commit `72b5d32be0bb7cee12765bf236972f8f4c622b83`. A source/URL/SHA-256 manifest is saved in ignored `artifacts/vegas-source-audit/manifest.json`. The vendor display driver advertises atomic DRM modesetting and dumb-buffer creation, which gives us a concrete display investigation path, without establishing compositor or GPU compatibility.
- Rechecked `scripts/valhalla --check` and `scripts/run-arm-vm --check` after selecting the 2025; both passed. `adb devices -l` and `fastboot devices` listed no phone.

## Problems found and handled

- Missing Android tools and USB utilities: installed from Arch repositories.
- Missing Tk shared library: installed `tk` and validated the GUI.
- Valhalla's Ubuntu `apt` setup and custom udev rewrite: replaced at the launcher level with the already prepared Arch dependencies and packaged rules. Actual USB permissions remain to be verified with the phone.
- Arch ARM's original download hostname failed TLS verification. Used a working HTTPS mirror and verified the detached signature; certificate validation stayed enabled.
- Earlier desktop notes assumed Waybar and text Hyprland configuration. The current ARM fork uses Quickshell and Lua; the handoff records the change.

## Requires the phone

- Exact retail variant, codename, SoC, firmware fingerprint, bootloader version, slot layout and actual USB IDs.
- Matching stock firmware and a confirmed recovery path.
- Official unlock eligibility and, if needed, LK analysis and whether this actual unit accepts a bootloader modification. Power 2025 owner reports lack the complete retail/build-specific procedure required here.
- Bootloader unlock, rooted partition/calibration backups, stock kernel configuration and boot-image inspection.
- USB shell, display/input, graphics, microSD access, power management and vendor radio/Wi-Fi/audio compatibility under Linux.

## Further host/VM work

The serial-console Arch ARM VM is ready as a development base. Its package upgrade, sudo setup, graphical virtual device setup, minimal Hyprland session, full Omarchy ARM install and phone UI adaptations are later steps. No working Omarchy desktop on ARM or native Linux phone port is claimed by this preparation.
