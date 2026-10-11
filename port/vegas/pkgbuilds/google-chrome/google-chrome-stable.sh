#!/bin/bash
# Moto G Power (vegas) launcher for Google Chrome.
# On GPU desktop boots Chrome composites on the Mali through ANGLE/GLES, with
# page and canvas drawing kept on the CPU. Scripted touch scrolling on a feed
# page (notes/chrome-scrolling-20261010.md): software 29 fps, Mesa kbase
# compositing 66, ARM libmali compositing 78 (120 Hz panel, Hyprland vfr on).
#   ~/.config/vegas/chrome-software  stay on software GL
#   ~/.config/vegas/chrome-libmali   use the ARM r48 compatibility trial
#                                    instead of Mesa kbase (experimental)
# Hyprland's session points GL at the patched kbase Mali libs, so the
# software path drops those first.
# The setuid chrome-sandbox works without unprivileged user namespaces.
config="${XDG_CONFIG_HOME:-$HOME/.config}"
libmali=/opt/vegas-gpu/libmali-trials/reproducible-v3
gpu=()
unset LD_PRELOAD
if [[ ! -f $config/vegas/chrome-software && -c /dev/mali0 && -f /run/vegas-gpu-ready ]]; then
  if [[ -f $config/vegas/chrome-libmali && -f $libmali/lib/libmali.so.0.48.0 && -w /dev/dma_heap/system ]]; then
    # The adapter widens r48's V3 JM atoms to the stock kernel's 72-byte
    # layout; it only touches ioctls on /dev/mali0.
    export VEGAS_LIBMALI_JM_COMPAT=stride72 LD_PRELOAD=$libmali/libmali-jm-compat.so
    export LD_LIBRARY_PATH=$libmali/lib
    unset LIBGL_DRIVERS_PATH
    # With touch boost, page drawing on the GPU beats the CPU on X at 120 Hz
    # (28% vs 37% late frames). ~/.config/vegas/chrome-cpu-raster opts out.
    raster=(--enable-gpu-rasterization)
    [[ -f $config/vegas/chrome-cpu-raster ]] && raster=(--disable-gpu-rasterization)
  else
    export LD_LIBRARY_PATH=/opt/vegas-gpu/mesa-kbase/lib
    export LIBGL_DRIVERS_PATH=/opt/vegas-gpu/mesa-kbase/lib/dri
    raster=(--disable-gpu-rasterization)
    [[ -f $config/vegas/chrome-gpu-raster ]] && raster=(--enable-gpu-rasterization)
  fi
  # 2D canvas stays on the CPU: it benchmarked slower on both GPU drivers.
  gpu=(--use-gl=angle --use-angle=gles --ignore-gpu-blocklist
       --disable-accelerated-2d-canvas "${raster[@]}")
else
  unset LD_LIBRARY_PATH LIBGL_DRIVERS_PATH
fi
flags=()
[[ -f $config/chrome-flags.conf ]] && mapfile -t flags < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$config/chrome-flags.conf")

# Hardware video decode (H.264, VP9) through the mtk_stateful VA-API driver on
# MediaTek's decoder, which video-prepare loads at boot
# (notes/video-decode-chrome-va-20261011.md). ~/.config/vegas/no-video-decode
# turns it off. Chrome keeps only the last --enable-features/--disable-features,
# so the user's lists from chrome-flags.conf are merged in.
enable=() disable=() rest=()
for f in "${flags[@]}"; do
  case $f in
    --enable-features=*) enable+=("${f#*=}") ;;
    --disable-features=*) disable+=("${f#*=}") ;;
    *) rest+=("$f") ;;
  esac
done
if [[ ! -f $config/vegas/no-video-decode && -f /opt/vegas-video/va/mtk_stateful_drv_video.so ]] &&
   compgen -G '/dev/video[0-9]*' >/dev/null; then
  export LIBVA_DRIVERS_PATH=/opt/vegas-video/va LIBVA_DRIVER_NAME=mtk_stateful
  enable+=(VaapiVideoDecoder,VaapiIgnoreDriverChecks,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL)
  disable+=(UseChromeOSDirectVideoDecoder)
fi
features=()
(( ${#enable[@]} )) && features+=("--enable-features=$(IFS=,; echo "${enable[*]}")")
(( ${#disable[@]} )) && features+=("--disable-features=$(IFS=,; echo "${disable[*]}")")
# Phone: pull down at the top of a page to reload it, as in Chrome on Android
# (tested on the touchscreen). User flags come later, so --pull-to-refresh=0 in
# chrome-flags.conf turns it off.
exec /opt/google/chrome/google-chrome --ozone-platform=wayland --enable-wayland-ime \
  --password-store=basic --pull-to-refresh=1 "${gpu[@]}" "${rest[@]}" "${features[@]}" "$@"
