#!/bin/bash
# Moto G Power (vegas) launcher for Google Chrome.
# Hyprland's session points GL at the patched kbase Mali libs, which crash
# Chrome's GPU process (no eglDupNativeFenceFDANDROID yet); use the system
# Mesa instead so WebGL keeps working in software. The setuid chrome-sandbox
# works without unprivileged user namespaces, so Chrome keeps its sandbox.
unset LD_LIBRARY_PATH LIBGL_DRIVERS_PATH
conf="${XDG_CONFIG_HOME:-$HOME/.config}/chrome-flags.conf"
flags=()
[[ -f $conf ]] && mapfile -t flags < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$conf")
exec /opt/google/chrome/google-chrome --ozone-platform=wayland --enable-wayland-ime \
  --password-store=basic "${flags[@]}" "$@"
