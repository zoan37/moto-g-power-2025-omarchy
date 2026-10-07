# Shutdown and phone UI update

Installed on 2026-10-07 and rebooted into native Omarchy. The larger keyboard
and inset bar pass live checks and webcam inspection. One cable-disconnected
shutdown and normal power-on cycle passed. The user likes the keyboard size
but still reports noticeable typing delay. Instrumented 30/60 Hz comparisons
and bounded renderer experiments now quantify the remaining repaint delay.
The working native boot image, original overlay and Android recovery image
remain preserved.

The user confirmed physical typing in the terminal, reported slight typing
lag and small keyboard keys, and observed clipped bar controls at the rounded
top corners. Omarchy Shutdown restarted the phone; entering fastboot,
disconnecting USB, and selecting Power off successfully kept it off.

## Display geometry

[Motorola's specifications](https://en-us.support.motorola.com/app/answers/detail/a_id/184283/p/30)
list a 6.8-inch LCD, 1080 x 2388 pixels, 387 ppi, 19.9:9 aspect ratio and
120 Hz maximum panel refresh. The body is 166.62 x 77.10 x 8.72 mm. Our
verified native DRM mode is 60 Hz; the nested desktop currently runs at 30 Hz.
The nominal 120 Hz specification does not mean this port supports 120 Hz.

The session uses scale 1.5: 720 x 1592 logical pixels. At the published pixel
density, 1 mm is about 15.24 physical pixels or 10.16 logical pixels. These
physical estimates use rounded manufacturer density, not a ruler measurement.

| Element | Previous | Installed update |
| --- | --- | --- |
| Keyboard height | 280 logical / 420 physical pixels, about 27.6 mm | 450 logical / 675 physical pixels, about 44.3 mm |
| Main keyboard rows | Six, including utility and number rows | Five: number row, three letter rows, controls/space |
| Nominal row height | About 70 physical pixels / 4.6 mm | About 135 physical pixels / 8.9 mm |
| Keyboard font | Sans 14 | Sans 20 |
| Bottom safe space | None | 32 logical / 48 physical pixels, about 3.1 mm |
| Bar top inset | None | 56 logical / 84 physical pixels, about 5.5 mm |
| Bar side insets | Existing internal widget padding only | 20 logical / 30 physical pixels on each side, plus internal padding |
| Bar height | Theme default 26 logical pixels | At least 48 logical / 72 physical pixels |

This gives larger phone-sized key targets. It retains Ctrl and Esc,
provides symbols and a full utility layout through the layout key, and keeps
the same Keys toggle. It does not add Android/iOS prediction or autocorrect.

The exact camera-hole bounds and corner radii are not measured. Native
geometry and the SER8 webcam confirm the bar sits below the camera hole and
clear of the rounded top corners at these conservative insets. These are
observed fit values, not manufacturer-provided cutout coordinates. The user-owned bar clone preserves Omarchy widgets and its menu;
packaged /usr/share/omarchy files are unchanged. Insets can be adjusted under
bar.phoneSafeTop, bar.phoneSideInset and bar.phoneBarHeight in shell.json.
The layer-shell margins reserve usable application space as well as moving
the panel. Live reserved areas are 104 logical pixels at top and 482 at bottom.
The bar is x=20, y=56, width=680, height=48; the keyboard is x=0, y=1110,
width=720, height=450, leaving 32 logical pixels below its bottom edge.

## Shutdown handling

The previous request reached power-transition as poweroff. The failure is
below menu dispatch; its exact kernel/firmware cause is not established.
USB-triggered charger boot is a hypothesis, not a confirmed diagnosis.

The desktop Shutdown command now opens a small terminal prompt. Its user
process waits up to 90 seconds for external power to disappear, then requires
three disconnected samples before queuing one complete poweroff request.
Closing the window or Ctrl+C cancels while waiting. Unknown readings and a
timeout cancel without leaving a queued shutdown. The root helper rechecks
external power immediately before the existing BusyBox poweroff -f syscall.

The checker reads power-supply ONLINE, not battery STATUS: the prior phone
could report Not charging while USB VBUS was still present. Motorola's
[charger source](https://github.com/MotorolaMobilityLLC/kernel-mtk/blob/72b5d32be0bb7cee12765bf236972f8f4c622b83/drivers/power/supply/mtk_charger.c)
defines mtk-master-charger with type Unknown and its ONLINE getter checks
charger existence, so that named supply is explicitly recognized. Other USB,
Mains and Wireless sources are recognized too; the battery is excluded. This
release source is a reference and does not establish the exact deployed
kernel/firmware shutdown behavior.

Waiting never holds the root power-transition lock, so the 40 C temperature
guard can still enter fastboot. Reboot and fastboot keep their direct helpers
and do not require unplugging. No compositor/device teardown or charging
configuration change was added. A returned/failed helper releases its lock,
and the request watcher keeps running after rejected requests.

Private /var/log/vegas/power/ records retain the pre-transition boot ID,
power-supply readings and next boot's filtered power/charger context. This
will help distinguish charger boot from another reset if unplugged shutdown
still restarts. Boot IDs remain local/private.

## Typing delay

The input bridge has no deliberate sleep between an input frame and its
Wayland flush. The keyboard already has the immediate-feedback patch enabled.
Timing now points to the render/presentation path: most injected contacts reach
the raw PTY reader in about 2–5 ms, and Foot commits about 1 ms after a key
event, while its compositor frame callback arrives roughly 110 ms later at
full resolution. These are software timestamps, not physical tap-to-light
measurements. Cold-keymap and occasional scheduling outliers also occur.

Installed improvements:

- Rebuild the keyboard at -O2; the prior cross-build did not request optimization.
- Disable the extra key popup while keeping pressed-key highlights and
  immediate drawing. This avoids a second feedback surface during typing.
- Set Foot's delayed-render-lower/upper to zero so echoed characters are
  submitted immediately. The default short batching delay is only one part
  of the pipeline; this does not promise a large overall improvement.
- Add vegas-refresh 30|60|status for a controlled comparison on the next boot.
  The default stays at the proven 30 Hz. Both Aquamarine's backend pacing and
  Hyprland's monitor declaration change together. Do not assume 60 Hz is
  faster until checking actual presentation, CPU load and temperature.

Continuous frame pacing (debug.vfr=false), damage tracking, four llvmpipe
workers and the stock thermal limits stay as previously verified. A higher configured rate cannot remove CPU rendering
cost or prove actual achieved frame rate. Hardware GPU rendering remains a
separate porting task.

### Measured trials and rollback

`scripts/measure-vegas-input.py` launches a temporary Foot timing window and
sends twelve synthetic contacts through the real Ilitek event device. Its
trace is restricted to that window and stores timestamps, not typed contents.
Private JSON reports preserve all samples and callback correlations.

| Trial | Median contact-to-PTY | Median Foot commit-to-frame-callback |
| --- | ---: | ---: |
| Full resolution, 30 Hz | 2.48 ms | 114.35 ms |
| Full resolution, 60 Hz | 2.38 ms | 116.25 ms |
| Temporary performance governors, 60 Hz | 2.24 ms | 113.19 ms |
| 720 x 1592 render target, 60 Hz; physically shrank desktop | 4.79 ms | 68.07 ms |
| 540 x 1194 target with Weston scale 2; full physical size, softer text | 3.49 ms | 85.37 ms |
| Same half-resolution layout, 100 ms tap interval | 3.53 ms | 88.58 ms |
| Same layout, VFR enabled | 75.46 ms | 128.32 ms |
| Rebuilt Aquamarine forwarding partial damage, full resolution/60 Hz | 2.79 ms | 109.58 ms |

The performance-governor and 60 Hz trials gave no material improvement.
Lower resolution trades sharpness for some improvement, and VFR made this
setup worse. The Aquamarine patch builds and runs but did not demonstrate a
useful latency improvement; it is an experiment, not an installed fix.
Original Aquamarine SHA-256
`8a2a2cbe95a56b16ec986acf8ba4e8d6b5c737f607ff3482c62c759983ee135e`
and the original desktop launcher were restored using
`/var/lib/vegas/render-updates/damage-20261007/rollback.sh`, followed by native
reboot. Full 1080 x 2388 output, scale 1.5, 30 Hz nested refresh,
`debug.vfr=false`, four llvmpipe workers and schedutil governors are retained.
The larger keyboard and inset bar remain installed; fresh configerrors is empty.

### GPU investigation

The stock kernel has `CONFIG_DRM_PANFROST` disabled. Its original vendor_dlkm
contains `mali_kbase_mt6835.ko`, r38p1, with Job Manager API 11.38. A checked
one-boot load of original dependencies succeeded. The initial probe needed
two additional device-tree supplier modules, `mcupm` and `fhctl`, which do
not appear as direct symbol dependencies of the GPU driver. After those loaded,
the Mali device bound and exposed `mali0`, major 10/minor 116. A read-only
version/property query returned API 11.38, product ID `0x9093`, shader mask
`0x5` (two cores), and maximum frequency 1,100,000 kHz.

This establishes kernel access to the GPU, not hardware-accelerated Omarchy.
The desktop continues to use llvmpipe. An isolated experimental Panfork/Kbase
userspace build is being investigated; no system Mesa replacement, automatic
GPU module startup, boot-image replacement or firmware change was performed.
`prepare-vegas-gpu-modules.py` checks source conflicts, dependency order,
original module ABI and SHA-256. `probe-vegas-stock-gpu.py --load` checks the
native root/kernel/hostname/private SD checkpoint, uploads checked originals
to /run and skips already loaded modules. Native reboot discards this trial.

## Checks completed offline

- Fake-sysfs shutdown tests cover USB, unknown/missing/invalid readings,
  battery exclusion, wireless power, unplug progression, timeout cancellation,
  duplicate requests and atomic queue writes. Recording backends cover root
  rejection, helper failure/lock cleanup and fastboot without unplugging.
- Bash syntax checks pass. Lua syntax and live hyprctl reload/configerrors pass.
- Optimized ARM keyboard builds and its help/layer listing execute under
  QEMU; phone layout and bottom margin are present.
- Cloned bar QML parses with qmlformat. Native module loading, rendered
  geometry and webcam camera/corner clearance checks pass after the Loader fix.
- Foot config passes a local check with its phone theme include omitted.
  The full native theme/config combination also starts successfully on the phone.
- Candidate archive entries, ownership, executable modes and SHA-256 checks
  pass. The guarded installer also passed on the real phone.

## Live validation on 2026-10-07

The 18-file SD userspace update installed and native reboot succeeded. The
first live run exposed two issues: the custom bar Loader injects properties
after construction, so required QML properties prevented loading; the new
keyboard margin parser needed braces around its multi-statement die macro.
Both are corrected in the reproducible builders. Margin parsing now has
valid/invalid regression checks under ARM QEMU.

Live Hyprland reload succeeds and configerrors is empty. The corrected bar
and five-row keyboard map at the geometry above. Native screenshots and
private webcam captures confirm the layout. An injected contact through the
real Ilitek event device typed a into Foot; contacts on Keys hid and restored
the keyboard. Root poweroff rejects a connected cable without leaving a lock
or request. The desktop Power off prompt opens and waits without queuing a
request while USB is connected. Battery temperature was 23 C before shutdown and 24 C after power-on.

The user unplugged USB while the desktop prompt was waiting. The screen stayed
black throughout a 65-second webcam recording, with no boot logo or desktop
return. The user then pressed Power and reconnected USB. Fresh native health
checks passed, and the saved last-request record confirms ACTION:poweroff from
the previous boot with all external ONLINE values zero (including
mtk-master-charger and primary_chg). This establishes one successful unplugged
shutdown/power-on cycle. It does not establish that USB-triggered charger boot
was the sole cause of the earlier restart, or that plugged-in kernel poweroff
works. The new flow deliberately requires disconnecting external power.
Shutdown starting with the cable already absent and repeated cycles can still
be tested later; fastboot remains the previously verified fallback.

The original pre-update rollback is
/var/lib/vegas/phone-updates/66a6652bb9b1e1dc/rollback.sh. A second backup at
/var/lib/vegas/phone-updates/987e77d1a33dc329/rollback.sh contains the first
installed candidate before the two startup corrections. Prefer the original
backup to return to the earlier working UI.

## Apply or repeat hardware tests

Boot the native phone and reconnect its isolated USB link. In the project:

```bash
python scripts/install-vegas-phone-update.py
python scripts/install-vegas-phone-update.py --apply --reboot
```

The first command only checks the local archive. Applying checks the exact
native root/kernel/hostname and the privately recorded microSD CID. It sends
bounded CGI chunks, checks hashes, rejects symlink destinations/ancestors,
backs up changed files, and provides per-file rollback. It writes only SD-root
userspace; no bootloader or boot partition flash is involved. A reboot is
needed to load the updated power-request watcher and start the new bar and
keyboard. The installer prints its private phone backup path.

Then validate:

1. Clean hyprctl reload/configerrors, mapped new bar and keyboard, visible
   controls clear of the actual rounded corners/camera, and correct app margins.
2. Physical quick typing and Keys show/hide. Compare the original screenshot
   with the new native screenshot and webcam view.
3. Read power-source-state while connected. Select Shutdown, verify the unplug
   prompt, disconnect USB and confirm the phone stays off for at least 60 s.
   Also test Shutdown with no cable attached. The cable-removal test and normal power-on
   passed once on 2026-10-07. If a repeat restarts, preserve the power logs.
4. Check timeout/cancel and a subsequent normal reboot. Test fastboot recovery
   after the updated root helper; the former version was verified separately.
5. If typing still feels slow, compare vegas-refresh 60 plus reboot with 30,
   observing achieved frame pacing, CPU use, temperature and key feedback.
   Restore 30 if it is less stable or increases lag.

Rollback is the generated backup/rollback.sh on the native phone, followed
by a native reboot. The tested init_boot image and original overlay remain
under artifacts/vegas-linux-bringup/native-desktop/ and the original
desktop-overlay.tar.gz. Exact Android recovery is still documented in
native-omarchy-20261006.md.
