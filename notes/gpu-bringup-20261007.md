# Native Mali GPU investigation — 2026-10-07

The Moto G Power 2025 XT2515-1 (vegas, MT6835 / Dimensity 6300) continues to
run native Arch ARM and Omarchy. The working desktop renders with llvmpipe.
GPU kernel access is verified; GPU-rendered pixels are not yet verified.

## Hardware and kernel access

The stock kernel has no CONFIG_DRM_PANFROST. Its separate MediaTek display
device drives the panel, while the vendor GPU uses Mali Kbase Job Manager.
Original modules from this phone's existing firmware were extracted and hashed.
The dependency closure contains 62 modules, including the DT clock supplier
fhctl. Loading mcupm and fhctl resolved GPU probe deferral; Mali then bound to
13000000.mali and exposed /dev/mali0.

The independent property query returned API 11.38, product ID 0x9093, shader
mask 0x5 (two cores), and maximum GPU frequency 1,100,000 kHz. This query
submits no GPU jobs. The module load is temporary; native startup does not
automatically load the GPU stack. No boot images or firmware were replaced.

`scripts/prepare-vegas-gpu-modules.py` prepares original dependency-ordered
modules and a manifest. `scripts/probe-vegas-stock-gpu.py` checks the archive
by default; its explicit `--load` path also checks native root, kernel,
hostname and private SD identity, and uses a reboot watchdog around loading.

## Isolated userspace experiment

