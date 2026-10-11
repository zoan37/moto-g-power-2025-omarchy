# KMS render-fence sync on direct DRM (Chrome "skipped lines"), 2026-10-11 (Claude Code)

## Summary

When Chrome scrolls under X.com's blurred sticky header, rows sometimes appear
skipped. The cause is that a Hyprland frame can be scanned out before the Mali
GPU has finished rendering it. This is a GPU/KMS sync bug, not a Chrome bug.
Fixing it by passing the fence through KMS (IN_FENCE_FD) is not possible on
this kernel, because the vendor mediatek-drm ignores that fence. The fix used
is the fallback: Aquamarine waits on the CPU for Hyprland's render fence
before each atomic commit. It is opt-in with `AQ_KMS_CPU_FENCE_WAIT=1`. Only
Aquamarine changes; the Hyprland binary stays the same.

## What runs on the phone

- `/opt/omarchy-android/{hyprland,aquamarine}` comes unchanged from the
  omarchy-android v0.1.1 release bundle (manifest/release.lock), copied from
  `fire-hd8-omarchy/working/desktop/rootfs`. Its SHA256SUMS match the hashes
  recorded for the phone:
  - libaquamarine.so.0.14.0 `8a2a2cbe…135e`: aquamarine a79fb21b (0.14.0) +
    omarchy-android patches/aquamarine/0001 (KGSL buffer pipeline).
  - Hyprland `c1f642c1…`: 0.56.1, upstream 5c9377c1 + patches/hyprland/0001.
    `--version` reports commit 5c897c03 ("v0.56.1-1-g5c897c03"), which is the
    local fork commit holding that patch.
- The omarchy-android builder (builder/guest/build-graphics.sh) produced them
  inside an Arch Linux ARM guest. The flow this repo has used to rebuild
  Aquamarine for vegas is different: it cross-compiles on the laptop with an
  isolated Arch aarch64-linux-gnu-gcc 16.1 against the fire-hd8 rootfs as
  sysroot, then runs a QEMU `Hyprland --version` check
  (artifacts/vegas-linux-bringup/aquamarine-damage). I reused that flow. The
  Spark and SER8 were not needed.

## Code reading

Hyprland 0.56.1 (third_party/hyprland @ 5c9377c1):
- `explicitSyncSupported()` is just `EGL_ANDROID_native_fence_sync` on the EGL
  display (OpenGL.cpp:385). The patched Mesa panfrost-kbase exposes it, so
  this is true on vegas.
- `CHyprGLRenderer::endRender` (GLRenderer.cpp:83) takes the explicit path:
  1. It creates a native-fence EGLSync (eglCreateSyncKHR + glFlush + dup fd).
  2. It keeps the client buffers sampled this frame (`m_usedAsyncBuffers`)
     until that fd polls readable, so wl_buffer.release already waits for
     Hyprland's GPU reads. Part (b) needs no change.
  3. It stores the fd in `m_inFence` and calls
     `output->state->setExplicitInFence(fd)` on every normal frame.
- The client linux-drm-syncobj protocol is created only when
  `supportsDrmSyncobjTimeline()` is true (ProtocolManager.cpp:249). It stays
  off.
- `output->supportsExplicit` is only reported in SystemInfo.

Aquamarine 0.14.0:
- `supportsExplicit = supportsTimelines && OUT_FENCE_PTR && IN_FENCE_FD`
  (DRM.cpp:1697).
- Atomic.cpp adds IN_FENCE_FD only when `supportsExplicit` is true. On vegas,
  Hyprland's fence is therefore silently dropped, and the commit relies on
  implicit sync. kbase provides none (no dma_resv imports).

Kernel side (third_party/kernel/kernel-mtk mediatek_v2, plus the shipped
vendor_boot `mediatek-drm.ko`, vermagic 5.15.180-android13-8):
- `mtk_atomic_commit` → `mtk_atomic_complete` → `mtk_atomic_wait_for_fences`
  → `mtk_fb_wait()`. Inside `mtk_fb_wait`, the `dma_resv_wait_timeout_rcu`
  call is commented out.
