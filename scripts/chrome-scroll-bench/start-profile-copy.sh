#!/bin/bash
# Run on the phone as root, e.g. ../pc < start-profile-copy.sh. ONLY with a
# signed-in account you are fine with being scrolled: it copies the default
# Chrome profile to chrome-bench (delete it afterwards).
# Benchmark Chrome on X with a temporary copy of the user's profile (alt account
# signed in, user's OK), libmali/GPU launcher defaults, DevTools on 9222 -> 9223.
pkill -TERM -x chrome; sleep 3; pkill -KILL -x chrome 2>/dev/null; pkill -x socat
rm -rf /home/omarchy/.config/chrome-bench
cp -a /home/omarchy/.config/google-chrome /home/omarchy/.config/chrome-bench
rm -f /home/omarchy/.config/chrome-bench/Singleton*
mapfile -d "" E < <(grep -z -v -E "^(LIBGL_DRIVERS_PATH|LD_LIBRARY_PATH|LD_PRELOAD)=" /proc/$(pgrep -f "quickshell -n" | head -1)/environ)
cd /home/omarchy
runuser -u omarchy -- env -i "${E[@]}" setsid /usr/bin/google-chrome-stable \
  --user-data-dir=/home/omarchy/.config/chrome-bench --remote-debugging-port=9222 \
  --app=https://x.com/home >/run/chrome-bench.log 2>&1 </dev/null &
setsid socat TCP-LISTEN:9223,bind=192.168.77.1,fork,reuseaddr TCP:127.0.0.1:9222 </dev/null >/dev/null 2>&1 &
for i in $(seq 30); do curl -s -m 1 http://127.0.0.1:9222/json/version >/dev/null && break; sleep 1; done
curl -s http://127.0.0.1:9222/json/version | grep -E 'Browser"'
