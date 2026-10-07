#!/bin/bash
export PATH=/opt/omarchy-android/hyprland/bin:/usr/local/bin:/usr/bin
export LD_LIBRARY_PATH=/opt/omarchy-android/aquamarine/lib
echo NATIVE_ARCH_HEALTH
readlink /proc/1/exe
findmnt -n -o SOURCE,FSTYPE,OPTIONS /
uname -r
pacman -Q bash glibc foot quickshell mesa
free -m
ps -eo pid,comm,%cpu,rss --sort=-%cpu | head -n 16
for name in temp capacity voltage_now current_now status; do
  printf 'BATTERY_%s:' "$name"
  cat "/sys/class/power_supply/battery/$name" 2>/dev/null || true
done
runuser -u omarchy -- env XDG_RUNTIME_DIR=/run/user/1000 LD_LIBRARY_PATH="$LD_LIBRARY_PATH" hyprctl -i 0 reload
runuser -u omarchy -- env XDG_RUNTIME_DIR=/run/user/1000 LD_LIBRARY_PATH="$LD_LIBRARY_PATH" hyprctl -i 0 configerrors
runuser -u omarchy -- env XDG_RUNTIME_DIR=/run/user/1000 LD_LIBRARY_PATH="$LD_LIBRARY_PATH" hyprctl -i 0 -j monitors
pgrep -a -x Hyprland
pgrep -a -x weston
pgrep -a -x quickshell
pgrep -a -x foot
pgrep -af '^vegas-touch-pointer$'
pgrep -a -f wvkbd-mobintl
tail -n 15 /home/omarchy/.local/state/vegas/touch.log
echo NATIVE_ARCH_HEALTH_END
