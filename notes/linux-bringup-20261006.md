# First native Linux boot — Moto G Power 2025

The owned XT2515-1 (`vegas`) successfully ran a native BusyBox Linux diagnostic
ramdisk on 2026-10-06. A USB ACM shell reported `uid=0 gid=0` and the exact stock
kernel, `5.15.180-android13-8-00021-g46a5565a0982-ab13743836`, with no Android init
or userspace running. This is a hardware bring-up milestone; Arch Linux ARM,
Hyprland and Omarchy are not yet running on the phone.

Only `init_boot_b` was replaced for the diagnostic experiment. The matching
stock image was restored afterward. Fresh ADB reads confirmed Android boot
completed, the original `W1VES36H.10-12-1` fingerprint and slot B, and the
bootloader remained unlocked (`flash.locked=0`, verified boot `orange`).

## Verified inputs and observations

- Downloaded `boot`, `init_boot`, `vendor_boot`, `dtbo`, `vbmeta` and
  `vbmeta_system` from the current-build TMO archive. ZIP CRCs and each Motorola
  flash XML MD5 matched. These stock copies are archive images, not device
  partition readbacks. They are saved separately under
  `backups/vegas-W1VES36H.10-12-1/` with SHA-256 manifests.
- The downloaded kernel's full build banner matches the running Android
  kernel. Boot header v4 contains only the kernel; `init_boot` has the generic
  ramdisk; vendor boot v4 contains the device tree and 197 hardware modules.
  This layout matches [AOSP's generic boot description](https://source.android.com/docs/core/architecture/partitions/generic-boot).
- The running kernel config was captured from `/proc/config.gz`. USB configfs,
  ACM/ECM/NCM, DRM, evdev, ext4 and modules are enabled. Devtmpfs and virtual
  terminals are disabled. The diagnostic init creates device nodes from sysfs.
- A signed Arch Linux ARM BusyBox package supplied the static ARM64 binary.
  Signature fingerprint is `68B3537F39A313B3E574D06777193F152BDBE6A6`.
- Native USB shell worked through UDC `11201000.usb0`. No `insmod` errors were
  found in the captured module-load log. Internal UFS storage and the real GPT
  partition names were enumerated without mounting storage.
- Native DRM exposed `card0-DSI-1`, status `connected`, advertising
  `1080x2388` modes. It was disabled; rendering has not been demonstrated.
- Only power/volume key devices were available. Android's touch modules live
  in vendor_dlkm and were not included in this first ramdisk.
- Battery temperature reads were 30–31 C. Health was `Good`, charging status
  `Not charging`, but capacity was `-1` and other gauge fields incomplete.
  This does not establish safe sustained operation or charging. The image has
  a three-minute restart limit and a 40 C temperature guard.

## Problems discovered and corrected

`fastboot boot` uploads an image and returns `OKAY`, but also reports
`command is not implemented!`. It did not boot from RAM. This unit needs a
temporary partition replacement for these experiments; do not treat the
fastboot exit status alone as success.

The vendor ramdisk's initial module-load list omits `reboot-mode.ko` and
`syscon-reboot-mode.ko`, although both files are included. Android normally
loads these later. Without them the three-minute restart booted the diagnostic
Linux image again instead of entering fastboot. Loading both from the native
shell made `reboot(RESTART2, "bootloader")` enter fastboot, and stock restoration
succeeded. The next builder revision includes them explicitly.

BusyBox `cttyhack` does not recognize ttyGS0. Its warning and early terminal
echo caused shell-input feedback and unmatched quotes after reconnecting USB.
The revised image uses BusyBox `script` to bridge the USB stream to a proper
PTY; it produced a root shell on hardware. Its timed return to fastboot was
also verified, followed by stock restoration.

## Backup and display-tool follow-up

USB ECM networking was subsequently verified on the actual phone alongside
the USB serial shell. The phone serves a small diagnostic CGI endpoint only on
`192.168.77.1:8080`. The host uses an in-memory, interface-specific
NetworkManager profile on `192.168.77.2/30`, with no default route or DNS change;
the runner deletes that profile after the experiment.

**Seventeen genuine partition readbacks** are now stored privately in ignored
`backups/vegas-device-readbacks/`: nvram, nvcfg, nvdata, persist, prodpersist,
protect1, protect2, proinfo, cid, utags, utagsBackup, lk_b, boot_b, vendor_boot_b,
dtbo_b, vbmeta_b and vbmeta_system_b. Each compressed stream was decompressed
and checked against the source size and SHA-256 calculated on the phone.
The actual boot_b, vendor_boot_b and dtbo_b readbacks match the current-build
TMO archive byte for byte. No device-specific partition was written or mounted.

The first unpaced serial gzip/base64 transfer was corrupt. Pacing allowed two
verified transfers; nvdata exceeded the serial timeout. USB networking completed
the remaining transfers without relying on the serial stream for bulk data.
The network status endpoint reported native root, the exact stock kernel,
29 C and battery status `Full`. This remains a short diagnostic, not a sustained
charging validation.

Built libdrm **2.4.134 modetest** for ARM64 without Cairo, using the official
source archive, a signed Arch Linux ARM libdrm package, and the verified ARM
glibc sysroot. Reproduce with `python scripts/build-vegas-drm-tools.py`, then
`python scripts/build-vegas-diagnostic.py`.

The first network display query failed to load libdrm because the libraries
were staged in `/lib`, while the Arch loader's search path used `/usr/lib`.
An explicit `/lib` runtime search path fixed that; the staged ARM help command
also passes without LD_LIBRARY_PATH. A hardware retest loaded the program but
failed to open the device: this upstream version interprets `-D` as a bus ID,
not a device path. No color-bar test ran during either failure. The reproducible
builder now records a small local change to open an absolute device path
explicitly. Diagnostic init also supplies the primary DRM node when the vendor
class omits it, conditional on DRM major 226 and the observed card0 connector.
The device-opening changes succeeded on hardware: libdrm queried the MediaTek
DRM driver, DSI connector 32, CRTC 60 and primary plane 35. The panel advertises
1080x2388 at 60/90/120 Hz. The attempted atomic pattern returned success but
its log showed no modesetting. Source inspection found this libdrm release
silently skips atomic `-s` without a `-P` plane argument. The prepared script
now selects the live CRTC/primary plane and includes `-P`, using 60 Hz. This
latest pattern command has not yet run on hardware; rendering remains unverified.

After the loader-fix experiment, stock `init_boot_b` was restored and fresh ADB
verified Android boot completion, the original fingerprint, slot B and unlocked
bootloader. No diagnostic NetworkManager profile remained on the laptop.

The user subsequently inserted a **32 GB Samsung microSD card** into the phone.
Android detected it as `disk:179,0`, total **32,010,928,128 bytes**, with an
existing Raspberry Pi installation (recovery/boot FAT volumes plus additional
partitions). Existing contents must be resolved with the user before preparation;
no card partition or file was written by our tooling. A subsequent native Linux
read-only survey confirmed `/dev/mmcblk0` has the identical capacity, with
RECOVERY FAT p1, SETTINGS ext4 p5, boot FAT p6 and root ext4 p7 (27.2 GiB).
No card filesystem was mounted during that experiment.

After the card/resource-query experiment, the runner successfully restored
stock `init_boot_b` and requested Android reboot. ADB did not reappear during
the follow-up verification window. A physical USB reconnection was requested;
latest Android boot verification is pending, rather than assumed from the
successful fastboot restore. No diagnostic host network profile remains.

A signed-base **8 GiB sparse Arch Linux ARM ext4 image** is now prepared at
`artifacts/vegas-linux-bringup/arch-rootfs/vegas-arch-rootfs.ext4`, with saved
archive ownership metadata, a clean read-only e2fsck result and a SHA-256
manifest. It contains ARM userspace, not a demonstrated phone desktop. The
phone must retain its matching stock kernel/modules; the generic archive's
kernel is not a Moto kernel replacement. No image has been written to the card.

## Reproducible artifacts

- Builders: `scripts/build-vegas-diagnostic.py` and `scripts/build-vegas-rootfs.py`; neither contacts the phone or card.
- Native init and reboot helper: `bringup/init`, `bringup/to-bootloader.S`.
- Bounded phone experiment and stock restore: `scripts/try-vegas-diagnostic.py`.
  It checks the exact Android fingerprint, unlock state, slot and checksums;
  it only writes `init_boot_b`. Use the recorded firmware and do not run this
  on another unit/build without a separate audit.
- First successful image and exact init snapshot:
  `artifacts/vegas-linux-bringup/first-native-boot/` (ignored by Git).
- Raw shell/driver/partition logs and restoration verification:
  `private/linux-bringup/diagnostic-attempt/` (ignored, private).

## Next work

1. Resolve whether the existing Pi data should be preserved; native Linux
   microSD visibility and read-only filesystem identification are verified.
2. Finish the DRM resource query and short test pattern before attempting a
   compositor. Obtain matching vendor_dlkm touch drivers and firmware.
3. Prepare a signed Arch Linux ARM root filesystem image on the host, resolve
   the existing Pi data with the user, then provision the confirmed card.
4. Bring up a minimal compositor, power/gauge management and touch input, then
   adapt Omarchy ARM. Wi-Fi, audio, suspend and cellular remain separate work.

## microSD provisioning continuation

The user confirmed the Pi files are disposable and requested provisioning
through the phone, without a USB card reader. Fresh ADB verified the original
Android fingerprint, slot B, unlocked bootloader and completed boot.

The signed Arch archive was repacked into 34 tar stages (885,570,033 compressed
bytes; 2,112,939,831 file-data bytes) preserving archive UID/GID, modes and links.
A manifest records each stage's SHA-256 and hashes for 33,457 regular files.
The native card-only CGI checks SD type, exact 32,010,928,128-byte capacity and
the card CID on every request; it accepts no arbitrary block-device path.
Extraction runs in a chroot confined to the card. Completed stages are synced
and checkpointed privately; the runner can continue through bounded native
boots before restoring Android. Final verification checks every regular file,
Arch ARM userspace commands, then an unmounted read-only e2fsck.

The planned MBR is one Linux partition, start sector 2048, length 62,519,296
sectors. Native formatting uses ext4 without orphan_file, with metadata/journal
initialization completed synchronously. Regular-file fixtures verified the
layout, ARM formatter/loader and filesystem check; isolated CGI checks rejected
GET formatting/writes and invalid card identities.

The first provisioning attempt wrote the new partition table, but the formatter
failed before any Arch archive stage. Stock init_boot_b was restored. The
formatter's stderr had not been included in the CGI response; the next revision
captures it and reports tool/loader details for diagnosis. Provisioning remains
in progress; the card is not yet ready to boot Arch.

The user also requested avoiding Android's manual unlock after reboot.
`locksettings set-disabled true` was accepted, but `get-disabled` still returned
false, so no successful screen-lock removal is claimed. The user was directed
to select None under Security & privacy > Device unlock > Screen lock, entering
the current credential only on the phone. This follows the
[Motorola Android 16 guide](https://help.motorola.com/hc/3592/16/pdf/help-moto-g-power-2025-16-na-en-us.pdf#page=303).

The stderr-enabled retry identified the formatter problem: BusyBox ash chose
its standalone mke2fs applet for an unqualified command, although the full
e2fsprogs executable existed at /bin/mke2fs and its explicit version query
worked. The applet rejected `-t`. The prepared CGI now invokes `/bin/mke2fs`
and `/bin/e2fsck` explicitly. The exact BusyBox shell with the staged root
verified the full formatter selection. Stock init_boot_b was restored after
the retry; the card still has no transferred Arch stages. Android unlock/USB
reconnection is pending before the corrected provisioning run.

The user selected Screen lock > None; fresh ADB `get-disabled` returned true.
The corrected provisioning run then successfully formatted and mounted the
32 GB card as the single `vegas-arch` ext4 partition. Checked archive transfers
are now in progress. The formatter issue is resolved on hardware; native Arch
userspace and complete installed-file verification remain pending.

**Completed card preparation:** All 34 archive stages were transferred over
seven bounded native boots. The final verifier matched all 33,457 regular-file
hashes. Arch ARM's /bin/bash ran natively from the microSD under the stock
5.15 kernel, within the BusyBox diagnostic init's chroot; `uname -m` returned
aarch64, /etc/os-release identified Arch Linux ARM, pacman reported 7.1.0 and
systemd's version command reported 261.2. This is native Arch userspace, not
yet a systemd PID 1 boot or desktop session. Unmounted e2fsck completed cleanly:
47,286 inodes used out of 489,472; 615,413 blocks used out of 7,814,912.

Stock init_boot_b was restored afterward. Fresh ADB confirmed the original
Android build, slot B, completed boot and unlocked bootloader, with screen
lock disabled (true). ADB reappeared without another manual unlock. The
progress/checksum records are private; no internal phone filesystem was
mounted or written by the card preparation.

## Storage comparison

A native read-only sequential test used 64 MiB from boot_b and 64 MiB from
the microSD partition with BusyBox dd `iflag=direct`, writing only to /dev/null.
The dd timings were 0.196220 s internally (326.2 MiB/s) and 0.781423 s for
microSD (81.9 MiB/s): approximately 4.0x faster internal sequential reads.
This is a small direct-I/O sample, not a write, random-I/O or application-launch
benchmark. The user elected to continue with microSD for now.

The optional firmware-metadata endpoint exited before the display test because
filename expansion was still disabled after safe query parsing, so its
partition-discovery loop never expanded. The endpoint now restores expansion
for the fixed sysfs search and supplies explicit HTTP errors. No pattern ran
in that attempt. Stock init_boot_b was restored and Android freshly verified.

### Accepted native modeset and vendor module capture

The corrected test on 2026-10-07 UTC selected connector 32, CRTC 60 and primary
plane 35. Atomic `modetest` logged a real 1080×2388 60 Hz modeset and plane
setup, returned 0, and emitted no atomic-commit errors. Visual confirmation
remains pending; accepted KMS commands do not establish a visible desktop.
The earlier plane parser overwrote the plane ID on numeric property headings;
it now recognizes resource rows by their hexadecimal CRTC mask, verified with
the actual ARM BusyBox awk and hardware resource log.

Slot-1 super metadata passed host liblp parsing. The bounded read-only helper
retrieved the single vendor_dlkm_b extent (14,209,024 bytes). Upstream
erofs-utils v1.9.1, commit d3fbccba6e409c100843563b416c47cd82c87ca0, was built
locally without installation and checked/extracted the EROFS image. The
ilitek_v3_mmi.ko module and sensors_class.ko dependency are present. Ilitek
vermagic matches the already working vendor-boot module set exactly, including
gd63125a6a58d; it has not yet been loaded in native Linux. Virtual A/B COW
partitions remain in metadata: no snapshot overlay was applied, so the capture
is recorded as the physical base extent rather than claiming a live merged
Android filesystem.

The exact stock init_boot_b was restored after this test. Fresh ADB verified
original Android build, slot b, unlocked state, completed boot and screen lock
None. The diagnostic image was 4,898,816 bytes, SHA256
50521139da2170a97edb572b4e445972dc4a8ea1d96b6313393941aed5310a69.

The user offered a webcam on their SER8 for visual checks. Existing SSH alias
ser8 works; USB camera nodes appeared then disappeared before capture. No
webcam frame was obtained yet.

EROFS tools source and usage: https://erofs.docs.kernel.org/en/latest/install.html

### SER8 camera connection

SSH through the existing ser8 alias reached the user's Beelink. Initial USB
port repeatedly disconnected the USB 2.0 Camera (0c45:636b) with enumeration
error -71. After the user moved it to another port, FFmpeg successfully
captured a private still showing the phone's Android home screen. No audio
was captured. Diagnostic runner now supports opt-in --webcam-ser8 together
with --usb-network --display-pattern for two private stills during the pattern,
without changing the bounded boot or stock-restoration behavior.

### Native display visually verified

The repeat experiment diagnostic-20261007T015540Z captured two private JPEG
stills through the SER8 webcam during the eight-second pattern. Both clearly
show full-screen vertical color bars and the lower test blocks on the phone.
The matching log selected connector 32, CRTC 60, plane 35; set 1080×2388 at
60 Hz with XR24; returned PATTERN_STATUS:0 and no atomic-commit errors. This
establishes actual native display rendering, not merely DRM mode enumeration.
It does not establish GPU acceleration, touchscreen input, an Arch systemd
boot or a working compositor/Omarchy desktop.

Stock init_boot_b restoration after the visually verified test was successful.
Fresh ADB again confirmed Android boot completed on the original fingerprint,
slot b, unlocked bootloader and Screen lock None. Private webcam frames are
in private/linux-bringup/diagnostic-20261007T015540Z/webcam-{0,1}.jpg.

## Native Omarchy continuation

User explicitly requested continuing until Omarchy Linux runs on the phone.
Stock vendor_dlkm sensors_class and ilitek_v3_mmi modules loaded in native Linux.
The initial probe identified Ilitek 77600 but lacked ILITEK_FW_77600. A checked,
read-only vendor_b capture and EROFS extraction recovered that 519,814-byte
stock firmware file. With it in the ramdisk's /lib/firmware, the next boot
reported firmware version 16.0.1.0, valid coordinates, and FW upgrade pass.
An input device exists; physical touch interaction still needs verification.

The pinned, checksum-verified ARM Omarchy 0.1.1 userspace from the local
fire-hd8-omarchy bundle is repacked into 58 resumable tar stages with 69,374
final regular-file hashes. Signed, hash-verified Arch ARM Weston/Xorg packages
from that project's recorded closure are appended. This is deploying into
/omarchy on the existing SD partition, preserving the verified generic Arch
root outside it. No Pi data or internal phone filesystem is being used.

Desktop code is in port/vegas/root. The stock kernel lacks devtmpfs, VT and
SysV IPC, so the initial native boot path uses a Bash PID 1 supervisor rather
than claiming a standard systemd boot. It starts udev, seatd with VT binding
disabled, DRM/Pixman Weston, software-rendered patched ARM Hyprland, real
Omarchy Quickshell and Foot. Hardware execution is still pending. The bundle's
service-free launcher switch is reused; Android/PRoot are not started.

A phone keyboard was cross-compiled from the locally pinned wvkbd sources and
its ARM help executed under QEMU. User configuration overrides preserve their
bundle originals, use portrait 1080×2388, scale 1.5 and 30 Hz nested rendering,
disable animations/Xwayland and add a keyboard button and initial terminal.
These settings still need live hyprctl reload/configerrors and visual checks.

The diagnostic builder --desktop requires verified desktop progress before
constructing a boot image. Test boots retain automatic return and a 40 C
guard. The runner's --keep-desktop requires standard monitor/bar/terminal
readiness and leaves the native boot running; use only after a visible test.
Sources: https://github.com/BlackFireAlex/omarchy-android ;
https://raw.githubusercontent.com/kennylevinsen/seatd/master/seatd/server.c ;
https://github.com/systemd/systemd/blob/main/README

The first desktop copy run stopped on a per-request timeout after 25/58
checked stages. Stock init_boot_b restoration and fresh Android completed
boot/slot-b verification passed. The checkpoint was retained. Native startup
temperatures across recorded runs remained 29–31 C. The resumed transfer
uses a six-minute maximum native boot with the same 40 C guard, a 130-second
per-stage timeout and reserved time before starting large chunks. Desktop
rendering test boots continue to default to three minutes.

Retrying the partly copied desktop stage exposed a BusyBox tar limitation:
--overwrite does not replace existing symlinks. A real ARM BusyBox fixture
reproduced File exists on a second extraction; the phone log confirmed the
same failure at usr/share/clc/gfx1030-amdgcn--.bc. card-chunk now lists the
checked archive and removes only listed file/symlink destinations inside the
selected chroot before extracting again. It preserves directories and cannot
follow absolute links outside the root being provisioned. This cleanup also
applies to configuration overlays. The phone retry passed the affected stage and subsequent checked stages; the cleanup fix is confirmed on hardware.

The Ilitek input device advertises ABS_MT_SLOT and ABS_MT_TRACKING_ID (type B),
matching the native touch bridge. bringup/touch-pointer.c forwards its first
contact as an absolute Wayland pointer to nested Hyprland, which otherwise
receives no touch from Aquamarine. The ARM binary loader/error path has been
checked under QEMU; protocol binding and actual contact behavior await the
desktop boot. Physical tapping cannot be verified while the user is asleep.
A separate virtual-click helper can exercise that same pointer protocol.

Phone overrides now include service-free reboot/shutdown requests handled by
the native PID 1. The persistent-boot marker is checked again when the test
timer expires, allowing a successful test to stay running. The temperature
guard remains active. No host desktop configuration was changed.

All 58 desktop stages were copied and all 69,374 hashes matched on hardware.
The first full checksum pass exceeded the initial 85-second request timeout;
the longer 200-second retry passed. The first executable checker masked a
Hyprland runtime-directory error because it tested only the final command's
exit status; it now uses set -e and a valid XDG_RUNTIME_DIR. A separate native
UID-1000 check confirmed Hyprland 0.56.1, matching Aquamarine 0.14.0 and the
other listed ABI libraries, Weston 15.0.1, Quickshell 0.3.1 and the keyboard.

The initial configuration packager retained non-executable modes on several
new scripts; its preflight rejected that overlay. The packager now marks
shebang/ELF files executable, and the corrected preflight passed.

The first full native switch_root succeeded. PID 1 is the Arch native-init
Bash supervisor, / is /dev/mmcblk0p1[/omarchy] ext4, and Hyprland reported
1080x2388 at 30 Hz, scale 1.5, clean reload/configerrors, a mapped Foot, an
Omarchy bar and a wvkbd layer. The Ilitek pointer bridge reported ready.
Private webcam stills in diagnostic-20261007T030933Z show the keyboard and
Hyprland window on the real display. The initial bar was still transparent
at readiness; the checker now requires opacity 1 and the keyboard layer.

The PID-1 USR1 transition restarted Linux instead of entering fastboot. Its
exact cause is not established (no pstore record was available). The direct
static helper, invoked by an ordinary root process, correctly entered
fastboot. Exact stock init_boot_b was manually restored and Android reboot
requested. Native transitions have been moved to an ordinary root helper,
with PID 1 left supervising; recovery needs another hardware test. Startup
now sets the hostname through BusyBox, initializes empty machine-id, creates
packaged system accounts with sysusers, starts apps in the home directory,
and uses a larger terminal font. Native startup battery remained 29 C.

The second desktop run still restarted Linux when the shutdown helper first
terminated compositor/device services. The exact failure is not established.
The minimal sync + static RESTART2 path again reached fastboot, and the waiting
runner restored Android. Removing extra teardown made the next complete
bounded desktop run return to fastboot and restore stock successfully.
Continuous frame pacing is enabled for the nested software renderer.

The final native run passed opaque bar/background, keyboard, Foot and clean
config checks, saved a native screenshot and webcam stills, and created the
persistent marker. Injected events into the actual Ilitek evdev path clicked
wvkbd's a key and typed a into Foot; down/up bridge logs and cursor position
matched. Virtual keyboard input ran uname -m and showed aarch64. Two synthetic
contacts on Keys hid/restored the keyboard. Uptime passed 244 seconds with
temperature 29 C, beyond the old bounded deadline. A user-level vegas-power
reboot request changed the private boot ID and returned to the same native
Arch root, bar, terminal, keyboard and bridge. The preserved working image
and manifest are under artifacts/vegas-linux-bringup/native-desktop. Current
use/recovery instructions are in native-omarchy-20261006.md.
