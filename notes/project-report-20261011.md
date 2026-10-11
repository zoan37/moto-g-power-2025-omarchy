# Project report: Omarchy on the Moto G Power 2025 (2026-10-11, updated in the afternoon)

A snapshot for anyone picking this up (another Claude Code session, a collaborator, or the user later).
It covers what works, how the system is put together, what was learned on 2026-10-10/11, and how the
project can feed back into Omarchy and Linux in the long run. Detailed logs live in the dated
`notes/*.md` files referenced below.

## 1. The goal

**Short term:** make this phone a good pocket Omarchy computer for daily use. Web apps (X, Grok, …),
smooth touch UI, a terminal, all-day battery. Calls and SMS are out of scope (closed modem stack).

**Long term:** contribute back to **Omarchy** (userspace: phone/ARM UX, scripts, packages) and to
**Linux/Mesa** (drivers), so the work isn't a one-off hack for one phone.

**Working principle:** get everything working on this phone first, even if some of it is
phone-specific, but build it in a way that keeps the long-term path open (section 6).

## 2. How the system is put together

| Layer | What runs | Source |
|---|---|---|
| Bootloader | Motorola LK, Valhalla-patched on **both** slots (bootloader unlocked) | `notes/valhalla-20261006.md` |
| Kernel | Google GKI `5.15.180-android13-8`, rebuilt byte-for-byte as **tier 1** (+DEVTMPFS, FHANDLE, AUTOFS; all vendor-module CRCs unchanged) in `boot_b` | `notes/kernel-build-20261010.md` |
| Vendor drivers | 197 MediaTek/Motorola modules (vendor_boot + vendor_dlkm) | open source (GPL), Motorola publishes them |
| Firmware | VCP (video), WMT (Wi-Fi/BT), touch, GPU, modem… | closed binaries, loaded as Android does |
| Ramdisk | our `init_boot_b`: BusyBox `init` loads modules, picks the root, `switch_root` | `bringup/init`, `bringup/ufs-guard` |
| PID 1 | **systemd** (default since 2026-10-11; boot ≈ 9 s); `native-init` (Bash supervisor) remains the fallback after an unconfirmed boot | `notes/systemd-migration-20261011.md` |
| Root filesystem | Arch Linux ARM + Omarchy, **ext4 inside `userdata`** (24 GiB offset, `/dev/loop7`) | `notes/internal-storage-20261010.md` |
| Compositor | Hyprland on direct DRM, rendering on the Mali through Mesa panfrost-kbase (patched) | `notes/gpu-bringup-20261007.md`, `notes/direct-drm-latency-20261007.md` |
| Apps | Google Chrome (web apps via `--app`), foot, Omarchy shell (Quickshell) | |

Android's kernel **is** Linux. This port keeps Android's kernel and vendor drivers and replaces
Android's userspace (init, SurfaceFlinger, ART, HALs) with a normal Linux userspace: native-init or
systemd, Hyprland, Linux apps. That's the same model postmarketOS and Ubuntu Touch use on most phones.

**Why not a 6.x/7.x mainline kernel like the Lenovo Chromebook?** MT6835 has essentially no mainline
support (display, GPU-on-kbase, video codec, Wi-Fi, charger). The vendor modules only load on this
exact GKI build. Mainline would mean porting or rewriting every driver (section 6.3).

## 3. What works today

