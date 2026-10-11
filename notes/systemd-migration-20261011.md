# systemd as PID 1 on the phone: design and staged files (2026-10-11)

Status: **prepared offline; nothing installed, flashed or booted.** The phone was not touched. Every file below is
inert until four separate steps happen: the files are installed on the root, `vegas-systemd-setup install` enables
them, an init_boot built with the [ramdisk diff](systemd-ramdisk-20261011.diff) is flashed, and `vegas-init
systemd-once` requests a systemd boot. Prerequisite: the tier-1 kernel (`DEVTMPFS`, `DEVTMPFS_MOUNT`, `FHANDLE`,
`AUTOFS_FS`; [kernel-build-20261010.md](kernel-build-20261010.md)) booted and checked with today's native-init first.

## Approach

- **Phase 1 is parity.** Each duty of `native-init` becomes a unit that runs the **same, unchanged script** (wifi-up,
  video-prepare, gpu-prepare, power-transition, vegas-bootctrl, vegas-fuelgauge, vegas-gpu-boost, vegas-desktop). The
  few inline loops in native-init became small helpers in `/usr/local/libexec/vegas/systemd/`. Logs go to the same
  `/var/log/vegas/*.log` files (`StandardOutput=append:`), except the recovery-critical units, which use the journal so
  they never depend on that directory. The desktop is still `vegas-desktop` (dbus-run-session, the bundle's
  service-free launchers with `OMARCHY_PROOT=1`), still without a logind session, still on seatd. Power transitions
  still go through `power-transition` (sync + direct reboot syscall), never systemd's shutdown.
- **native-init stays the fallback** and is not edited. The ramdisk picks it unless systemd was requested, and goes back
  to it after any systemd boot that did not confirm itself.
- **Phase 2** (later, separate tests): an Omarchy-native session (PAM/logind session, uwsm, user units), like the
  Chromebook's `quigon-desktop@`. See the end of this note.

All files live under `/usr/local` (systemd, udev, tmpfiles and sysctl all search `/usr/local/lib/...`), so
`/etc` keeps only enablement state, and the existing installer
(`scripts/install-vegas-phone-update.py`: paths under `usr/local/`, no `@`) can carry them. That is why the ttyGS0 shell
is `vegas-serial-shell.service`, not a `serial-getty@ttyGS0` drop-in.

## Files

| Path (under `port/vegas/root/`) | What |
|---|---|
| `usr/local/lib/systemd/system/vegas-*.{service,timer,slice}` | 25 units (table below) |
| `usr/local/lib/systemd/system/upower.service.d/10-vegas-no-userns.conf` | `PrivateUsers=no` (no `USER_NS` in the kernel) |
| `usr/local/libexec/vegas/systemd/*` | usb-gadget, watchdog-handoff, thermal-guard, power-request-watch, gpu-boot, gpu-boost-reset, sched-tune, desktop-pre, systemd-confirm |
| `usr/local/lib/systemd/system.conf.d/50-vegas.conf` | no hardware watchdog use; `CrashAction=reboot`; 15 s stop timeout |
| `usr/local/lib/systemd/logind.conf.d/50-vegas.conf` | power/suspend/lid keys and idle action **ignored**, no VTs |
| `usr/local/lib/systemd/journald.conf.d/50-vegas.conf` | persistent journal, 64 MB cap, 30 s sync |
| `usr/local/lib/systemd/{coredump,pstore}.conf.d/50-vegas.conf` | coredumps capped at 256 MB; pstore archived but not unlinked |
| `usr/local/lib/systemd/network/10-vegas-{usb0,wlan0}.link` | keep `usb0`/`wlan0` names and MACs |
| `usr/local/lib/sysctl.d/50-vegas.conf` | `kernel.sched_pelt_multiplier = 4` |
| `usr/local/lib/tmpfiles.d/vegas.conf` | `/var/log/vegas`, `/var/log/vegas/power`, `/var/lib/vegas` |
| `usr/local/lib/udev/rules.d/70-vegas-devices.rules` | card0, mali0, dma_heap system/mtk_mm, the vcodec video node: same owner/group/mode as the scripts set |
| `usr/local/share/vegas/systemd/49-vegas-power.rules` | polkit: no logind power-off/reboot/suspend for non-root (installed into `/etc/polkit-1/rules.d`) |
| `usr/local/sbin/vegas-systemd-setup` | `preflight` (read-only report), `install`, `undo` |
| `usr/local/sbin/vegas-init` | `status`, `systemd-once`, `systemd`, `native` |
| `usr/local/bin/vegas-doctor` (edited) | the scheduler check now finds `vegas-top.slice` on systemd boots |
| `notes/systemd-ramdisk-20261011.diff` | proposed `bringup/init` change (not applied) |

