# Bluetooth for BlueZ (2026-10-11)

**Status:** BlueZ sees and powers the controller (`hci0`, "moto-vegas"). A 12 s scan found 9 devices. The shade has
Bluetooth and Airplane-mode tiles, and the status bar shows Omarchy's Bluetooth widget (tap: device list).
Not tested yet: pairing and connecting, BT audio through PipeWire (bluez5 SPA plugin, part of pipewire-audio), and
behaviour across suspend.

## How the chip is reached

- The MT6835's combo chip (connac1x, same WMT/BTIF stack as Wi-Fi) has no kernel HCI driver. The vendor
  `bt_drv_connac1x.ko` (vendor_dlkm, deps already loaded by Wi-Fi) only exposes raw **H4 HCI on `/dev/stpbt`** for
  Android's vendor HAL. Opening it makes WMT power Bluetooth on ("WMT turn on BT OK").
- Raw test: HCI Reset OK, Read Local Version = HCI 5.2 (0x0d), manufacturer 0x0046 (MediaTek).
- **hci_vhci** (virtual HCI) isn't in the GKI kernel. It is built as a module for the tier-1 kernel:
  `EXTRA_MODULES=BT_HCIVHCI scripts/build-vegas-gki-kernel.sh tier1 modules_prepare`, then
  `... tier1 M=drivers/bluetooth modules` with `third_party/kernel/results/tier1/Module.symvers` copied into
  the out dir first. vermagic matches. Everything it needs is in the KMI export list (`hci_alloc_dev_priv`,
  `hci_register_dev`, `hci_recv_frame`, …; the rest are inlines). sha256 `87766999…313e4`; it is kept in
  `artifacts/bluetooth/modules/` and installed to `/opt/vegas-bluetooth/modules/`.
- `vegas-bt-bridge` (`/usr/local/sbin`) opens /dev/stpbt, creates a primary controller on /dev/vhci, and copies
  packets both ways. /dev/stpbt reads can hold partial or several packets, so they are re-framed by H4 type
  (event/ACL/SCO/ISO); vhci takes one packet per write.

## Fixes the bridge applies

- **Factory address:** the chip starts with MediaTek's placeholder `00:00:46:67:61:01` (the same on every phone).
  Android's HAL programs the real one from NVRAM. The `nvdata` partition is all zeros on this phone (wiped), but
  the raw `nvram` partition holds MediaTek's NVRAM backup:
  - header: table size, data size, then 140-byte entries `[u32 7][u32 offset][u32 size][char path[128]]`
  - data region at 0x20000; `APCFG/APRDEB/BT_Addr` at data offset 0x36, its first 6 bytes are the address
    (Motorola OUI)

  The bridge sets it with MediaTek's vendor command **0xFC1A** (address little-endian) before creating hci0; it
  survives HCI Reset. The address is not written to this repo.
- **Sync train:** the firmware advertises `LMP_SYNC_TRAIN` (extended features page 2, byte 0, bit 0x04) but answers
  Read Synchronization Train Parameters (0x0c77) with "Unknown HCI Command", and the kernel aborts controller
  setup ("Close Index" right after it, seen with `btmon -w`). The bridge clears that bit in the Read Local Extended
  Features (page 2) reply.
- `bnep` (BT networking) isn't in the kernel: bluetoothd logs it, and PAN tethering won't work.

## Audio (PipeWire)

PipeWire's BlueZ plugin ships in `pipewire-audio`. WirePlumber's BlueZ monitor normally runs only while the user's
logind session is active on a seat; the phone's desktop is seatless (lingering user manager), so
`~/.config/wireplumber/wireplumber.conf.d/50-vegas-bluez.conf` disables `monitor.bluez.seat-monitoring`. After that
the adapter advertises Audio Source/Sink (A2DP), A/V Remote Control and Handsfree/AG. New outputs start at 25%
(`50-vegas-volume.conf`).

## Boot

`vegas-bluetooth.service` → `/usr/local/libexec/vegas/bluetooth-up`: waits for Wi-Fi's WMT stack (wmt_drv +
wlan0), loads hci_vhci and bt_drv_connac1x, then execs the bridge (Restart=on-failure). `bluetooth.service`
(BlueZ 5.87, `pacman -S bluez bluez-utils`) is enabled. Opt out with `/var/lib/vegas/no-bluetooth`.