| Area | State |
|---|---|
| Boot | Internal UFS root, systemd as PID 1, **~9 s** from kernel to running (desktop a few seconds later). The microSD is a rescue copy (automatic fallback, or a `prefer-card` file) with a **red SD bar badge + critical notification** |
| A/B safety | `vegas-bootctrl` marks each good boot successful and drops slot a (old stock Android, which would format `userdata`) from the fallback order |
| Display | 1080×2388 DSI panel, **120 Hz** default (`vegas-refresh 60/90/120`), UI scale 3; **adaptive brightness** from the ambient light sensor; **auto-rotate** (tile) with touch following |
| GPU | Mali-G57 MC2 via Mesa panfrost-kbase (with this project's JM deferred-free + native fence FD patches). ARM libmali r48 (compat shim) is opt-in for Chrome |
| GPU clock | **Fixed**: GED loading mode 0 plus Android-style touch boost (`vegas-gpu-boost`). Before this the Mali never left 390 MHz |
| Scrolling | X at 120 Hz: ~92 fps (≈100 fps with autoplay off), up from ~41. KMS commits wait for render fences (no half-drawn frames; the user reports no ghosting/jumps) |
| Video | **Hardware decode in Google Chrome** (H.264/HEVC/VP9 ≤1440p) through this project's stateful VA-API driver `mtk_stateful` |
| Touch / keyboard | Native touch. wvkbd four-row phone layout with a **haptic tick**, auto show/hide on text focus |
| Phone shell | Status bar, nav bar (back/home/keyboard + swipes), home screen with open/pinned apps, **pull-down quick settings** (Wi-Fi picker, Bluetooth, DND, airplane, auto-rotate, brightness/volume, notifications), Omarchy popups fitted to 360 pt |
| Power key | Short press: screen off/on; long press: power menu; **double-tap to wake**; screen timeout 60 s |
| Sleep | **s2idle autosleep** while the screen is off; wake on power key, double tap, **Wi-Fi traffic (instant push)**, 15-min sync windows |
| Audio | Speakers (factory calibration), mic, earpiece/headset paths, PipeWire with realtime threads, headphone-jack switching |
| Bluetooth | BlueZ via a /dev/stpbt ↔ hci_vhci bridge (factory address), A2DP/HFP endpoints registered |
| Sensors | 22 via the SCP sensor hub (`/dev/hf_manager`): light, proximity, accelerometer, gyro, compass, barometer, Motorola rotation |
| Battery | % and temperature (kernel fuel-gauge mode), UPower |
| Vibration | Awinic LRA: keyboard ticks, notifications, power menu |
| Web apps | Chrome `--app` with one app per workspace (Grok, X pinned on the home screen) |
| Wi-Fi | WMT + wpa_supplicant (`vegas-wifi`), touch network picker |
| Health check | `vegas-doctor`: one command, ok/WARN/FAIL per subsystem (0/0 today) |

**Not yet:** camera, GPS, cellular (out of scope), a lock screen, a charge limit, Bluetooth audio tested with a real
headset.

## 4. This session's work (2026-10-10/11) and lessons

### 4.1 Storage: SD card → internal UFS
- The SD card was the reason the desktop froze: ~2.3 s per write, I/O stall 77%.
- **Motorola LK boots only its own 85-partition GPT.** A spec-valid extra partition caused
  "Failed to initialize partition table". Recovery: `fastboot flash gpt PGPT` from the firmware zip,
  which reset A/B to slot a (old Android, locked LK), then Valhalla LK on `lk_a`, re-unlock, `set_active b`.
- Final design: ext4 at a fixed offset inside `userdata` through a loop device. The GPT stays stock.
- **Never change the GPT on this phone.** **Linux must mark boots successful** (Android's
  boot_control HAL normally does), or the bootloader falls back to slot a after 7 boots.

### 4.2 Smooth scrolling (measured, not guessed)
- Harness: real finger swipes injected into the touchscreen evdev node + Chrome DevTools traces
  (`scripts/chrome-scroll-bench/`, `notes/chrome-scrolling-20261010.md`). It uses fresh profiles
  only. **Never script the user's real accounts** (the user flagged their X timeline).
- Fixes, biggest first:
  1. Hyprland `debug.vfr` on when rendering on the GPU. It had been off since the llvmpipe days,
     and the continuous 120 Hz redraw starved Chrome. Typing latency is unchanged on the GPU.
  2. **MediaTek GED bug:** loading mode 5 reads utilization fields that only CSF Malis fill. On this
     JM G57 the policy always saw 0% load, so the GPU stayed at its minimum clock. Mode 0 fixes it.
     A touch boost covers GED's slow ramp.
  3. Chrome GPU compositing by default; ARM libmali + GPU raster for Chrome (opt-in).
- No gain: ANGLE-on-Vulkan, uclamp on key threads, pinning the GPU process to the A76s, CPU
  `performance` governor.
- **Autoplay video** (software decode) is the biggest remaining jank source.

### 4.3 Hardware video decode (in progress, promising)
- The vendor V4L2 **stateful** decoder works from plain Linux: load `vcp.ko`,
  `mtk-vcodec-common.ko`, `mtk-vcodec-dec-v1.ko`, `vdec-fmt.ko`, then `mknod /dev/video0`.
- Supported codecs: H.264, HEVC and VP9 up to 2560×1440 (no AV1). Output is **MM21** tiles (the same
  layout as on the Chromebook).
- 1080p H.264 decodes at **64 fps** with dmabuf (`mtk_mm` heap) buffers, verified frame-correct
  (`bringup/vdec-test.c`).
- Driver quirk: MMAP bitstream buffers only work with `mtk_v4l2_dbg_level>=1`, because the driver
  fills `va` only in debug mode. That's a one-line fix if we rebuild the module from `kernel-mtk`.
- Route to the browser (`notes/video-decode-research-20261011.md`): **Arch Linux ARM's `chromium` is
  built with `use_v4l2_codec=true`**, and upstream Chromium already handles MM21 (libyuv/GL detile).
  Next: try ALARM Chromium with
  `--enable-features=AcceleratedVideoDecoder,AcceleratedVideoDecodeLinuxGL`, and port Raspberry Pi OS's
  small Chromium patches (sandbox device allow-list) if needed. Google Chrome would need a stateful
  VA-API driver (larger; VP9 first).

### 4.4 UX
- **Keyboard:** wvkbd covered 43% of the screen because `OMARCHY_SCALE` goes stale when the Display
  panel changes the scale. It's now ~26% tall, four rows, auto show/hide.
- **Bar:** Grok/X launchers and an SD-boot badge. **The bar still needs a phone redesign:** at 3×
  the clock overlaps the workspaces. Proposal: a slim status line on top, apps/workspaces in their own
  row or launcher.

### 4.5 Hardware tooling
- Servo button pusher design (Pico + 3× SG90/MG90S, printed plungers, firmware-timed releases):
  `hardware/button-pusher/README.md`. It's a SuzyQ-like recovery path, since LK error screens only
  respond to buttons.
- Software side: LK has `oem fb_mode_set` (always boot to fastboot), a possible future "safe mode".
- The Chromebook's USB webcam works for watching the phone screen only with `uvcvideo quirks=0x80`
  and YUYV ≤640×480.

### 4.6 Kernel track
- The boot kernel is Google's GKI ab13743836 and rebuilds **byte-identically** on the SER8 (x86_64,
  exact clang r450784e).
