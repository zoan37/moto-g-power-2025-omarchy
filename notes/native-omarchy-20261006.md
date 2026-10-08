# Native Omarchy on Moto G Power 2025

Verified 2026-10-06 local time (2026-10-07 UTC). The owned XT2515-1 / vegas now
boots Arch Linux ARM directly from the 32 GB Samsung microSD and runs the real
Omarchy ARM desktop. Persistent native boot and its 40 C battery-temperature
guard are configured. The 2026-10-07 phone UI update is installed and its larger
keyboard and bar safe insets pass native/live webcam checks. Cable-aware shutdown
and normal power-on passed one full cycle; see [the update record](phone-polish-20261007.md).

## Current graphics update — 2026-10-07

The running desktop now uses patched Mesa/Kbase and direct KMS Hyprland at
120 Hz, with a phone UI scale of 2.4. Wi-Fi works through the stock MediaTek
modules and a native WMT loader. The initial Weston/llvmpipe measurements
below describe the first successful desktop, not the current GPU path.
See [GPU bring-up](gpu-bringup-20261007.md) for driver prerequisites and tests.

## First desktop verified on the phone

- All 58 desktop archive stages copied; all 69,374 final regular-file hashes
  matched before applying user overrides and performing first-boot setup.
- PID 1 is /usr/bin/bash running the custom native supervisor; / is
  /dev/mmcblk0p1[/omarchy], ext4. Android userspace and PRoot are not running.
  The bundle compatibility variable selects its service-free launchers.
- Stock 5.15.180 Motorola kernel/modules; Weston 15.0.1 uses the real DSI DRM
  output at 1080x2388 / 60 Hz with Pixman. Nested Hyprland 0.56.1 uses Mesa
  llvmpipe, 1080x2388 / 30 Hz, scale 1.5. Quickshell 0.3.1 runs Omarchy's bar.
- Live Hyprland reload passed with no config errors. The bar and background
  are opaque, Foot is mapped, and wvkbd is visible. Native PNGs and private
  SER8 webcam stills document the desktop on the real phone display.
- Matching Ilitek module plus stock ILITEK_FW_77600 initializes the controller.
  The first-contact bridge maps its type-B events to an absolute click pointer.
  A synthetic contact through the actual Ilitek evdev device clicked "a" on
  wvkbd and typed into Foot. Clicking "Keys" through that same pipeline hid
  and restored the keyboard. Virtual keyboard input executed uname -m and
  displayed aarch64 in the terminal. On 2026-10-07 the user also confirmed
  physical typing, reporting small keys and a slight delay.
- The persistent marker survived a normal user-requested reboot. A changed
  private boot ID proved the reboot, and the native root, desktop and bridge
  returned. The original three-minute test deadline also passed without a
  restart while persistent mode was enabled.
- The corrected bounded desktop test returned to fastboot and restored exact
  stock init_boot_b automatically. Fresh stock Android recovery was verified
  before leaving the final native boot running.
- Observed native battery temperature stayed 29–30 C. Battery capacity reports
  -1, so charge percentage and sustained charging are not established.

## Use

Keep the microSD inserted. Linux starts directly into the omarchy user's
portrait desktop without a login or screen unlock. Tap **Keys** in the top
bar to show/hide the on-screen keyboard. Single-finger input is implemented
as pointer movement and clicking; multitouch gestures are not implemented.
The hostname is moto-vegas and the terminal starts in the user's home.

### Power off (updated 2026-10-07)

Select **Shutdown** in Omarchy or run `omarchy-system-shutdown`. The Power off
prompt waits up to 90 seconds for USB and other external chargers to be unplugged.
After three disconnected readings, it requests poweroff. Close the prompt or
press Ctrl+C to cancel while waiting. Connected or unreadable power states are
rejected by the root helper as well. This flow requires disconnecting external
power; reboot and fastboot do not.

One full cycle passed on 2026-10-07: unplugging led to a continuously black screen
for over a minute, and normal Power-button startup returned to native Omarchy.
The previous-boot record confirms poweroff with all charger sources offline.
The earlier plugged-in shutdown restarted the phone; its exact kernel/firmware
cause is not established. Repeated cycles and already-unplugged shutdown are
additional tests, and reliable plugged-in kernel poweroff is not claimed.