- Nothing calls `drm_atomic_helper_wait_for_fences`.
- The module imports no `dma_fence_wait*`, no `dma_resv_*` and no
  `drm_atomic_helper_wait_for_fences`.
- DRM core accepts IN_FENCE_FD and stores `plane_state->fence`, but this
  driver never waits on it. Android HWC waits for fences in userspace.

So even with timelines or `AQ_KMS_FENCE_WITHOUT_TIMELINES`, IN_FENCE_FD would
not stop the scanout race on vegas. Blur is the heaviest GPU work in a
Hyprland frame, which makes it the most likely place for the display engine
to read rows the GPU has not written yet. Those rows show the buffer's older
contents, scrolled by a different amount, so they look like skipped lines.

## Change (port/vegas/patches/aquamarine-kms-cpu-fence-wait.patch)

The patch applies on top of a79fb21b + omarchy-android 0001 and only touches
src/backend/drm/DRM.cpp.

- `AQ_KMS_CPU_FENCE_WAIT=1`: in `CDRMOutput::commitState`, just before
  `connector->commitState(data)`, it does the following for real commits
  (not tests) that carry a buffer and an in-fence ≥ 0:
  - `poll(fence, POLLIN)` with a 100 ms limit. On timeout it logs a warning
    and commits anyway.
  - Logs once at startup, and warns once if a buffer arrives without a fence
    (meaning Hyprland fell back to implicit sync).
  - Every 600 frames it logs at DEBUG:
    `drm: KMS CPU fence wait: N frames, render unfinished at commit in M, avg wait X us, max Y us, timeouts Z`.
    M > 0 is direct evidence of the race.
- `AQ_KMS_FENCE_WITHOUT_TIMELINES=1`: `supportsExplicit` no longer requires
  timelines, so IN_FENCE_FD is attached. This is useful only on drivers that
  honour IN_FENCE_FD. It is not used on vegas.
- Adds `#include <cmath>`, which GCC 16 requires.
- Both env vars default off, so other hardware behaves exactly as before.
- `port/vegas/root/usr/local/bin/vegas-desktop-drm` sets
  `AQ_KMS_CPU_FENCE_WAIT=1` on the Mali path only. The stock library ignores
  it.

Build, hashes and install steps are in
artifacts/vegas-linux-bringup/aquamarine-kmsfence/BUILD.md. The new
libaquamarine.so.0.14.0 has sha256 `3c3308b4…dcd9`. Its SONAME and exported
symbol set are identical to the original. The QEMU Hyprland `--version`
check passes.

## Trade-off and risks

- The CPU blocks for the rest of each frame's GPU time on Hyprland's main
  thread. The vendor atomic ioctl is already synchronous (it waits for
  vblank). Input and event handling are delayed by that time. If a frame's
  render misses vblank, it is shown one frame late rather than torn, so
  GPU-bound blur at 120 Hz may lose frames that were previously shown
  half-drawn. vegas-refresh 60 is the lever if this happens.
- If a kbase fence never signals, each affected frame stalls 100 ms instead
  of deadlocking.
- Not addressed: the client acquire side. Chrome's buffers rely on Chrome
  waiting on its own GL fence before it commits, which is what it does
  without compositor explicit sync. I did not re-verify this here. Glitches
  inside Chrome's own content would point there.
- Future option: defer the atomic commit from the event loop until the fence
  fd is readable, which avoids blocking. It needs aquamarine/Hyprland commit
  plumbing (commit currently returns synchronously).

## How to verify on the phone

1. The Hyprland log has `EGL_ANDROID_native_fence_sync` in "Supported EGL
   display extensions". It has no "Explicit sync failed" or
   "eglDupNativeFenceFDANDROID failed" errors.
2. The log has
   `drm: AQ_KMS_CPU_FENCE_WAIT: waiting for render fences ...` and periodic
   `KMS CPU fence wait:` lines. "render unfinished at commit in M" with M > 0
   while scrolling X.com confirms the race. "Explicit sync unsupported" is
   expected to stay; modetest IN_FENCE_FD stays -1 by design.
3. A/B test: put `export AQ_KMS_CPU_FENCE_WAIT=0` in
   `~/.config/vegas/drm-debug.env`, reboot, and the skipped lines should
   return.