- Tier 1 (DEVTMPFS, FHANDLE, AUTOFS) keeps **all 2355** vendor-module symbol CRCs.
- Next: flash tier 1 to `boot_b` (stock `boot.img` kept), check all modules load, then try systemd as
  PID 1. That unlocks normal Omarchy services and udev, and removes the hand-made `/dev` nodes.

### 4.7 Afternoon of 2026-10-11 (systemd onward)
Each item has a dated note with the details and the measurements:
- **systemd** as PID 1 on the tier-1 kernel (`notes/systemd-migration-20261011.md`).
- **Audio** (`notes/audio-20261011.md`): 16 vendor modules (one more than planned: the PMIC efuse), UCM2, speaker
  calibration from the persist partition, the jack watcher, RTKit via a polkit rule (seatless session).
- **Suspend** (`notes/suspend-20261011.md`): MediaTek's pd/clk/alarm checkers need the display off and retries;
  wake-on-WLAN "any" with traffic-based grace, because the firmware can't drop broadcasts.
- **Bluetooth** (`notes/bluetooth-20261011.md`): hci_vhci built for tier 1; the bridge clears a feature bit the
  firmware advertises but rejects.
- **Sensors** (`notes/sensors-20261011.md`), **vibration** (`notes/haptics-20261011.md`), **phone UI**
  (`notes/phone-ui-20261011.md`: quick settings, power key, double tap, Omarchy patches).
- Lessons:
  - Hyprland reloads on config file changes, so pushes must be atomic.
  - Hyprland does not rotate touch with the output.
  - The desktop runs on its own D-Bus session (not the user bus).
  - Omarchy's menu skips `when:` guards in its PRoot mode.
  - Loud test tones are not OK: audible tests stay at about −80 dB.

## 5. Open vs closed: what we depend on

| Piece | Status | Notes |
|---|---|---|
| Kernel + vendor modules | **Open (GPL)** | `kernel-mtk`, `motorola-kernel-modules`; we read and could patch them (e.g. the vcodec MMAP fix) |
| Firmware (VCP, WMT, touch, modem) | Closed | Normal for Linux too (`linux-firmware`); loaded by LK or the drivers |
| GPU userspace | **Open by default** (Mesa) | ARM libmali is the only closed library in use, and it's optional |
| Android HALs (camera, audio, codec2) | Closed, **not used** | We replace them with Linux-standard interfaces (V4L2, ALSA/UCM, DRM) |

## 6. Long-term vision: contributing back

There are two upstreams, and they get different kinds of work:

### 6.1 Omarchy (userspace): realistic now
Candidates, roughly in order of how generic they are:
1. **Scale-aware on-screen keyboard sizing.** The stale `OMARCHY_SCALE` bug affects any Omarchy setup
   whose scale changes after login.
2. **Phone/touch UX for the shell:** an app-launcher bar widget (`vegas.apps`), a touch-friendly bar
   layout, keyboard auto show/hide, cursor `hide_on_touch`. Package them as optional shell plugins
   or a "mobile" profile, not phone-specific hacks.
3. **GPU-aware defaults for weak GPUs.** Lessons like vfr, GPU compositing in Chrome and measured
   refresh-rate choices, with the scroll benchmark as evidence. Possibly an Omarchy doc on tuning.