The verified fallback remains `vegas-power fastboot`: wait for the bootloader
menu, disconnect USB/charger, use volume buttons to select **Power off**, and
press Power to confirm. The user previously confirmed this kept the phone off.

The stock kernel lacks devtmpfs, VT and SysV IPC. This port uses a Bash boot
supervisor, udev, seatd without VT binding, Weston and Hyprland. It does not
provide a standard systemd system/user boot. Native reboot uses a fixed
request queue and a direct root helper. General Omarchy service operations
still need porting.

## Working image and source

- Preserved image: artifacts/vegas-linux-bringup/native-desktop/init_boot.vegas-native-desktop.img
- SHA-256: f23b3846a366dabe24247d66e7114df1d50ba9520bf423435a86f63dc8fe5ee1
- Preserved verification manifest: artifacts/vegas-linux-bringup/native-desktop/manifest.json
- Post-reboot desktop screenshot: artifacts/vegas-linux-bringup/native-desktop/desktop.png
- Phone overrides: port/vegas/root (user configs and /usr/local helpers).
- Desktop payload: pinned omarchy-android 0.1.1 ARM rootfs, source SHA-256
  dbee00c0cb41f6b213c153ff4dc1c0898ba99dd0543ac13cef1601820366e2c5.
- The extra signed Weston/Xorg/colord/libgusb package files have verified
  contents, but their pacman database registrations remain to be completed.
  The software stack is pinned; general upgrades are not yet validated.
- The longer investigation and failed intermediate experiments are recorded
  in linux-bringup-20261006.md. No host desktop config was changed.

## USB development access

The native gadget has a fixed host MAC 02:00:00:00:77:02. The isolated link uses
host 192.168.77.2/30 and phone 192.168.77.1; it adds no default route or DNS.
The in-memory NetworkManager profile `vegas-native-usb` is set to reconnect to this interface.
It disappears on a laptop reboot. The root development HTTP service is bound
only to 192.168.77.1:8080. Native Linux has no Android ADB server.

If the laptop's USB address is absent, identify the interface with that MAC
and create an in-memory, interface-specific NetworkManager connection using
manual 192.168.77.2/30, ipv4.never-default=yes, ipv4.ignore-auto-dns=yes and
IPv6 disabled. Status is available at /cgi-bin/status. /cgi-bin/command
accepts a local Bash script POST (maximum 131072 bytes) and executes as root.
This is a development service for the owned phone, not a public API.

## Return to stock Android

From the connected laptop in this project directory:

```bash
curl --noproxy '*' http://192.168.77.1:8080/cgi-bin/return-fastboot
fastboot getvar product
fastboot getvar current-slot
sha256sum backups/vegas-W1VES36H.10-12-1/init_boot.stock.img
```

Check vegas and slot b. The stock image must match:
0874d125f5fe4be612c8c61a6467f2a24988d2688cd4c1beec2b093fc179c15a.
Then restore only the native Linux init_boot replacement:

```bash
fastboot flash init_boot_b backups/vegas-W1VES36H.10-12-1/init_boot.stock.img
fastboot reboot
```

The Valhalla LK and unlocked bootloader stay in place. The original Android
build remains W1VES36H.10-12-1. Linux recovery does not require restoring LK
or other unique partitions. Matching firmware and device-specific backups
are preserved in ignored firmware/ and backups/ directories.

To re-enter Linux from recovered Android, the diagnostic builder's --desktop
mode recreates a CID-bound image from the verified SD checkpoint. The runner
--usb-network --native-desktop --keep-desktop performs model/build/slot/unlock
and image checksum checks before flashing init_boot_b. See its --help.

## Remaining hardware work

Fast physical typing and the prepared UI/shutdown changes, Wi-Fi, cellular,
audio, phone cameras, suspend,
power-off behavior, reliable battery percentage and sustained charging remain
unverified. Graphics currently use CPU software rendering. This is a native
desktop bring-up port; it is not yet a complete daily-use phone OS.