Under native-init the new udev rules and `.link` files are also read (udevd runs there too). They set the values the
scripts already set, so nothing changes. Nothing else is read without systemd.

## Duty → unit mapping

Line numbers refer to `native-init` as of today (203 lines).

| native-init (lines) | systemd | Notes |
|---|---|---|
| dirs, `/tmp`, `/dev/shm`, `/run/user/1000` (7–12) | systemd mounts; `tmpfiles.d/vegas.conf`; logind keeps `/run/user/1000` (omarchy **lingers**) | `desktop-pre` keeps the mkdir fallback |
| `exec >>init.log` (13–14) | per-unit `append:` logs + journal | `vegas-power-boot-report` writes `VEGAS_SYSTEMD_PID1` and `uname -a` to init.log |
| loop7 direct I/O (17) | `vegas-loop-dio.service` | only when `/` is `/dev/loop7` |
| `power-boot-report` (19) | `vegas-power-boot-report.service` | early, before sysinit.target |
| hostname (20) | systemd reads `/etc/hostname` (`moto-vegas`) | gpu-prepare checks it |
| chmod null/zero/... (21), mknod loops (33–46) | **devtmpfs + udev** | gone |
| card0 mknod/perms (47–50), dma_heap (51–55) | devtmpfs + `70-vegas-devices.rules` | card0 mknod fallback kept in `desktop-pre` (the ramdisk notes a missing DEVNAME) |
| `lo up` (22) | systemd (PID 1 sets up loopback) | |
| `usb0` address (23) | `vegas-usb-gadget.service` | re-applies 192.168.77.1/30; rebuilds the gadget only if no ramdisk made it |
| `systemd-sysusers` (24), machine-id (25–27) | `systemd-sysusers.service`, PID 1 | `vegas-systemd-setup install` writes a machine-id if empty |
| `dbus-daemon --system` (28) | `dbus.socket` (stock) | |
| `upowerd` (30) | `upower.service` (stock) + drop-in | pulled in by `vegas-desktop` (`Wants=`), D-Bus activation otherwise |
| root httpd (31) | **`vegas-recovery-httpd.service`** | see "Recovery channel" |
| `systemd-udevd --daemon`, trigger, settle (57–59) | `systemd-udevd`, `systemd-udev-trigger` | same coldplug as today |
| input group, usermod (60–63) | sysusers + `99-vegas-input.rules` + `desktop-pre` | |
| seatd (64–68) | **`vegas-seatd.service`** (`seatd -u omarchy -l debug`, `SEATD_VTBOUND=0`) | conflicts with the package's `seatd.service`; `desktop-pre` waits for the socket |
| power request loop (70–87) | **`vegas-power-request.service`** (`power-request-watch`) | polling kept: a path unit could miss the request after logind mounts tmpfs over `/run/user/1000` |
| temperature guard (89–99) | **`vegas-thermal-guard.service`** | `DefaultDependencies=no`, `Restart=always`, OOM-protected, survives isolate and shutdown |
| 180 s persistent-boot guard (100–102) | `vegas-persistent-guard.timer` + `.service` | `OnStartupSec=180`; marker checked at boot and again when it fires |
| kill desktop-watchdog (103–107) | **`vegas-watchdog-handoff.service`** | only after the httpd **listens** (see below) |
| GPU trial/strike/gpu-prepare (109–136) | **`vegas-gpu.service`** (`gpu-boot`) | strike clear after 180 s: `vegas-gpu-confirm.timer` |
| SD notification cleanup (138–143) | `desktop-pre` | |
| GED mode 0 / margin 70 (154–156) | `vegas-gpu.service` | only with `/run/vegas-gpu-ready` |
| `vegas-gpu-boost` (157) | **`vegas-gpu-boost.service`** | new: `ExecStopPost` drops the OPP pin and CPU floors if it dies mid-boost |
| `video-prepare &` (162) | `vegas-video.service` (`Type=exec`) | `RefuseManualStop` (unloading the decoder reboots the phone) |
| `vegas-fuelgauge &` (166) | `vegas-fuelgauge.service` | |
| `wifi-up &` (169) | `vegas-wifi.service` (`Type=exec`, `RemainAfterExit`) | its daemons stay in the unit's cgroup; `RefuseManualStop` |
| — | video, fuelgauge, wifi `After=vegas-gpu.service` | keeps native-init's order of vendor module loads |
| `sched_pelt_multiplier` 4 (178) | `sysctl.d/50-vegas.conf` | |
| cgroup2 mount, `top-app`, `latency_sensitive` (179–185) | systemd mounts cgroup2; **`vegas-top.slice`** + `vegas-sched.service` | below |
| `runuser ... vegas-desktop` in top-app (186–190) | **`vegas-desktop.service`** (`User=omarchy`, `Slice=vegas-top.slice`) | `Restart=always` (new), max 3 starts / 5 min |
| bootctrl after 60 s (192–202) | `vegas-bootctrl-mark.timer` (`OnActiveSec=60`, started by the desktop) + `.service` | `ExecCondition=pgrep -x Hyprland` |
| `wait -n` reaper (203) | PID 1 | |
| — | `vegas-systemd-confirm.timer` + `.service` | clears the ramdisk's systemd trial strike |
| — | `vegas-serial-shell.service` | root shell on `/dev/ttyGS0` (laptop: `/dev/ttyACM0`), second recovery channel |