4. **ARM/aarch64 support polish** (packages, Chrome vs Chromium on ARM). This overlaps the
   Chromebook's existing upstream threads (omacom/omarchy#9211, discussion #14945).
5. A **fallback-boot notification pattern** (a loud SD/rescue badge) as a generic idea, if Omarchy
   grows rescue-boot support.

### 6.2 Mesa / other open projects: medium term
- **Mesa panfrost-kbase JM fixes** (deferred BO free while atoms are in flight, native fence FDs).
  These matter to anyone running Panfrost on vendor kbase kernels. Check whether upstream Mesa still
  carries a kbase backend before preparing patches.
- **Chromium Linux V4L2 bits.** If ALARM Chromium needs patches for this decoder (sandbox allow-list,
  MM21 handling), they're generic, like Raspberry Pi's.
- **Bug reports to MediaTek/Motorola**, not upstream Linux: the GED loading-mode bug on JM Malis, and
  the vcodec "MMAP needs debug level" quirk. They live in vendor trees.

### 6.3 Mainline Linux for MT6835: long term, big
What it would take (the model is Collabora's MT8189 Chromebook work, which the user already
contributed to): upstream device tree, clocks, power domains, pinctrl, PMIC, display (DSI + panel),
touch, UFS, Wi-Fi (MT6631/WMT), charger and gauge, and then Panfrost (mainline Mali driver) and the
mainline `mtk-vcodec`. That's months of multi-person work. The vendor sources are open and help as
reference, but they aren't upstream-quality. Worth tracking: whether postmarketOS or Collabora start
on Dimensity 6300 / MT6835-family chips.

### 6.4 How to work now so the long-term path stays open
- **Keep phone-specific glue isolated** (`port/vegas/…`, `vegas-*` scripts). Put generic
  improvements in separate small commits that could become Omarchy PRs.
- **Prefer standard interfaces** over Android-isms: systemd, udev, V4L2 + Chromium's Linux decoder,
  ALSA UCM, DRM/KMS. Then the userspace keeps working if the kernel later goes mainline.
- **Measure and write it down.** Every result in this report has numbers and a reproducible script.
  That's what maintainers need.
- **Upstream early, in small pieces.** One small PR (e.g. keyboard scale) beats a giant phone branch.
- Keep a running list of "upstreamable" items (sections 6.1–6.2) in the repo and update it as things
  land.

## 7. Suggested roadmap

**Next:**
- Bluetooth headset test (A2DP/HFP), and a battery-drain measurement from the autosleep log (asleep vs screen-off
  awake).
- Lock screen (only with a tested on-screen keyboard path to the PIN/password).
- Charge limit (Motorola's demo-mode limiter can't be turned off without a reboot; a proper one needs the
  charger IC's charge-enable control).
- Smoothness: an asynchronous fence wait in aquamarine instead of the CPU wait (the cost is ~2–4 points of late
  frames).

**Then:**
- First small Omarchy PRs: scale-aware keyboard sizing, the phone shell plugins as an optional "mobile" profile,
  phone-fit fixes like `omarchy-patches/*.patch`.
- GPS (MediaTek GNSS needs its proprietary `mnld` daemon: hard), camera (MediaTek ISP + HAL3: very hard).

**Later (months):**
- Build the button pusher and wire it into recovery scripts.
- Mesa kbase patches upstream (if a home exists).
- Watch and maybe join MT6835 mainline efforts.

## 8. Decisions for the user
- ~~Chromium vs Google Chrome for hardware video~~: decided Google Chrome; done through the stateful VA-API driver.
- How phone-specific should the shell become vs a generic "Omarchy mobile" plugin set?
- The Android dual-boot idea: the first 24 GiB of `userdata` is reserved, but making Android use only
  that part needs fstab changes in vendor partitions. Keep it parked?

## 9. Pointers
- Notes: `notes/internal-storage-20261010.md`, `notes/chrome-scrolling-20261010.md`,
  `notes/kernel-build-20261010.md`, `notes/video-decode-research-20261011.md`,
  `notes/handoff-kernel-20261010.md`, `hardware/button-pusher/README.md`.
- Commits: fa27c63 … (2026-10-10/11) on `main` (github.com/zoan37/moto-g-power-2025-omarchy).
- Afternoon notes: `notes/systemd-migration-20261011.md`, `audio-20261011.md`, `suspend-20261011.md`,
  `bluetooth-20261011.md`, `sensors-20261011.md`, `haptics-20261011.md`, `phone-ui-20261011.md`.
- Phone access: USB network `192.168.77.1:8080/cgi-bin/command` (root shell CGI, USB link only).
