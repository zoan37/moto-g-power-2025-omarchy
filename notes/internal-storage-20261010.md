# Omarchy on internal UFS storage (2026-10-10)

## Why

Opening a Chrome web app froze the desktop for tens of seconds. The CPU was mostly idle; the microSD
card (32 GB Samsung, already at UHS SDR104) was the bottleneck: `/proc/pressure/io` showed every task
stalled ~77% of the time, and the block stats averaged ~2.3 s per write request. Chrome's profile
(synced extensions, X's service-worker cache) does many small synced writes, which SD cards handle
badly. The Chromebook (same CPU class, Mali-G57) runs from internal UFS and does not have this problem.

## Layout

The internal storage is one 122 GB UFS LUN (`sdc`, 4 KiB sectors, 85 GPT partitions); the only large
one is Android's `userdata` (`sdc84`, 107.9 GiB). Omarchy now lives **inside** `userdata`:

| Range in userdata | Use |
|---|---|
| 0 – 24 GiB | free (reserved for a possible future Android userdata) |
| 24 GiB – end (83.9 GiB) | ext4, label `vegas-omarchy`, mounted through `/dev/loop7` |

The ramdisk (`bringup/init` + `bringup/ufs-guard`) attaches `loop7` at offset 25769803776, checks the
label and filesystem type, runs `e2fsck -p`, mounts it and switch_roots into it. `native-init` turns
on direct I/O for `loop7`. The microSD stays as the rescue copy.

### Boot order and the SD warning

1. microSD with a `prefer-card` file at its root → card (on purpose).
2. internal root present and passes its checks → internal.
3. otherwise → card.

The ramdisk writes `/run/vegas-boot-source` (`ufs` or `card <reason>`) and logs `VEGAS_BOOT_SOURCE:`
to the kernel log. On a card boot, `vegas-boot-source-notify` posts a critical notification and the
`vegas.bootsource` bar widget shows a red **SD** badge for the whole session. Verified on the phone:
with the internal root missing, the ramdisk logged `UFS_ERROR:… missing`, booted the card, and both
warnings appeared.

## Lesson: Motorola's bootloader only boots its own GPT

The first attempt split `userdata` with a new GPT partition (`omarchy`, entry 86) instead of a loop
device. The new table was spec-valid (checked with `sfdisk -V` on a 4 KiB-sector loop image; only
entry 84's end, entry 86 and the CRCs changed). Motorola's LK refused to boot with it:
`fastboot getvar reason` → **`Failed to initialize partition table`**.

Motorola's `PGPT` (flashed to the `gpt` pseudo-partition by every rescue flash) is a template: header
CRCs 0, `userdata` = 0..0 ("rest of the disk"), `flashinfo` = size 4096 at the end. LK expects exactly
that layout. **Do not add or resize GPT partitions on this phone.**

Recovery that worked:

1. Extract `PGPT` from the matching firmware zip with HTTP range reads (32768 bytes; MD5 must match
   `flashfile.xml`), then `fastboot flash gpt PGPT`. All 83 other entries are identical in name, start,
   end and attributes; only per-partition GUIDs are regenerated.
2. Side effect: the flash reset the A/B metadata to **slot a** (old W1VE36H.10-12 Android with a stock,
   locked LK). Slot a showed "No valid operating system could be found" and its LK refused
   `set_active` and unsigned flashes ("Preflash validation failed").
3. `fastboot flash lk_a` with the same Valhalla-patched LK used on `lk_b` (hash `9b189a1d…`), then
   `fastboot oem unlock <Valhalla key>` (confirm on the phone; it wipes `userdata`), then
   `fastboot set_active b` and reboot. Both slots now carry the patched LK.

## A/B boot control

Android's boot_control HAL marks each good boot successful. Native Linux never did, so every boot
spent one of slot b's tries; at zero, the bootloader falls back to slot a. Slot a holds old stock
Android, which would format `userdata`, and with it the Omarchy root. `vegas-bootctrl` (in
`/usr/local/sbin`) reads and writes the AOSP `bootloader_control` block at offset 2048 of `misc`
(magic `BCAB`, CRC32 over the first 28 bytes, verified before any write). `native-init` runs it once
Hyprland has been up for 60 s:

- `mark-successful`: current slot keeps its priority and tries, successful = 1
- `disable-other`: the other slot gets priority 0 and tries 0, so the phone never falls back to slot a

`vegas-bootctrl status` prints both slots.

## Other changes

- `gpu-prepare` and `scripts/install-vegas-phone-update.py` accept `/dev/loop7` (backed by `userdata`,
  label `vegas-omarchy`) as well as the CID-checked SD root.
- `scripts/split-vegas-userdata.py` (GPT split) is kept only as a record of the failed approach. Do not
  run it on the phone.
- GPT backups from before the change: `backups/vegas-gpt-20261010/` (ignored).
