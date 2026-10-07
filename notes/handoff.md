# Historical planning handoff

This record summarizes early planning before the actual phone was identified.
The target is the Moto G Power 2025 XT2515-1 / vegas. Native Omarchy and GPU
rendering are now verified; see [current status](status.md). Original assistant
transcripts, device identifiers and unlock keys are kept locally and are not
part of this repository.

## Original session intent

The user chose the retail unlocked Moto G Power 2026, preferring its LCD and the challenge of a new hardware port. The target is a native Arch Linux ARM system with an Omarchy-derived desktop, with cellular integration worth investigating. They wanted laptop preparation before the phone arrives and expressed interest in Valhalla/Val Protocol instead of waiting for Motorola's official unlock route.

Claude researched the stock-kernel/initramfs path and proposed an ARM VM, microSD rootfs, display/touch bring-up, software-rendered Hyprland and eventually an Android vendor-service container for radio/Wi-Fi/audio. The preparation requests at the end were interrupted; there was a temporary Valhalla research clone, but no installed adb/fastboot or project environment at the start of this continuation.

## Corrections and limits to carry forward

- The original session's `XT2617`/2026 identity assumptions were provisional. Subsequent published source identifies `vegas` with the Power 2025. Do not infer identical 2025/2026 firmware from the earlier notes; verify the actual unit.
- The [Valhalla README](https://github.com/crabcakes97/Valhalla) reports the G Power 2026 as unlocked, without identifying a tested firmware build. That is a project claim, not a compatibility guarantee for a newly purchased unit.
- Exact LK/bootloader compatibility and the bootloader's ability to accept the modified LK remain device-dependent. Host tests cannot establish those.
- The earlier claim that the inactive A/B slot provides a full fallback Android system was corrected during Claude's session. Virtual A/B snapshots do not establish that recovery path; verify real partitions and preserve stock firmware externally.
- Unlocking does not grant an unrooted Android shell permission to dump protected partitions. Firmware images can be saved in advance; device-specific radio/identity backups need an actual read path on the phone.
- Android 13+ launch devices commonly place the generic ramdisk in `init_boot`, with vendor contents in `vendor_boot`. Identify header versions, DTBs and vendor modules before replacing a ramdisk. See [AOSP generic boot partition](https://source.android.com/docs/core/architecture/partitions/generic-boot).
- A stock Android kernel is not itself proof of usable Linux DRM/KMS, GPU rendering, USB gadget networking, microSD boot or Hyprland. Discover those interfaces first. Software rendering remains conditional on a usable display backend and compositor support.
- Halium/oFono/vendor-service integration is a research branch. Android 16 compatibility, MediaTek radio HAL behavior, Wi-Fi and audio are unverified here. An installed SIM/eSIM profile does not guarantee Linux cellular support.
- Claude described a key padding bug. The current pinned Valhalla checkout generated a clean 20-character alphanumeric key in the local test. The adapter validates that shape rather than assuming the historical bug still applies.
- The current Omarchy ARM fork follows Omarchy 4's Lua configuration and Quickshell bar, so the earlier Waybar-based desktop plan needs updating.
- Keep public device-port material separate from raw device identifiers and the private transcript. No remote was created, no public posting occurred, and no commit identity was chosen.

## Next device-dependent work

Use the two read-only survey commands in the README, locate matching firmware, inspect LK and stock boot images, and write the actual recovery/unlock procedure from those results. After that, obtain backups and design the smallest recoverable Linux boot experiment. Do not infer phone support from a QEMU result.
