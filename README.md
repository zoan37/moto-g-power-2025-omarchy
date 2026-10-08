# Omarchy on Moto G Power 2025

Native Arch Linux ARM and Omarchy on the **Motorola Moto G Power 2025,
XT2515-1 / vegas**: MediaTek Dimensity 6300 (MT6835), Mali-G57 MC2, and a
1080 × 2388 portrait display.

Linux boots from microSD using the phone's stock Motorola kernel. Hyprland
drives the display directly at 120 Hz with GPU acceleration through a patched
Mesa/Kbase driver. This is native Linux, with no Android userspace or PRoot.

This repository contains the port's source, configuration, patches, and
hardware test records. It is an experimental device port, not a finished
installer or a general guide for other Motorola models. The actual target is
the **2025** phone; older research mentions 2026 before its identity was corrected.

## Hardware status

| Component | Verified result |
| --- | --- |
| Boot/storage | Persistent native boot from ext4 microSD; reboot back into Linux |
| Display/GPU | Direct DRM/KMS Hyprland at 1080 × 2388 / 120 Hz; patched Mesa on Mali-G57 |
| Desktop | Omarchy/Quickshell, Foot terminal, phone bar, app/workspace controls |
| Touch/keyboard | Physical tapping and typing; five-row keyboard; corner/camera safe insets |
| Wi-Fi | Stock MediaTek modules with wmt-pyloader, wpa_supplicant and dhcpcd; HTTPS verified |
| Browser | Chrome ARM64 runs; software default; separate GPU trials work |
| Power | Native reboot; one full shutdown/power-on cycle with USB disconnected |
| Recovery | Exact stock Android init_boot restoration and Android boot verified |
| Audio/cellular/suspend | Not established |
| Battery | Temperature guard active; charge percentage and sustained charging not established |

The stock kernel needs a custom Bash PID 1 supervisor rather than a normal
systemd boot. General Omarchy service operations still need porting. Shutdown
requires disconnecting USB/chargers. Browser video can lag; hardware video
decoding is not established.

## GPU work

The daily desktop uses the patched open-source Mesa/Kbase path. Measured
Foot commit-to-frame callback latency fell from roughly 114 ms on the initial
software desktop to 13–16 ms on direct GPU/KMS. Frame callbacks measure the
software presentation path, not physical panel illumination.

An isolated ARM r48 libmali experiment also renders on this phone, using a
hash-locked product-check patch and a process-local JM job-format adapter.
It passed 2,000 iterations each of shader rendering, full-resolution rendering,
and native-fence synchronization. Matching Chrome canvas trials produced
22 FPS with ARM GPU raster, 8–9 FPS with Mesa GPU raster, and 30–31 FPS with
software. ARM compositing with CPU raster reached 25 FPS. Chrome therefore
keeps its existing software default. The proprietary library is not included.

See [GPU implementation and measurements](notes/gpu-bringup-20261007.md).

## Start here

- [Current status and historical checkpoints](notes/status.md)
- [Native boot, USB access, and stock Android recovery](notes/native-omarchy-20261006.md)
- [Phone UI and cable-aware shutdown](notes/phone-polish-20261007.md)
- [Hardware research and model identification](notes/power-2025.md)
- [Valhalla unlock experiment](notes/valhalla-20261006.md)

The instructions record a tested device with an unlocked bootloader and the
original `W1VES36H.10-12-1` Android build in slot B. They depend on locally
prepared firmware, stock modules, a microSD rootfs, and build artifacts.
Those inputs are deliberately excluded from Git. A fresh clone alone cannot
flash a complete installation. Preserve matching stock firmware and
device-specific backups before any boot-chain or storage writes.

The original desktop builder/test commands, after preparing those prerequisites,
are:

```bash
python scripts/build-vegas-diagnostic.py --desktop
python scripts/try-vegas-diagnostic.py --usb-network --native-desktop
```

The bounded test restores stock `init_boot_b` when it finishes. The explicit
`--keep-desktop` option retains a verified desktop. Read the recovery and
bring-up notes before using either; the runner checks model, build, slot,
unlocked state and image hashes. GPU mode additionally needs the verified
driver/module package described in the GPU notes; it is not implied by these
two bootstrap commands.

The USB diagnostics endpoint accepts root commands on a dedicated
`192.168.77.1:8080` point-to-point link without authentication. Keep it on that
trusted development link; do not forward it or bind it to Wi-Fi.

## Repository layout

- `port/vegas/root/`: phone filesystem overrides and native startup helpers.
- `port/vegas/patches/`: Aquamarine and Mesa compatibility patches.
- `bringup/`: diagnostic ramdisk helpers, graphics/input probes and JM adapter.
- `scripts/`: guarded preparation, build, transfer and test tools.
- `vendor/wvkbd/`: matching GPL keyboard source, including immediate-feedback changes.
- `vendor/protocols/`: the pointer protocol needed to rebuild the input helpers.
- `notes/`: measurements, implementation details and chronological research.
- `notes/upstream-revisions.json`: upstream source locations and pinned revisions.

Several cross-builders use the sibling Fire HD workspace's staged ARM sysroot;
they need that SDK or adjustment to the documented build paths. Third-party
desktop, firmware and toolchain payloads are fetched/prepared separately.

Private transcripts, device identifiers, unlock keys, Wi-Fi credentials,
firmware, backups, proprietary libmali, and generated images stay outside Git.
Do not attach them to issues. See [license and upstream attribution](THIRD_PARTY.md).

Related experiment: [Omarchy on Fire HD 8 (2016)](https://github.com/zoan37/fire-hd8-omarchy).
