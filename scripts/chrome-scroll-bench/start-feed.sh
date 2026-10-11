#!/bin/bash
# Benchmark Chrome: fresh empty profile, local synthetic feed, DevTools on 9222
# (laptop reaches it at 192.168.77.1:9223). Never the user's profile.
pkill -KILL -f chrome-bench 2>/dev/null; pkill -x socat; sleep 1
rm -rf /tmp/chrome-bench
mapfile -d "" E < /proc/$(pgrep -f "quickshell -n" | head -1)/environ
cd /home/omarchy
runuser -u omarchy -- env -i "${E[@]}" setsid /usr/bin/google-chrome-stable \
  --user-data-dir=/tmp/chrome-bench --no-first-run --no-default-browser-check --remote-debugging-port=9222 \
  --app=file:///home/omarchy/bench-feed/index.html >/run/chrome-bench.log 2>&1 </dev/null &
setsid socat TCP-LISTEN:9223,bind=192.168.77.1,fork,reuseaddr TCP:127.0.0.1:9222 </dev/null >/dev/null 2>&1 &
for i in $(seq 30); do curl -s -m 1 http://127.0.0.1:9222/json/version >/dev/null && break; sleep 1; done
curl -s http://127.0.0.1:9222/json/version | grep -E 'Browser"'
pgrep -a -f 'chrome-bench' | grep -v -- '--type=' | head -1 | grep -o -E -- '--use-gl=[a-z]+|--disable-gpu-rasterization' | tr '\n' ' '
