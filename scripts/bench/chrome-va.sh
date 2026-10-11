#!/bin/bash
# chrome-va.sh URL [chrome args]: a separate Google Chrome test instance (profile
# /tmp/chrome-va, DevTools on port 9333) in the user's Wayland session, through the
# normal launcher (GPU env), with VA-API decode via the mtk_stateful driver.
# Run as root on the phone. Measure with mediawatch.py / chrome-cpu.sh;
# --disable-accelerated-video-decode gives the software baseline.
url=$1; shift
pid=$(pgrep -f 'quickshell -n' | head -1)
mapfile -d '' E < /proc/$pid/environ
exec runuser -u omarchy -- env -i "${E[@]}" \
  LIBVA_DRIVERS_PATH=/opt/vegas-video/va LIBVA_DRIVER_NAME=mtk_stateful \
  /usr/bin/google-chrome-stable --user-data-dir=/tmp/chrome-va --no-first-run --no-default-browser-check \
  --remote-debugging-port=9333 --enable-logging=stderr \
  --enable-features=VaapiVideoDecoder,VaapiIgnoreDriverChecks,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL \
  --disable-features=UseChromeOSDirectVideoDecoder "$@" "$url"
