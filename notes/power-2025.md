# Moto G Power 2025 target and bring-up evidence

Updated 2026-10-03. The requested target is native Arch Linux ARM with an Omarchy-derived desktop on the Power 2025. An Omarchy-inspired Android system is an alternative under discussion. No phone is connected; model/build-specific procedures cannot yet be finalized.

## Device identification

- Motorola's [US retail getting-started guide](https://en-us.support.motorola.com/ci/fattach/get/86265551/1740665354/redirect/1/filename/moto%2Bg%2Bpower%2B-%2B2025.US%2BRetail.GSG.es-US.7011AA005585-A.pdf) identifies **XT2515-1**. Carrier and regional variants exist; the actual unit must be surveyed.
- Motorola's [specifications](https://en-us.support.motorola.com/app/answers/detail/a_id/184283/~/specifications---moto-g-power---2025/) list Dimensity 6300, 8GB RAM, 128GB storage, microSD, a 2388 x 1080 LCD, and a 5000mAh battery.
- The [Motorola kernel source request for W1VE36H.10-12](https://github.com/MotorolaMobilityLLC/kernel-mtk/issues/194), marked published, associates Power 2025 with **vegas** and lists separate connectivity, GPU and Motorola kernel-module repositories. Its vendor build fingerprint is not a substitute for the actual Android system fingerprint.
- XT2617 was the provisional 2026 model family. Never mix images from these two product years.

## Unlock evidence and limits

- An owner [reports unlocking an AT&T Power 2025 using an exploit](https://www.reddit.com/r/androidroot/comments/1vpnghr/anyone_else_wish_there_was_a_way_around_carrier/). This is encouraging firsthand testimony, not a verified procedure for a retail XT2515-1 on its delivered firmware.
- The [Valhalla tested-status list](https://github.com/crabcakes97/Valhalla#tested-status) explicitly names Power 2026 and regular G 2025, but does not explicitly name Power 2025. Do not conflate regular G and G Power.
- Check official unlock eligibility for the actual phone first. For any exploit, use the matching stock LK and understand recovery before making writes. No flash command is prepared or authorized by this research alone.

## Pinned source audit

Repository: [MotorolaMobilityLLC/kernel-mtk](https://github.com/MotorolaMobilityLLC/kernel-mtk).
Research branch: `android-16-release-w1ve36h.10-12r3`.
Pinned commit: `72b5d32be0bb7cee12765bf236972f8f4c622b83`.
Downloaded files and SHA-256 values: ignored `artifacts/vegas-source-audit/manifest.json`.
This release is a research reference, not selected firmware for the future phone.

| Area | Source evidence | Still needs runtime verification |
| --- | --- | --- |
| Display | `moto-mgk_64_k515-vegas.config` enables three named 1080 x 2388 panel modules. `mediatek_v2/mtk_drm_drv.c` advertises `DRIVER_MODESET`, `DRIVER_ATOMIC`, GEM and dumb-buffer creation, and has an MT6835 match. | Loaded panel, DRM device/connector, modesetting without Android, usable buffers and a compositor session. DRM display support is separate from accelerated GPU rendering. |
| Touch | `vegas-touch.dtsi` includes Novatek and Ilitek SPI alternatives. | Actual controller, matching module/firmware, evdev events and coordinate mapping. |
| USB shell | `gki_defconfig` enables USB configfs and serial/ACM/ECM/NCM functions. | Actual stock kernel configuration, controller modules, PHY/power dependencies and gadget operation in our initramfs. |
| microSD rootfs | GKI config includes ext4/MMC; MediaTek config lists MMC modules; `vegas-msdc.dtsi` describes the card controller's supply and detection GPIO. | Drivers available before mounting root, actual block path and successful filesystem mount. |
| Diagnostics | GKI config enables `/proc/config.gz` and pstore options. | Read access and whether delivered kernel uses these options. |
| Battery | Earlier 2025 charger/thermal source audit is available in [battery-charging.md](battery-charging.md). | Actual populated hardware, temperature readings, protection configuration and charging behavior under the port. |

These are build inputs, not the final merged `.config`. They do not demonstrate that Linux or Hyprland has booted on this phone. Reuse stock kernel, DTBs/DTBOs and matching vendor modules first; preserve an externally recoverable Android system before experimenting.

## First useful native-Linux milestone

1. Survey the exact phone in Android and bootloader mode using the existing read-only script.
2. Obtain matching stock images and establish recovery; verify unlock and obtain device-specific backups through an actual readable interface.
3. Inspect `boot`, `init_boot`, `vendor_boot`, DTBOs and module-loading order before designing an initramfs. Do not assume `fastboot boot` is supported or that an inactive virtual A/B slot provides a complete fallback OS.
4. Boot the smallest recoverable Linux environment and obtain a USB shell with logs.
5. Load the correct display/touch/storage modules. Verify DRM modesetting and input before attempting Hyprland or Omarchy.
6. Add Arch ARM userspace, Wi-Fi, browser, then Omarchy and a touch-oriented interface. Treat suspend, cellular, audio and Android-app compatibility as separate milestones.

The existing QEMU VM validates generic ARM userspace, not these phone drivers.

## Android alternative

An Omarchy-inspired Android interface could provide shared themes, a small app launcher, scripts and Termux while retaining Android applications. A launcher prototype is distinct from replacing the OS.

A genuinely custom Android system would need a verified device-specific ROM or investigation of a compatible [generic system image](https://source.android.com/docs/core/tests/vts/gsi). Neither route is confirmed for the future unit, and a GSI is not a guarantee of working radio, camera or every app. No LineageOS support for this exact model is claimed here.

[Grok Bot's official mobile documentation](https://docs.x.ai/grok-bot/mobile) lists Android support. App behavior on a custom ROM, including authentication and notifications, still requires testing. Native Linux plus [Waydroid](https://docs.waydro.id/) is another research option after phone bring-up, not a shortcut around missing kernel/display support.
