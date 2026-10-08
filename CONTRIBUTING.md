# Contributing

Start with README.md and the current status record. Older notes preserve
intermediate failures; label new observations with the date, exact model,
firmware/kernel version, rendering path and whether they were tested on
physical hardware. Distinguish benchmark timings from perceived UI latency.

Keep changes to the phone's user configuration and /usr/local helpers;
do not patch the packaged /usr/share/omarchy tree. Avoid switching live DRM
compositors: releasing DRM master has triggered resets on this device.

Build and inspect changes before deploying. Flashing, formatting storage,
changing the boot chain, and rebooting require a deliberate hardware session
with matching recovery files. Do not run those operations as generic tests.
Cross-builds require the staged SDK described in the build scripts.

Do not commit partition dumps, firmware, proprietary drivers, raw transcripts,
unlock keys, serial/IMEI numbers or Wi-Fi credentials. Sanitize diagnostic
output and screenshots before attaching them to issues. Publish useful driver
errors and measurements rather than entire device surveys.

Before a source-only contribution, check git diff --check, parse modified
Python scripts, and run bash -n on modified Bash scripts. For driver changes,
report actual rendering/fence tests and kernel faults; compilation alone does
not establish hardware compatibility. Upstream files and modifications must
retain the licenses recorded in THIRD_PARTY.md.
