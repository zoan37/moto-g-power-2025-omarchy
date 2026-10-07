# Arch Linux ARM VM

The host has `qemu-system-aarch64` for full-system emulation and an existing `qemu-aarch64-static` binfmt registration for ARM userspace work. This laptop is x86_64, so aarch64 full-system guests use TCG emulation, not KVM hardware acceleration.

The generic Arch Linux ARM rootfs was downloaded from `https://ca.us.mirror.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz`. The original `os.archlinuxarm.org` HTTPS endpoint failed hostname verification here; that was not bypassed.

The detached signature was checked in a project-local GPG keyring. The signing fingerprint matches the official [Arch Linux ARM signing-key page](https://archlinuxarm.org/about/package-signing): `68B3537F39A313B3E574D06777193F152BDBE6A6`. GPG's unknown-trust warning refers to this fresh local keyring; the fingerprint was compared separately with the published key. The downloaded archive was signed on 2026-08-05.

The archive is extracted into ignored `vm/rootfs/`. `fakeroot` preserves archive ownership metadata during extraction and writes `vm/fakeroot.state`; `mkfs.ext4 -d` uses that metadata to construct the regular-file disk `vm/archarm.raw`. Host partitions and removable media are not involved.

The generic kernel includes ext4, the virtio PCI transport and virtio block support. `scripts/run-arm-vm` extracts the disk's current `/boot/Image` using read-only `debugfs` and boots it directly with this disk. This keeps the kernel aligned with guest kernel updates. It does not use the phone's stock kernel and cannot validate Motorola drivers. Run one VM instance at a time.

```bash
./scripts/run-arm-vm
```

The guest serial console uses QEMU's normal terminal controls: Ctrl+A then X exits the emulator. The rootfs's initial accounts are `root` / `root` and `alarm` / `alarm`, as documented by [Arch Linux ARM](https://archlinuxarm.org/platforms/armv8/generic). No host port forwarding is configured. Use `poweroff` in the guest to shut down cleanly.

## Rebuilding the base disk

After verifying the downloaded signature, use a fresh rootfs directory and a new regular-file disk name. Keep extraction and disk creation in the same fakeroot context, or reuse the saved state:

```bash
fakeroot -s vm/fakeroot.state bsdtar -xpf vm/ArchLinuxARM-aarch64-latest.tar.gz -C vm/rootfs
fakeroot -i vm/fakeroot.state mkfs.ext4 -q -F -b 4096 -L archarm-vm -d vm/rootfs vm/new-archarm.raw 2097152
```

The second command creates an 8 GiB sparse filesystem image. It rebuilds from the extracted archive, so guest changes are not carried into the new image.

## Desktop work next

Before an Omarchy install, upgrade the guest packages, install/configure sudo for the normal user, and copy the pinned Omarchy ARM checkout into the guest. Run its installer dry run in that guest. A graphical guest with a suitable virtual display and a minimal Hyprland session is a separate step; the serial-console VM is the current base for that work. See `status.md` for keyring and boot validation.

The ARM fork uses Lua configuration and Quickshell. Several packages need AUR/source builds, and `--no-aur` leaves parts of the desktop unavailable. Establish the minimal compositor and terminal first before investing in the full Omarchy package set.
