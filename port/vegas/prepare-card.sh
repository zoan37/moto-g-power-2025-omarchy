#!/bin/bash
set -e
test -x /omarchy/usr/local/libexec/vegas/native-init
test -x /omarchy/usr/local/bin/vegas-touch-pointer
test -x /omarchy/usr/local/bin/wvkbd-mobintl
chroot /omarchy /usr/bin/getent passwd omarchy | awk -F: '{print "Desktop user: " $1 ", uid=" $3 ", gid=" $4}'
chroot /omarchy /bin/bash -n /usr/local/libexec/vegas/native-init /usr/local/bin/vegas-desktop
chroot /omarchy /usr/bin/weston --version
for name in dev proc sys run; do
  mkdir -p "/omarchy/$name"
  mount --bind "/$name" "/omarchy/$name"
done
trap 'for name in run sys proc dev; do umount "/omarchy/$name"; done' EXIT
mkdir -p /omarchy/tmp/vegas-version-check
chown 1000:1000 /omarchy/tmp/vegas-version-check
chmod 0700 /omarchy/tmp/vegas-version-check
chroot /omarchy /usr/bin/runuser -u omarchy -- env \
  XDG_RUNTIME_DIR=/tmp/vegas-version-check LD_LIBRARY_PATH=/opt/omarchy-android/aquamarine/lib \
  /opt/omarchy-android/hyprland/bin/Hyprland --version
rm -rf /omarchy/tmp/vegas-version-check
chroot /omarchy /usr/local/bin/wvkbd-mobintl --help
echo VEGAS_PHONE_OVERLAY_CHECKED
