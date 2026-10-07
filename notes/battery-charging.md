# Battery and charging research

Researched 2026-10-03. No phone is connected; native Linux charging has not been validated.

## Conclusion

The available evidence supports expecting dedicated hardware protection in this phone family. It does not establish that every battery safety function survives an arbitrary replacement operating system. Preserve and audit the exact device's charging and thermal stack before charging under native Linux.

## Evidence and its limits

- **2026 documentation:** Motorola describes charging slowdown and shutdown when the phone overheats. This establishes stock-system behavior, without separating hardware from software enforcement. [Motorola 2026 user guide](https://en-us.support.motorola.com/app/answers/detail/a_id/190548/~/user-guide-%28pdf%29---moto-g-power--2026).
- **Relationship to the older design:** Motorola's September 19, 2025 FCC amendment adds XT2617-1/-2/-3/XT2617V to IHDT56AU5 and lists a battery change. Shared certification is evidence of a related design, not proof of identical populated charger ICs or limits. [Manufacturer's amendment](https://fccid.io/IHDT56AU5/Letter/IHDT56AU5-cvrltr-FCC-Class-II-Permissive-Change-r2-8689327).
- **2026 battery:** the filed test report lists RB52 batteries from Sunwoda and SCUD. The internal photographs show the assembled pack and board; the inspected images do not establish the pack protector IC or its thresholds. [Filed test report](https://device.report/m/493cd213f717335cf7e659024a4fd7b2259c7e2e31a09cb3bc8e3200005a9f4f), [internal photographs](https://fccid.io/IHDT56AU5/Internal-Photos/IHDT56AU5-InPho-Part1-6-8641060).
- **Related Motorola kernel source:** the 2025 `vegas` device tree contains SGM41543D and SC89890H charger nodes, with SC8541 and NU2115 charge-pump alternatives. Multiple nodes are not evidence that every part is populated. The SC89890H configuration enables termination and a safety timer, while SC8541 settings disable several local thermal protections. Its driver reads those settings and writes the corresponding fields. These are 2025 configurations, not verified 2026 runtime settings. [Charger configuration](https://github.com/MotorolaMobilityLLC/kernel-mtk/blob/android-16-release-w1ve36h.10-12r3/arch/arm64/boot/dts/mediatek/vegas-charger.dtsi), [SC8541 driver](https://github.com/MotorolaMobilityLLC/motorola-kernel-modules/blob/android-16-release-w1ve36h.10-12r3/drivers/power/sc8541_mmi/sc8541_mmi.c).
- **Actual schematic caveat:** the 2025 Motorola Level 3 service manual, page 42, identifies SGM41543D and draws its THM input connected to a fixed 10 kΩ/10 kΩ divider. My interpretation is that this input does not directly measure battery temperature in that depicted circuit. Page 44 depicts an SC8541 charge pump; page 45's OVP section contains external FETs, not a separately identified battery-pack protection controller. [2025 service manual](https://documents.cdn.ifixit.com/QEglGyneHgCAZIbK.pdf). These findings cannot be assumed for the changed 2026 battery or every board revision.
- **Charger silicon capabilities:** SG Micro documents autonomous charge initiation/termination, voltage/current protections, a safety timer, and chip thermal regulation/shutdown for SGM41543D. Battery-temperature protection requires the appropriate thermistor connection. Hardware enforcement can depend on wiring and programmable settings. Chip junction temperature is not battery-cell temperature. [Manufacturer's datasheet](https://www.sg-micro.com/rect/assets/20df191c-2caf-444b-8f2b-e24804761a6a/SGM41543_SGM41543D.pdf).

## Bring-up requirements

1. Capture the shipped model/build and stock battery voltage, temperature, current, and charging status using read-only interfaces. Record the battery's actual specified limits from matching documentation.
2. Identify the probed charger/charge-pump drivers, vendor modules, device-tree overlays, thermal sensors, and any userspace charging services. Reusing the stock kernel alone does not prove the complete stack is running.
3. Preserve the matching configuration and stock charging environment. Use stock Android for charging during initial native Linux experiments.
4. Before enabling native charging, establish credible battery-temperature readings, working current/voltage limits and charge termination, and a temperature-dependent charge inhibit. Check suspend, reboot, and failed-service behavior through code/configuration and controlled normal-operation observations.
5. Verify ordinary charging first; assess fast charging separately. Do not deliberately overheat, short, overcharge, or deeply discharge the battery to test protection.

Raw downloaded documents, source excerpts, and research images are under ignored `artifacts/battery-research/`. Do not treat the 2025 source as a drop-in 2026 charging configuration.
