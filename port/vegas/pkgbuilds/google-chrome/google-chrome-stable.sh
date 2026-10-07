#!/bin/bash
# Moto G Power (vegas) launcher for Google Chrome.
# Default: system Mesa (software GL; WebGL still works). Hyprland's session
# points GL at the patched kbase Mali libs, so drop those first.
# Opt in to the Mali GPU with ~/.config/vegas/chrome-gpu (GPU desktop boots
# only): ANGLE on the kbase GLES driver for compositing, with page and canvas
# drawing kept on the CPU, which benchmarks equal-or-better on this driver.
# The setuid chrome-sandbox works without unprivileged user namespaces.
config="${XDG_CONFIG_HOME:-$HOME/.config}"
gpu=()
if [[ -f $config/vegas/chrome-gpu && -c /dev/mali0 && -f /run/vegas-gpu-ready ]]; then
  export LD_LIBRARY_PATH=/opt/vegas-gpu/mesa-kbase/lib
  export LIBGL_DRIVERS_PATH=/opt/vegas-gpu/mesa-kbase/lib/dri
  gpu=(--use-gl=angle --use-angle=gles --ignore-gpu-blocklist
       --disable-accelerated-2d-canvas --disable-gpu-rasterization)
else
  unset LD_LIBRARY_PATH LIBGL_DRIVERS_PATH
fi
flags=()
[[ -f $config/chrome-flags.conf ]] && mapfile -t flags < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$config/chrome-flags.conf")
exec /opt/google/chrome/google-chrome --ozone-platform=wayland --enable-wayland-ime \
  --password-store=basic "${gpu[@]}" "${flags[@]}" "$@"
