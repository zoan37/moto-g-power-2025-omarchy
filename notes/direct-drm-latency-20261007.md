# Direct-DRM Hyprland and typing latency — 2026-10-07 (Claude Code)

## Result

Hyprland can drive the MediaTek DSI panel directly (no nested Weston), with
libinput touch, at 120 Hz. It is opt-in per boot; the default boot is still
the proven Weston + nested Hyprland desktop.

    touch /home/omarchy/.config/vegas/drm-once   # as omarchy, then reboot

`vegas-desktop` consumes the flag and execs `vegas-desktop-drm`. Optional
experiment env goes in `~/.config/vegas/drm-debug.env` (sourced by the DRM
launcher). Originals are in /var/lib/vegas/render-updates/drm-20261007/.

## What it took

- omarchy must be in `video`: Aquamarine reopens card0 itself after seatd
  hands it the master fd (EACCES -> "CBackend::create() failed!"). native-init
  now does `usermod -aG input,video omarchy`.
- The MediaTek driver reports a phantom connected DP-1 (4096x2160 modes). The
  catch-all monitor rule enabled it with the panel mode; the DSI pipeline then
  logged >10k `DDP_COMPONENT_RDMA0: underflow!` and the panel showed vertical
  stripes while the scanout FB (dumped via GETFB2 + MAP_DUMB) was a perfect
  desktop. monitors.lua disables DP-1 and names DSI-1 when VEGAS_DIRECT_DRM=1.
  Weston's vegas.ini already had DP-1 off. With this, zero underflows.
- autostart.lua skips vegas-touch-pointer (it EVIOCGRABs the Ilitek device)
  when VEGAS_NATIVE_TOUCH=1.
- Never switch compositors live: killing the DRM master (Weston) makes the
  phone reboot within ~2 s (not a requested reboot). Writes not yet synced are
  lost — always `sync` before such tests.

## Latency measurements (scripts/measure-vegas-input.py)

| Setup | tap -> PTY | Foot commit -> frame callback |
| --- | ---: | ---: |
| Weston nested, 30 Hz (Codex baseline) | 2.5 ms | 114 ms (+ Weston frame) |
| Direct DRM 120 Hz, vfr=false | 3.5 ms | 93–123 ms |
| Direct DRM, vfr=true | ~100 ms | ~172 ms |

VFR adds ~100 ms to input handling on both paths; it stays off.

## Where the time goes

Hyprland's debug overlay reports ~105–130 ms render time per frame. perf
(installed from signed Arch ARM packages pushed over the CGI endpoint) shows
~95% of Hyprland samples in llvmpipe JIT code. Render time did not change
with: damage tracking off, scale 1/1.5/2, border 0, cm_enabled=false +
use_fp16=0 (kept, harmless), no wvkbd, no Quickshell, LP_PERF=no_tex, or
pinning to A76/A55 cores. Hyprland's GL pipeline (work FB + copy, per-frame
clear) is GPU-shaped; on llvmpipe it costs roughly a fixed ~100 ms/frame.
Conclusion: Mali GPU rendering is the fix for typing latency, not tuning.

DRM dumb-buffer memory: write 6.7 GB/s, read 0.85 GB/s (vs 2.5 GB/s cached).

## Open issue

Twice, Hyprland on direct DRM deadlocked (all threads in futex, IPC dead)
shortly after toggling `debug:overlay` via `hyprctl eval` during tests. Not
seen in normal use yet, but untested for long sessions; this is why DRM is
not the default. Needs gdb on the phone to get a userspace backtrace.

## Next: GPU

The kbase Mesa trial faults (0x58 DATA_INVALID / 0x59 TILE_RANGE) on real
Hyprland drawing — see gpu-bringup-20261007.md. On the direct-DRM path the
GPU must also render into buffers the display can scan out (dumb/dma-buf
imported into kbase), which avoids Weston's pixman copy entirely.
