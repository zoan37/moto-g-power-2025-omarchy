# License and upstream attribution

The root [MIT license](LICENSE) covers original project code and documentation.
It does not replace the licenses of upstream code, vendored source, patches
to upstream files, or externally obtained binaries.

| Component | Source and licensing |
| --- | --- |
| Omarchy ARM desktop/configuration | [BlackFireAlex/omarchy-android](https://github.com/BlackFireAlex/omarchy-android), MIT; notice in `LICENSES/omarchy-android-MIT.txt` |
| Aquamarine patch | [hyprwm/aquamarine](https://github.com/hyprwm/aquamarine), BSD 3-Clause; notice in `LICENSES/aquamarine-BSD-3-Clause.txt` |
| Mesa/Kbase patches | [RandomCoderOrg/mesa-gfxstream, tensor-g1](https://github.com/RandomCoderOrg/mesa-gfxstream/tree/tensor-g1); upstream per-file licenses apply; core Mesa uses MIT |
| Phone keyboard | [jjsullivan5196/wvkbd](https://github.com/jjsullivan5196/wvkbd), GPL v3 with MIT-licensed components identified in its COPYING files |
| Pointer protocol | `vendor/protocols/wlr-virtual-pointer-unstable-v1.xml`; upstream copyright and MIT notice are embedded in the XML |
| Wi-Fi loader dependency | [Muzuwi/wmt-pyloader](https://github.com/Muzuwi/wmt-pyloader), GPL v3; fetched separately, not vendored here |
| Unlock tooling dependencies | [crabcakes97/Valhalla](https://github.com/crabcakes97/Valhalla) and [Maikyxd/val-protocol](https://github.com/Maikyxd/val-protocol); their distribution terms apply; Val Protocol uses AGPL v3 |

## Keyboard source

The included ARM keyboard binary is accompanied by `vendor/wvkbd/`, based on
upstream revision `e14b53aff4fd1f471add6b21b3885c2cff945509`. That directory
includes the exact reviewed immediate-feedback changes to `main.c`, `drw.c`
and `drw.h`, plus the original license/copyright notices and protocol sources.
`scripts/build-vegas-phone-keyboard.py` checks their hashes, applies the
five-row phone layout and bottom-margin changes, and builds the binary.
These wvkbd modifications remain under its upstream GPL terms.

The builder requires an aarch64 sysroot with Wayland, xkbcommon and pangocairo,
Clang/LLD, make, pkg-config, wayland-scanner, and QEMU's aarch64 runner. Its
default sysroot is the staged SDK in the sibling Fire HD project. No tablet
connection is needed for this cross-build.

## External payloads

Motorola firmware, device-specific partition backups, proprietary ARM libmali,
Chrome, the full desktop rootfs, and downloaded kernel modules are not included
in this Git repository. Obtain them from their respective providers under
their own terms. The experimental libmali preparation script takes an existing
local binary and creates an ignored local trial; it does not download or
redistribute that library.