### top-app as a slice

systemd owns `/sys/fs/cgroup` (it mounts cgroup2 itself; native-init's mount and `cgroup.subtree_control` writes are
not needed). `vegas-top.slice` sets `CPUWeight=100` (the default weight; setting it makes systemd enable the cpu
controller there, the same trick as the Chromebook's `app-.scope.d/50-quigon-cpu.conf`) and `AllowedCPUs=0-7`
(cpuset enabled, no restriction, as native-init's `+cpu +cpuset`). systemd has no property for the Android vendor file
`cpu.uclamp.latency_sensitive`, and a slice can't run commands, so `sched-tune` writes it into
`/sys/fs/cgroup/vegas.slice/vegas-top.slice/` (path from `systemctl show -P ControlGroup`), from
`vegas-sched.service` at boot and again from the desktop's `ExecStartPre=+`. Hyprland and everything it starts inherit
the slice, as they inherited `top-app`.

Difference: with the cpu controller on, `system.slice`, `user.slice` and `vegas.slice` are weighted siblings (100
each) instead of top-app vs. individual root-cgroup processes. That only matters under contention. Re-run the
scrolling bench (notes/chrome-scrolling-20261010.md) on the first persistent systemd boot.

### Boot order on systemd

```
switch_root → systemd
  early (DefaultDependencies=no): vegas-usb-gadget → vegas-recovery-httpd → vegas-watchdog-handoff
                                  vegas-thermal-guard, vegas-loop-dio, vegas-power-boot-report
  sysinit: udevd + coldplug, tmpfiles, sysusers, sysctl (pelt 4), journald
  basic: dbus.socket, timers (persistent guard, GPU strike clear)
  multi-user (default.target):
    logind (+ user-runtime-dir@1000, user@1000 for the lingering user), vegas-seatd, vegas-power-request,
    vegas-gpu → {vegas-gpu-boost, vegas-video, vegas-fuelgauge, vegas-wifi}
    vegas-top.slice → vegas-sched
    vegas-desktop (after seatd, gpu, sched, user-runtime-dir@1000) → timers: bootctrl mark, systemd confirm (+60 s)
    vegas-serial-shell
```

`/run/user/1000` must be logind's tmpfs before Weston/Hyprland create sockets in it (mounted later, it would hide
them). Hence `Wants=/After=user-runtime-dir@1000.service` on the desktop, and **linger**: any `runuser -u omarchy` (gpu-prepare's EGL
smoke test, CGI commands, vegas-doctor's hyprctl) opens a PAM session through `pam_systemd`; without linger, logind
would unmount `/run/user/1000` when that session ends, under the running desktop.

## Recovery channel

**Keeping the ramdisk's httpd alive does not work.** Its document root, CGI scripts and shell live in the initramfs,
which `switch_root` deletes; the process would stay chrooted in that emptied root and could not see the new one. So the
root's own copy (the ramdisk copies `busybox`, `to-bootloader` and the report CGIs into the root on every boot) runs as
`vegas-recovery-httpd.service`, the first thing systemd starts:

- `DefaultDependencies=no`, ordered only after `vegas-usb-gadget` (the address already exists, set by the ramdisk), not
  after local-fs, sysinit, dbus or any network service. A failed emergency/rescue boot still has it
  (`IgnoreOnIsolate=yes`, also `WantedBy=emergency.target rescue.target`). Not stopped by shutdown.target.
- `Restart=always` with no start limit; `KillMode=process`, so restarting the server doesn't kill a command's
  background jobs (return-fastboot). `OOMScoreAdjust=-1000`.
- `vegas-watchdog-handoff` kills the ramdisk's static `desktop-watchdog` only once `192.168.77.1:8080` is in LISTEN
  state (`/proc/net/tcp`). If systemd never gets the recovery channel up, the watchdog still sends the phone to
  fastboot at 180 s, and the ramdisk's strike (below) starts native-init on the next boot. native-init kills the
  watchdog unconditionally; this is stricter on purpose.
- Second channel: `vegas-serial-shell` (root autologin on the gadget's ACM function). Same trust level as the CGI on the
  point-to-point USB link.
- `ufw` is disabled by `install` (Omarchy enables it; its DROP policy would cut the laptop off). The kernel has no
  `NF_TABLES`, so Arch's iptables-nft would fail anyway, but that is no reason to leave it enabled.

## Ramdisk change (proposed, not applied)

Full diff: [systemd-ramdisk-20261011.diff](systemd-ramdisk-20261011.diff) (`patch -p1` from the repo root; checked
with `patch --dry-run` and `sh -n`). Build as today with `scripts/build-vegas-diagnostic.py --desktop`.

1. **devtmpfs on `/dev`** when `/proc/filesystems` lists it (tier-1 kernel), else tmpfs + mknod as today.
   `DEVTMPFS_MOUNT` doesn't apply to initramfs boots, so the ramdisk has to mount it (`-o mode=0755,nosuid`, as
   mkinitcpio does). This also changes native-init boots on the tier-1 kernel: its mknod loops then find existing
   nodes and skip them.
2. **Init choice**, after the root is mounted at `/newroot`:
   ```sh
   init=/usr/local/libexec/vegas/native-init
   if devtmpfs && [ -x /newroot/usr/lib/systemd/systemd ] && [ -f /newroot/var/lib/vegas/systemd-setup/installed ]; then
       if card_ok && [ -e /mnt/arch/force-native-init ]; then          # physical override on the microSD
           :
       elif [ -e /newroot/var/lib/vegas/systemd-pending ]; then        # last systemd boot never confirmed
           mv /etc/vegas/systemd → systemd.disabled-unstable; rm systemd-once, pending
       elif [ -f /newroot/etc/vegas/systemd-once ]; then rm it; init=/usr/lib/systemd/systemd
       elif [ -f /newroot/etc/vegas/systemd ]; then init=/usr/lib/systemd/systemd
       fi
   fi
   if systemd: write /newroot/var/lib/vegas/systemd-pending; chmod 0755 /run
   echo "$init" >/run/vegas-init; ... exec switch_root /newroot "$init"
   ```
   The task suggested "`/etc/vegas/systemd` present"; this adds the one-boot flag, the strike, the `installed` gate
   (so systemd never boots with the stock unit set, e.g. a display manager or ufw enabled) and the card override.
   The strike has to be written by the ramdisk: if systemd dies before running any unit, nothing on the root could
   record it.
3. Unchanged: module loading, gadget, the ramdisk httpd until switch_root, `desktop-watchdog`, boot-source choice.

## Getting back to native-init

From most to least convenient:

1. **Automatic.** `systemd-once` boots systemd once; any next boot is native-init. With persistent `systemd`, any boot
   that did not confirm within the session (Hyprland running and the httpd listening, 60 s after the desktop starts)
   leaves `systemd-pending`, so the next boot is native-init and persistent systemd is renamed to
   `systemd.disabled-unstable`. A PID 1 crash reboots (`CrashAction=reboot`, kernel `PANIC_TIMEOUT=-1`) into that path.
2. **httpd up** (desktop or anything else broken): POST
   `vegas-init native; /usr/local/libexec/vegas/power-transition reboot` to `/cgi-bin/command`, or GET
   `/cgi-bin/return-fastboot` then `fastboot reboot`. The ttyGS0 shell works too.
3. **httpd never came up:** within 180 s the ramdisk's desktop-watchdog enters fastboot (`fastboot devices` shows the
   phone); `fastboot reboot` → native-init (strike).
4. **Hung after the handoff, no network:** hold Power + Volume down for the bootloader, `fastboot reboot` → native-init,
   unless that boot had already confirmed. Then use the card override: create `force-native-init` at the
   microSD's root from the laptop.
5. Last resort, as today: flash the previous init_boot (`artifacts/vegas-linux-bringup/native-desktop/`) or the
   stock one.

## Install and activation (when there is a hardware session)

1. Ship the new files with the existing installer flow (`scripts/install-vegas-phone-update.py` accepts `usr/local/`
   paths; give `prepare-vegas-phone-update.py` the file list above, mode 0755 for scripts, 0644 for units/configs),
   including the edited `vegas-doctor`.
2. `vegas-systemd-setup preflight` (read-only) → save the output under `private/`. Review: systemd version (kernel 5.15
   is above systemd 261's 5.14 "recommended baseline"; minimum 5.10), conflicting enabled units, `/etc/fstab` lines, `/etc/resolv.conf`,
   enabled user units (global and omarchy), `/etc/pam.d/runuser` → `system-login` → `pam_systemd`, `/usr/lib/modules`.
3. `vegas-systemd-setup install`: disables only the conflicting units it finds enabled (recorded in
   `/var/lib/vegas/systemd-setup/disabled-units`): display managers, `omarchy-seamless-login`, the package's `seatd`,
   iwd, NetworkManager, systemd-networkd/resolved/timesyncd, wpa_supplicant/dhcpcd units, ufw, sshd, avahi, docker. Then it
   enables the vegas units, sets `multi-user.target` as default (no display manager), enables linger for omarchy,
   masks `systemd-gpt-auto-generator` (the root is a loop device or a card subdirectory) and installs the polkit
   rule. `undo` reverts all of it.

## Test plan for the first systemd boot

Attended, laptop on USB, battery below 35 °C and charged, `fastboot` at hand. Ask before every flash.

**Step 0, kernel only** (planned already): tier-1 `boot_b`, today's init_boot. `uname -v` `#2`; no CRC/"disagrees
about version" lines in `dmesg`; `devtmpfs` in `/proc/filesystems`; vegas-doctor as good as on stock; Wi-Fi, GPU desktop,
video decode, touch, keyboard.

**Step 1, files inert:** install the files plus `vegas-systemd-setup install`, reboot (native-init). vegas-doctor and
the desktop are unchanged (proves the files are inert; the udev rules/.link files are the only ones read).

**Step 2, new ramdisk on native-init:** flash the init_boot with the diff, no flag set. Expect `findmnt /dev` =
devtmpfs, `/run/vegas-init` = native-init, `VEGAS_INIT:` in `dmesg`, and the same vegas-doctor result. This
tests native-init on devtmpfs before systemd is involved.

**Step 3, first systemd boot:**

- Optional but recommended: `rm /etc/vegas-persistent-boot` so the 180 s fastboot guard is armed (it needs systemd
  working; the desktop-watchdog covers the time before that). Re-create the marker from the laptop once the checks pass,
  as `try-vegas-diagnostic.py` does with `KEEP_DESKTOP`. Note: if the boot fails, the next native-init boot also arms its 180 s guard
  until the marker is back.
- `vegas-init systemd-once`, then `/usr/local/libexec/vegas/power-transition reboot`.
- Laptop: poll `curl --noproxy '*' -m 3 http://192.168.77.1:8080/cgi-bin/status` every 2 s → expect
  `NATIVE_PID1:systemd`. Optionally `picocom /dev/ttyACM0`.

Checks (through `/cgi-bin/command`; keep the outputs in `private/`):

| Area | Command | Expect |
|---|---|---|
| PID 1 | `cat /run/vegas-init; systemctl is-system-running; systemctl --failed` | systemd; `running` or `degraded` with only understood failures |
| Boot time | `systemd-analyze; systemd-analyze blame \| head -20; systemd-analyze critical-chain vegas-desktop.service` | first boot is slower (ldconfig, hwdb, catalog) |
| Warnings | `journalctl -b -p warning --no-pager \| head -150` | look for namespace (226/NAMESPACE), ACL, watchdog, firmware, cgroup messages |
| Mounts | `findmnt /dev /run /sys/fs/cgroup /run/user/1000 /` | devtmpfs, tmpfs (0755), cgroup2, tmpfs, loop7 |
| Nodes | `ls -l /dev/dri /dev/mali0 /dev/dma_heap /dev/video* /dev/input/event* /dev/ttyGS0` | group video/input as before |
| Recovery | `systemctl status vegas-recovery-httpd vegas-watchdog-handoff vegas-serial-shell; pgrep -a desktop-watch` | httpd active; watchdog gone; handoff log line |
| Guards | `systemctl status vegas-thermal-guard vegas-power-request; systemctl list-timers` | active; persistent-guard timer only without the marker |
| GPU | `ls /run/vegas-gpu-ready; cat /sys/kernel/ged/hal/{dvfs_loading_mode,timer_base_dvfs_margin}; systemctl status vegas-gpu-boost` | ready, 0, 70, active |
| top-app | `cat /sys/fs/cgroup/vegas.slice/vegas-top.slice/cpu.uclamp.latency_sensitive /proc/sys/kernel/sched_pelt_multiplier; grep . /proc/$(pgrep -x Hyprland)/cgroup` | 1, 4, `vegas-top.slice` |
| Desktop | `vegas-doctor`; screenshot (grim, as `try-vegas-diagnostic.py`); touch, keyboard, Chrome web app, a video by hand | as on native-init |
| Wi-Fi | `ip -br addr; wpa_cli -i wlan0 status; date` | wlan0 address, time synced |
| Battery | `cat /sys/class/power_supply/battery/{capacity,temp}; upower -e` | capacity ≥ 0; bar shows it |
| logind | `loginctl list-sessions; loginctl show-user omarchy -p Linger`; press the power key | Linger=yes; key does nothing |
| polkit | `pkcheck -a org.freedesktop.login1.reboot -p $(pgrep -u omarchy -x Hyprland)` | not authorized |
| Confirm | after 60 s: `ls /var/lib/vegas/systemd-pending; tail /var/log/vegas/bootctrl.log` | pending gone; slot marked |
| Strikes | after 180 s: `ls /var/lib/vegas/gpu-boot-pending` | gone |
| Power | `vegas-power reboot` from the desktop's terminal | reboot through the request file; next boot native-init (once used up) |

Then save `journalctl -b -o short-monotonic` and `systemd-analyze plot` to `private/`. After a failed boot, native-init
can read the persistent journal: `journalctl -D /var/log/journal --list-boots`, then `-b -1`.

**Step 4:** `vegas-init systemd` (every boot); repeat the checks, then `vegas-power fastboot`, the 40 °C guard path
(`power-transition fastboot` via the guard is the same helper), a crash restart (`kill -SEGV` Hyprland → back within
seconds), and the cable-disconnected power-off. Re-measure scrolling (top-app as a slice).

Rollback at any point: the ladder in "Getting back to native-init". `vegas-systemd-setup undo` removes the
enablement state; the ramdisk then ignores systemd (no `installed` marker).

## Open risks

1. **Kernel features the tier-1 kernel still lacks.** `USER_NS` (PrivateUsers=: upower, also pcscd,
   power-profiles-daemon, uuidd. upower gets a drop-in; check the others in `systemctl --failed`), `PID_NS` (only
   paccache uses PrivatePIDs), `SYSVIPC`/IPC namespaces, `CGROUP_PIDS` (TasksMax= ignored with warnings),
   `TMPFS_POSIX_ACL`/`TMPFS_XATTR` (logind uaccess ACLs on devtmpfs nodes and tmpfiles ACL lines for `/run/log/journal`
   fail: expect a degraded `systemd-tmpfiles-setup`; device access works through the video/input groups and seatd,
   as today), `POSIX_MQUEUE` (dev-mqueue.mount skipped), PSI (`cgroup_disable=pressure` in the built-in cmdline: no
   systemd-oomd). `CONFIG_DMIID` absent (ARM). If any of these cost more than a warning, the next kernel tier should
   add `USER_NS`, `TMPFS_POSIX_ACL`, `TMPFS_XATTR`, `CGROUP_PIDS`, which may not be ABI-neutral; check CRCs first.
2. **Firmware.** systemd-udevd stopped loading firmware in v217; the kernel loads it directly from
   `firmware_class.path=/vendor/firmware` (absent on the root) and then `/lib/firmware/...`. That is the same as today
   (udevd already runs under native-init). The ramdisk's modules use the ramdisk's `/lib/firmware` (ILITEK). WMT, GPU,
   video and audio load from the root's `/usr/lib/firmware` (notes/audio-20261011.md puts `aw883xx_acf.bin` there).
   `FW_LOADER_USER_HELPER=y` (systemd recommends `n`) means a driver that explicitly asks for the sysfs fallback
   waits 60 s, as it does today. udev's coldplug also autoloads modules by modalias if `/usr/lib/modules/$(uname -r)`
   exists on the root, again as today (preflight shows it).
3. **Battery temperature guard** is now a unit: `DefaultDependencies=no`, `Restart=always`, OOM-protected, kept through
   isolate and shutdown. It starts a little later than in native-init (after systemd's own setup, a few seconds). If
   systemd hangs before any unit runs, the desktop-watchdog's 180 s fastboot is the only guard, as for native-init
   before line 89. vegas-gpu-boost still stops boosting at 37 °C on its own.
4. **logind vs seatd.** logind always runs on systemd (multi-user.target, D-Bus). It no longer matters for devices:
   phase 1 has no session for the desktop, and libseat is pinned to seatd (`LIBSEAT_BACKEND=seatd`). The real risk was
   **power-key handling** (default `HandlePowerKey=poweroff` would shut down through systemd, with the cable check
   bypassed): set to ignore, along with suspend/lid/idle. Sessions appear for every `runuser` (pam_systemd), so linger
   is required. The lingering user manager starts the user units enabled for omarchy (pipewire/wireplumber sockets,
   Omarchy units under default.target). They live on the user bus, not on the desktop's private dbus-run-session bus;
   review the preflight list.
5. **journald on the loop root.** Persistent journal, 64 MB cap, 30 s sync. Power transitions skip systemd's shutdown,
   so the journal is never closed cleanly: on the next boot journald renames the "uncleanly shut down" file and starts
   a new one (expected, harmless). On the card rescue root it writes to the SD card (slow, small). ACLs on
   `/var/log/journal` work (ext4 has POSIX ACLs).
6. **systemd's own shutdown path is not used.** On this phone, tearing down the compositor/devices before the
   reboot syscall once restarted Linux instead of entering fastboot or powering off (notes/linux-bringup-20261006.md).
   Stock `omarchy-system-reboot/-shutdown` call `systemctl reboot/poweroff` via `systemd-run --user`, which becomes
   possible with a user manager. The polkit rule denies that for non-root users, so vegas-power stays the only path.
   Root `systemctl reboot` is still possible: don't use it until it is tested attended. The hardware watchdog stays
   untouched (`RuntimeWatchdogSec`/`RebootWatchdogSec` off; MediaTek's kernel kicker feeds it).
7. **Units that would fight the port** if enabled in the Omarchy root: a display manager or `omarchy-seamless-login`
   (DRM master), iwd/NetworkManager/networkd (wlan0, usb0), resolved (resolv.conf), timesyncd (vs ntpd), ufw (cuts off
   the recovery httpd). `install` disables what it finds; preflight shows the rest. An `/etc/fstab` line for a missing
   device would drop into emergency mode (httpd survives).
8. **Interface renaming:** `.link` files pin `usb0` and `wlan0`. udev already runs under native-init with the same
   default policy and they kept their names; the files are a guarantee.
9. **First systemd boot is slower:** `ConditionNeedsUpdate=/etc` jobs (ldconfig, hwdb update, journal catalog) run
   once. The watchdog handoff does not wait for them.
10. **Concurrency:** units start in parallel. Vendor module loads keep native-init's order (GPU first; video,
    fuel gauge, Wi-Fi after it); those three load in parallel with each other, as they did in native-init's
    background jobs.
11. **The strike is strict by design:** a deliberate reboot within the first 60 s of a systemd boot counts as
    unconfirmed. A pending marker left from a tier-1 boot also counts if the kernel is swapped back and forth.
12. **Resource defaults:** systemd turns on memory accounting for all units (some overhead); systemd-coredump now keeps
    crash dumps (capped at 256 MB; useful for the diagnose-crash workflow); systemd-pstore archives pstore records
    (`Unlink=no` keeps the originals).
13. `RefuseManualStop` on Wi-Fi and video: to retry Wi-Fi, reboot (as today). Making WMT restartable is separate work.

## What Omarchy gets from systemd

Phase 1 already gives a working `systemctl`/`journalctl`/`loginctl`/`timedatectl`/`hostnamectl`, D-Bus activation of
system services, coredumps, timers, and a lingering **user manager** (`systemctl --user`). On the laptop's current
Omarchy, 65 of 441 `omarchy-*` scripts call systemctl/uwsm/loginctl/timedatectl/journalctl and 34 use uwsm. Omarchy
ships user units such as `omarchy-crash-watch`, `omarchy-fcitx5`, `omarchy-sleep-lock`, `bt-agent`, plus
`user@.service.d/faster-shutdown.conf`.
The phone's pinned omarchy-android 0.1.1 bundle avoids all of that through `OMARCHY_PROOT=1`.

**Phase 2 (separate test, after phase 1 is persistent):** a `vegas-omarchy@.service` like `quigon-desktop@`
(`PAMName=login`, `Environment=XDG_SEAT=seat0 XDG_SESSION_TYPE=wayland XDG_SESSION_CLASS=user`, `Restart=always`)
running `uwsm start ... hyprland.desktop` instead of dbus-run-session. That brings:

- PipeWire/WirePlumber as user units, which the audio bring-up needs (notes/audio-20261011.md "Session work still
  needed")
- xdg-desktop-portal(-hyprland) for Chrome file pickers and screen sharing
- polkit agent, gnome-keyring, Omarchy's own user units, and the stock reboot/shutdown/logout scripts (with the
  polkit rule adjusted once systemd's shutdown is proven)
- later, logind-driven suspend

Work it needs:

- move vegas-desktop-drm's environment (AQ_*, HYPRLAND_*, OMARCHY_*, the Mali LD_LIBRARY_PATH/LIBGL_DRIVERS_PATH,
  `AQ_KMS_CPU_FENCE_WAIT`) into a `wayland-wm@hyprland.desktop.service.d` drop-in, as the Chromebook's
  `10-quigon-gpu.conf` does
- a hyprland.desktop pointing at the bundle's `/opt/omarchy-android/hyprland/bin/Hyprland`
- drop `OMARCHY_PROOT=1`
- move top-app to the user manager: uwsm puts Hyprland and apps under `user@1000.service/{session,app}.slice`, so
  write `latency_sensitive` there (CPUWeight drop-ins on `app-.scope`/`app-.service`/`wayland-wm@.service`, as quigon
  does) or on `user-1000.slice`
- the uwsm VT lookup is avoided by the seat0 PAM session, as on the Chromebook (notes in the Chromebook repo,
  desktop-20261008.md)

## Result on the phone (2026-10-11, night): systemd is the default PID 1

Tier-1 kernel (#2) plus the systemd-aware ramdisk (this diff applied to bringup/init, init_boot sha256 7c185e98…):
- Step 1: files installed with `vegas-systemd-setup install`; native-init boot unchanged (vegas-doctor 0/0).
- Step 2: new ramdisk on native-init: `/dev` is devtmpfs, native-init chosen, vegas-doctor 0/0.
- Step 3: `vegas-init systemd-once`. `NATIVE_PID1:systemd`, recovery httpd answering 9 s after boot,
  `systemctl is-system-running` = **running** (0 failed units), **startup 8.2 s (kernel 3.2 s +
  userspace 4.9 s)**, Hyprland in `vegas.slice/vegas-top.slice/vegas-desktop.service`, systemd-pending
  cleared by the confirm timer, vegas-doctor 0/0.
- Step 4: `vegas-init systemd` (default). A reboot requested from the desktop session (`vegas-power reboot`)
  goes through vegas-power-request; the next boot is systemd again, 7.7 s, running, vegas-doctor 0/0.
- The fastboot `fb_mode` safety net used during testing is cleared, and a normal unattended boot reaches
  systemd + the desktop.

Notes: the top-app slice is an inner cgroup node, so a process started outside the desktop service (e.g. a
test browser launched from the root CGI) can't be moved into it. Apps launched from Hyprland inherit
vegas-desktop.service's cgroup inside the slice.