Source: [RandomCoderOrg/mesa-gfxstream, tensor-g1 branch](https://github.com/RandomCoderOrg/mesa-gfxstream/tree/tensor-g1),
commit 4a03b179a986578d2c6685299ea9e7ae8fa387ec. This experimental Panfork
build includes Mali-G57 model recognition and both JM and CSF interfaces.
It was cross-built against the existing Arch ARM SDK with Panfrost, EGL,
GBM and Wayland enabled, under /opt/vegas-gpu/mesa-kbase. System Mesa and
the desktop's graphics environment were not replaced.

The first EGL attempt failed device permissions: omarchy is not in video.
Temporary ownership of /dev/mali0 was changed to omarchy:video, mode 0660,
for this boot's test. This ownership is not a persistent installation.

With device access, EGL initialization aborted. A fresh private core and
AArch64 backtrace identified a duplicate free of the DRI screen: both
drisw_init_screen's failure path and its caller driCreateNewScreen2 freed
the same object. The trial now leaves ownership to the caller. A second
phone test returned EGL_NOT_INITIALIZED without aborting, but still failed
pipe-screen creation. Both copies of the diagnostic core were deleted.

Saved patches under port/vegas/patches:

- mesa-kbase-modern-glibc.patch namespaces this older Mesa's private C11
  call_once/once_flag implementation to avoid current glibc collisions.
- mesa-kbase-init-ownership.patch fixes the duplicate free and adds
  opt-in loader diagnostics.
- mesa-kbase-vegas-trace.patch adds opt-in descriptor selection, Panfrost
  model, Kbase API negotiation and named setup-stage diagnostics.

All diagnostics require VEGAS_GPU_TRACE=1. The latest diagnostic driver
build completed successfully and is saved as panfrost-vegas-trace.so and
its compressed copy under artifacts/vegas-linux-bringup/mesa-kbase-build.
trace-manifest.json records binary, compressed binary, source and patch hashes.
This diagnostic build has not been uploaded or tested on the phone.

The original trial archive/manifest still describe the initial installation.
The installed panfrost_kbase_dri.so alias was subsequently replaced atomically
with the ownership-fix build; its other hardlink aliases retain the initial
build. Do not treat the original archive manifest as proof of the current
phone alias. Future trial updates should verify hashes and replace the one
test alias atomically; avoid modifying a mapped library in place.

## Next direct investigation

The next EGL run with VEGAS_GPU_TRACE=1 should reveal whether the selected
pipe descriptor enters Panfrost and, if so, which Kbase initialization stage
fails. The existing log proves only that the loader selects driver name
panfrost and pipe-screen creation returns NULL.

`bringup/gpu-context-probe.py --compare-version` independently performs JM
version negotiation, context flags, tracking mmap, property retrieval and
EXEC/JIT virtual-address setup on fresh fds. It compares the driver's zero
version query with an explicit 11.35 request. It submits no GPU job; close
releases each context. Python syntax/help checks pass, and a compiled C
check confirmed its five ioctl request values against the driver headers.
Actual device results remain pending.

Do not change the desktop renderer until an isolated EGL context can clear
and read back the expected pixel. Subsequent work must test real drawing,
buffer sharing with the display stack, compositor operation, timing and
temperature separately. The original software-rendered desktop remains the
working fallback, with its original Aquamarine library restored and checked.

## Related projects reviewed

[Omarchy ARM](https://github.com/alexisraitano-myffu/omarchy-arm) and
[Omarchy Android](https://github.com/BlackFireAlex/omarchy-android) already
provide foundations used by this port. Snapdragon GPU interfaces differ
from Mali Kbase. [Omarchy Snapdragon](https://github.com/bprendie/omarchy-snapdragon)
and [Omarchy Dragon](https://omarchy.org/news/2026/09/introducing-omarchy-dragon/)
provide useful ARM laptop work, rather than an established MT6835 GPU driver.

[Omarchy Mobile](https://github.com/Cube-Oakley/omarchy-mobile) explores
Hyprland/Quickshell phones; its Pixel 7 Pro work uses different Mali hardware
and a mainline graphics stack. [moarchy](https://github.com/SimonSchubert/moarchy)
currently targets Pixel 3a with Sway and is a possible UI reference.

The most specific alternative lead is
[mesa-panvk-mali-g57](https://github.com/apexspan-svg/mesa-panvk-mali-g57),
whose author reports Vulkan and Zink on Dimensity 6300 / Mali-G57 MC2 with
JM API 11.38/11.46. Its documented build targets Android/Termux and X11,
so native glibc/Wayland compatibility is unverified. The hardware match is
promising evidence for investigation, not proof it works on this phone.

## Access checkpoint

The session switched to managed workspace-write with restricted network
access. Requests to 192.168.77.1:8080 returned Errno 1 Operation not permitted.
The user then selected Full access; the immediate retry still returned that
denial. Recheck after the execution environment refreshes. Do not work around
the policy through a different transport. No new phone changes occurred
after this access cutoff; all subsequent diagnostics were prepared locally.

## First GPU desktop boot: flicker (2026-10-07, continued in Claude Code)

The one-boot trial reached a running Hyprland with panfrost_kbase_dri.so,
libEGL, libGLESv2 and libgbm from /opt/vegas-gpu mapped, but the screen
flickered heavily. /var/log/vegas/desktop.log was flooded with Mesa
"Atom N reported event 0x58!" (484x) and "0x59!" (271x) within ~9 minutes:
kbase DATA_INVALID_FAULT and TILE_RANGE_FAULT. Faulted fragment/tiler jobs
leave partially drawn buffers that Weston still scans out, hence flicker.
The isolated clear and 600-draw shader tests never exercised these
descriptors (textures, blending, varied framebuffer sizes/formats).
Log: private/linux-bringup/gpu-20261007/flicker-faults.log.

Returned to software rendering via vegas-power reboot (trial flag was
already consumed; /run/vegas-gpu-ready is tmpfs). After reboot: no GPU
marker, zero kbase faults, webcam shows stable desktop.

Next: reproduce off-screen with a texture + blend + non-square FBO test
under PAN_MESA_DEBUG=trace/sync to capture the first faulting job's
descriptors, then compare tiler heap / framebuffer descriptor layout against
the stock kbase module expectations for this MediaTek build (same class of
layout mismatch as the earlier job-struct size fix and tiler init setting).

## Root cause of the GPU faults/flicker: JM BOs freed while in flight (fixed)

Off-screen reproduction: `bringup/gpu-hypr-probe.c` (W H FRAMES flags; flags
s=depth/stencil, b=blend, c=scissors, t=glTexSubImage each frame, o=copy pass,
x=scissor outside a small FBO). Basic full-screen drawing on the Mali takes
~1.5 ms/frame with zero faults. Any run with `t` faulted after ~10-30 frames:
0x42 JOB_READ_FAULT, 0x04 TERMINATED, sometimes 0x59 TILE_RANGE_FAULT, plus
wrong pixels — the same codes as the flickering desktop.

Cause: `kbase_callback_all_queues()` (used by panfrost_bo_unreference to defer
freeing until submitted jobs finish) only walks CSF event slots;
`event_slot_usage` is never incremented on JM, so it always reported idle.
BOs (e.g. texture shadow copies from glTexSubImage on a busy texture) went
straight to the BO cache / MEM_FREE while atoms still read them.

Fix: port/vegas/patches/mesa-kbase-jm-deferred-free.patch — record each JM
atom's submit sequence, queue frees behind every atom submitted before them,
run them from kbase_handle_events once drained (callbacks outside kbase
locks). Probe: old driver FAIL on t/sbcto/sbctox x300 frames; patched PASS,
0 faults. Driver sha256 69652ad13a5c72809ad9a7acb7e5f2522bc4a19fca6395b91f0f4e87b47417ad,
installed as /opt/vegas-gpu/mesa-kbase/lib/dri/panfrost_kbase_dri.so (old one
kept as .pre-jmdefer); gpu-prepare hash updated (repo + phone).

GPU desktop trial (Weston + nested Hyprland on Mali), first boot after fix:
0 faults, normal picture on webcam, tap->PTY ~7 ms, Foot commit->frame
callback median ~30 ms (was ~114 ms on llvmpipe). Remaining latency is mostly
the 30 Hz nested refresh (AQ_WAYLAND_REFRESH_MHZ=30000).

The earlier hard resets happened while running a second, nested GPU Hyprland
on the *unpatched* driver with modules loaded live; not re-tested since.

Next: 60 Hz for the GPU path; a persistent GPU mode with a boot-loop guard;
then direct-DRM + GPU (needs scanout-capable buffers imported into kbase).

## GPU desktop is now the default: direct to the panel at 120 Hz

- `vegas-gpu on|off|status` (user flag ~/.config/vegas/gpu-desktop). native-init
  loads the stock Mali modules via gpu-prepare when it is set. Boot-loop
  guard: /var/lib/vegas/gpu-boot-pending counts GPU boots that have not yet
  reached 3 min uptime; power-transition clears it on deliberate
  reboot/poweroff; two unconfirmed boots rename the flag to
  gpu-desktop.disabled-unstable and boot software. Verified the strike clears.
- With the GPU ready, vegas-desktop execs vegas-desktop-drm: Hyprland owns KMS,
  HYPRLAND_EGL_SURFACELESS=1 with the patched kbase Mesa, rendering straight
  into GBM scanout BOs on card0 (DSI-1 1080x2388@120, DP-1 disabled). Create
  ~/.config/vegas/nested to use the Weston + nested GPU path instead.
- `bringup/gpu-scanout-probe.c`: kbase GBM BO on card0 -> DMA-BUF -> EGLImage
  render target -> glFinish -> CPU check through the BO map: 300/300 frames
  pixel-exact. No PAN_MESA_DEBUG=batchsync needed in practice (no tearing seen).
- All 26 hardlinked *_dri.so aliases in /opt/vegas-gpu/mesa-kbase/lib/dri must
  be the patched build: GBM loads mediatek_dri.so, and the unpatched alias
  produced 6 faults at startup before they were all replaced.
- Two `ioctl(KBASE_IOCTL_MEM_FREE): Operation not permitted` lines appear at
  each direct-DRM GPU startup; no faults follow. Not yet investigated.

| Path | tap -> PTY | Foot commit -> frame callback |
| --- | ---: | ---: |
| llvmpipe, Weston nested (start of day) | 2.5 ms | 114 ms |
| GPU, Weston nested 30 Hz | 7 ms | 30 ms |
| GPU, Weston nested 60 Hz | 4 ms | 36–39 ms (Weston/readback bound) |
| **GPU, direct KMS 120 Hz (default)** | **7 ms** | **13–16 ms** |

Hyprland uses ~9% CPU on the GPU path (vs ~380% peak on llvmpipe).

## Wi-Fi (2026-10-07)

MT6835 + MT6631 A-die via MediaTek's WMT stack, no Android userspace:
stock vendor_dlkm modules (ORDER in /opt/vegas-wifi/ORDER: ccci/mddp deps,
btif_drv, connadp, wmt_drv; then after the loader handshake wmt_chrdev_wifi,
wlan_drv_gen4m_6835, connfem) + Muzuwi/wmt-pyloader's loader/launcher
(third_party/wmt-pyloader; matches Motorola's wmt_loader disassembly). No
firmware files are requested on this SoC. iwd cannot work (kernel lacks
CRYPTO_USER_API_*), so wpa_supplicant (ctrl socket group omarchy) + dhcpcd.
native-init starts /usr/local/libexec/vegas/wifi-up in the background;
`vegas-wifi scan|connect|status|forget`; ~/.config/vegas/wifi-off disables.
Connected to the home 2.4 GHz network (key copied from the laptop's
NetworkManager over USB without printing it); DNS/HTTPS verified.

Grok Bot: built from omarchy-pkgs/pkgbuilds/grok-bot for aarch64 on the laptop
(makepkg CARCH=aarch64; upstream arm64 .deb sha256 matched the PKGBUILD) and
installed with pacman -U. Needs --no-sandbox (~/.config/grok-bot-flags.conf)
because CONFIG_USER_NS is off. Renders in software for now (GPU process does
not use the kbase driver).

## EGL_ANDROID_native_fence_sync on kbase JM (Chrome/Chromium GPU)

Chrome's GPU process loaded the kbase driver but crash-looped on
eglDupNativeFenceFDANDROID: kbase fence_get_fd was a stub (-1), fence import
returned NULL and fence_server_sync called a DRM-only path.
port/vegas/patches/mesa-kbase-native-fence-fd.patch:
- Export: gather the syncobj's outstanding atoms, merge pairwise with
  BASE_JD_REQ_DEP atoms (two pre-deps per atom), then submit a
  BASE_JD_REQ_SOFT_FENCE_TRIGGER atom; the stock module
  (kbase_sync_fence_out_create) writes a sync_file fd into the base_fence at
  jc and signals it when the trigger runs. Atom-ID wrap: if the merge tree
  would cross 255, drain first and drop the (complete) dependencies.
- Import: keep a dup of the sync_file; fence_finish polls it; fence_server_sync
  waits on the CPU (no SOFT_FENCE_WAIT plumbing into JM submits yet).
bringup/gpu-fence-probe.c: 600/600 iterations (dup fd, signal ~1.1 ms,
re-import + eglWaitSyncKHR + eglClientWaitSyncKHR) across atom-ID wraps.
Installed as the system driver (sha256 def2b82f...; previous build kept in
lib/dri-jmdefer-backup); Hyprland latency unchanged (8 ms / 17 ms), 0 faults.

Chrome 155 then runs fully on the Mali: chrome://gpu shows compositing,
rasterization, canvas, WebGL and WebGPU hardware accelerated; renderer
"ANGLE (Panfrost, Mali-G57 (Panfrost), OpenGL ES 3.1 Mesa 23.0.0-devel)".
scripts/bench (CDP over stdlib websocket): scroll fps on Wikipedia, 3 runs:
software 71, GPU-composite-only 72, all-GPU 65 (rAF/main-thread bound);
2D canvas 2000 arcs/frame: software 27, all-GPU 9, composite-only 24.
So Chrome stays on software by default; ~/.config/vegas/chrome-gpu opts into
ANGLE/GLES compositing with CPU raster (google-chrome-stable launcher).
Canvas slowness on GPU raster looks like BO allocation churn (clear_page,
dcache clean, TLB flushes in the GPU process profile), not fence waits.
