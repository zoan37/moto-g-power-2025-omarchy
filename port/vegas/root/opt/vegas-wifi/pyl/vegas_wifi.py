#!/usr/bin/env python3
# Vegas (MT6835) Wi-Fi bring-up using wmt-pyloader's loader/launcher.
# Phase "loader": wmtdetect handshake (creates /dev/stpwmt). Phase "launcher":
# patch/config handshake and wmtWifi power-on (keeps running to serve requests).
import sys, time, logging
import logformat, kmsg, loader, launcher
logformat.get_logger().setLevel(logging.DEBUG)
phase = sys.argv[1]
if phase == "loader":
    sys.exit(loader.do_loader())
kmsg.start_listener()
rc = launcher.do_launcher()
print("launcher rc", rc, flush=True)
while True:
    time.sleep(3600)
