# Handoff: own kernel, audio/suspend/battery, and web apps as the phone's apps (2026-10-10)

Written at the end of the MT8189 Chromebook port session
([zoan37/lenovo-slim3-chromebook-omarchy](https://github.com/zoan37/lenovo-slim3-chromebook-omarchy)), for a new
session on this phone. Nothing on the phone was touched for this note; everything below about the phone comes from
this repo's notes and `artifacts/`.

## Goal

Make the phone a normal Omarchy system: **systemd as PID 1** (Omarchy's services, shutdown with USB attached), then
**audio, suspend and battery percentage**, and make **web apps** (Grok, X, ...) the phone's apps. Cellular and hardware video decode are out of scope (closed modem and
codec stacks on this kernel).

## Product direction (agreed with the user, 2026-10-10)

There is no official Omarchy mobile yet. From first principles, a good phone experience means it wakes instantly,
lasts all day, is touch-first, gets notifications, and runs a few daily apps fast. Without cellular this phone is a
pocket Omarchy computer, not a phone replacement. The direction:

- **Web apps first, the Omarchy way.** Omarchy itself ships X, HEY, ChatGPT and others as Chrome `--app` web apps.
  This port already overrides `omarchy-launch-webapp` (`port/vegas/root/usr/local/bin/`) to Chrome `--app` and runs
  one app per workspace. Next:
  - install web apps for the user's daily apps (**Grok** grok.com and **X** x.com first) with launcher icons in the
    phone bar
  - check sign-in persistence and web push notifications while Chrome runs
  - make them feel native: full screen, scaling, keyboard behaviour
- **Android apps (Waydroid): optional, later.** Only for apps with no usable web version. Reasons not to start
  there:
  - it's a second OS: about 1–1.5 GB of the 8 GB RAM, plus extra battery drain
  - without Google Play Services, the X/Grok Android apps likely work worse than their websites (login, push)
  - with GApps the device is uncertified, and banking apps often still refuse
  - Android apps would likely start on software rendering

  What makes it feasible later: the stock kernel already has everything Waydroid needs (`ANDROID_BINDER_IPC`,
  `ANDROID_BINDERFS`, `ASHMEM`, `VETH`, `BRIDGE`, NAT; `USER_NS` is off, but Waydroid's privileged LXC doesn't need
  it). An interesting idea for its graphics: the Android container could use the phone's own Mali driver from the
  vendor partition (Halium-style), which the Linux desktop can't easily do.
- **Not planned:** calls/SMS (closed modem stack) and hardware video decode.

## Why a kernel rebuild comes first

The stock GKI kernel (5.15, `android13-5.15`, build W1VES36H.10-12-1) is why PID 1 is a Bash supervisor. Its config
(`artifacts/vegas-linux-bringup/stock-kernel.config`, captured from `/proc/config.gz`) has:

| Option | Stock | systemd needs it? |
|---|---|---|
| `CONFIG_DEVTMPFS` | off | yes (hard requirement) |
| `CONFIG_FHANDLE` | off | yes (hard requirement for systemd and udev) |
| `CONFIG_AUTOFS_FS` | off | optional (automounts) |
| `CONFIG_SYSVIPC` | off | not systemd; some desktop software (shared memory) |
| `CONFIG_VT` | off | optional (no VT on a phone is fine; seatd already runs without VT binding) |
| `CGROUPS`, `MEMCG`, `NAMESPACES`, `SECCOMP`, `BPF_SYSCALL`, `INOTIFY_USER` | on | yes, already fine |
| `SUSPEND`, `PSTORE`, `SND_SOC`, `USB_CONFIGFS` | on | |

No userspace workaround exists for DEVTMPFS/FHANDLE, so systemd needs our own kernel build.

## The ABI problem (main risk)

The 197 vendor modules in vendor_boot (+ vendor_dlkm) were built against the stock kernel's KMI:
`CONFIG_MODVERSIONS=y` (symbol CRCs), `CONFIG_TRIM_UNUSED_KSYMS=y`, `CONFIG_MODULE_SIG=y`,
`CONFIG_CFI_CLANG=y` + `CONFIG_LTO_CLANG_FULL=y`, Android clang 14.0.7 (r450784e). Suggested tiers:

1. **ABI-neutral first:** only `DEVTMPFS` (+ `DEVTMPFS_MOUNT`) and `FHANDLE` (+ `AUTOFS_FS`). These add a
   filesystem and syscalls without changing structures that vendor modules see, so the stock modules may load
   unchanged. Verify: rebuilt kernel's `Module.symvers` CRCs vs the stock ones for every symbol the vendor modules
   import (`modprobe --dump-modversions` / `modinfo`), before booting anything.
2. **If CRCs or CFI types differ, or for SYSVIPC/VT** (SYSVIPC changes `task_struct`): rebuild the vendor modules
   too, from the same source drop, so the whole set matches (the postmarketOS approach for downstream devices).
3. Use the **exact** Android clang (r450784e): with CFI + full LTO, mixing compilers is asking for trouble. Android's
   prebuilt toolchain is x86_64-only, so build on an x86_64 host (this Dell or the SER8), **not** the DGX Spark
   (arm64), unlike the Chromebook kernels.

## Sources

- Kernel: [MotorolaMobilityLLC/kernel-mtk](https://github.com/MotorolaMobilityLLC/kernel-mtk), tag
  `android-16-release-w1ve36h.10-12r3` (the charger notes in `notes/battery-charging.md` already cite it; check it
  matches the running build's banner, W1VES36H.10-12-1).
- Motorola modules: [MotorolaMobilityLLC/motorola-kernel-modules](https://github.com/MotorolaMobilityLLC/motorola-kernel-modules), same tag.
- Source request listing the separate connectivity and GPU repos: [kernel-mtk#194](https://github.com/MotorolaMobilityLLC/kernel-mtk/issues/194).
- Vendor config fragment: `artifacts/battery-research/moto-mgk_64_k515-vegas.config`.

## Boot and recovery facts

- Boot header v4: `boot` holds only the kernel, `init_boot` the generic ramdisk (this port replaces `init_boot_b`),
  `vendor_boot` the DTB + 197 modules. `fastboot boot` does **not** boot from RAM on this unit (it says OKAY plus
  "command is not implemented"): testing a kernel means replacing `boot_b`.
- Stock images with SHA-256 manifests: `backups/vegas-W1VES36H.10-12-1/` (boot, init_boot, vendor_boot, dtbo, vbmeta,
  lk). Bootloader unlocked via Valhalla (orange state), so a custom `boot` image boots.
- Per CONTRIBUTING.md: flashing, boot-chain changes and reboots are a deliberate hardware session with matching
  recovery files. **Ask the user before each flash.** A kernel that doesn't boot = restore `boot.stock.img`.
- Access: USB network, `http://192.168.77.1:8080/cgi-bin/command` (`scripts/measure-vegas-input.py` → `command()`).
  The laptop firewall blocks phone→laptop; push files in chunks.

## Lessons from the Chromebook worth reusing

- **A slot/fallback tool before anything risky:** the Chromebook's `quigon-kernel` (install / status / restore
  previous / boot-once test slot) made kernel iteration safe. Here: keep stock `boot` on slot A or as a file, and
  script "flash test kernel, boot once, fall back".
- **Health check:** `quigon-doctor` (one command, ok/problem per subsystem) catches regressions after every kernel
  change.
- **Audio:** the Chromebook's UCM profile came from ChromeOS's mixer configs. Here, Android's
  `/vendor/etc/audio/` (`mixer_paths*.xml`, audio policy) has the exact ALSA controls per route: translate to a UCM2
  profile for PipeWire.
- **Suspend:** `scripts/sleepstress.sh` in the Chromebook repo (RTC wake, checks Wi-Fi/display/sound after each
  cycle). Android kernels suspend fine, but watch for Wi-Fi (WMT) and the display coming back.
- **Battery:** capacity reads -1 (`notes/native-omarchy-20261006.md`). MediaTek's gauge may wait on an init step that
  Android userspace normally does. Then Omarchy's battery panel needs the same fix as the Chromebook
  (`upower -e | grep BAT` misses non-`BAT` names).
- **Small Omarchy items:**
  - hold Quickshell at 0.3.1 (`IgnorePkg`): 0.3.2 breaks re-locking, quickshell#1230
  - `omarchy-notification-wait` before `notify-send` in autostart
  - `$OMARCHY_PATH/bin` comes first in Hyprland's PATH, so `/usr/local/bin` overrides only work by absolute path
- **Privacy:** same rules as this repo's CONTRIBUTING.md. Scan diffs before committing (no serial, unlock code,
  IMEI, Wi-Fi details, home paths).

## Suggested order

1. No phone needed: fetch the tag, diff its defconfig + fragments against `stock-kernel.config`, confirm the
   build reproduces the stock kernel (same banner, matching `Module.symvers` CRCs).
2. Build tier 1 (DEVTMPFS + FHANDLE). Compare CRCs for every symbol the vendor modules use.
3. Hardware session (with the user's OK): flash `boot_b`, boot, check that every vendor module loads, then try systemd
   as PID 1 on a copy of the init path (keep the Bash supervisor as the fallback).
4. Then audio (UCM), suspend, battery, one at a time, each with a test script.

Independent of the kernel work, and doable on the current system at any time:

5. Web apps as phone apps: Grok and X first (see "Product direction"), then whatever else the user uses daily.
6. Chrome smoothness on the phone: scrolling, keyboard, notifications. Measure before and after; the GPU vs software
   Chrome trade-off in `notes/gpu-bringup-20261007.md` still applies.
7. Later, optional: Waydroid for apps without a web version.
