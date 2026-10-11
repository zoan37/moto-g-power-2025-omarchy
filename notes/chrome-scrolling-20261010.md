# Chrome touch scrolling (2026-10-10)

The user reported X scrolling as "laggy, jumpy, ghostly" compared with Android/iOS. Panel: 1080×2388,
120 Hz, UI scale 3. Desktop: Hyprland on direct DRM with native touch, rendering on the Mali (Mesa
panfrost-kbase).

## Method

Real-path, repeatable and account-free:

- Fresh empty Chrome profile in `/tmp`, never the user's profile or accounts. The page is a synthetic
  feed (400 cards with avatars, text and 1200×800 JPEGs, sticky blurred header) served from
  `file://`.
- Input: a 0.4 s, 1200 px upward swipe written into the real touchscreen evdev node (`ILITEK_TDDI`),
  so it passes through the kernel, libinput, Hyprland and Chrome's touch-scroll plus fling. One swipe
  scrolls exactly 400 CSS px at scale 3. `Input.synthesizeScrollGesture` does **not** scroll this
  desktop Chrome, so don't use it.
- Measurement: a DevTools trace (categories `benchmark,viz,cc,disabled-by-default-devtools.timeline.frame`)
  over the swipe and fling. Frame times come from the ends of the renderer's
  `SubmitCompositorFrameToPresentationCompositorFrame` spans (deduplicated). Frame verdicts come from
  `PipelineReporter.args.frame_reporter.state`.
- DevTools is reached from the laptop via `socat` on 192.168.77.1:9223. Recent Chrome refuses remote
  debugging on the default profile, which is another reason to use a separate profile.

## Results (mean of 3 swipes each)

| Setup | fps | median gap | late (>1.5 vsync) |
|---|---:|---:|---:|
| Software GL (old default) | 29 | 33 ms | 99–100% |
| Mesa kbase compositing, CPU raster | 41–44 | 22–25 ms | 78–82% |
| Mesa kbase compositing + GPU raster | 41 | 20 ms | 81% |
| ARM libmali r48 compositing | 51 | 16.5 ms | 69% |
| ARM libmali, fullscreen | 52 | 16.5 ms | 65% |
| **Hyprland vfr on**, Mesa kbase compositing | **66** | 16.5 ms | 60–63% |
| **Hyprland vfr on**, ARM libmali compositing | **78–80** | **8.3 ms** | 43% |

Hiding the cursor and `render.direct_scanout = 1` made no measurable difference. Typing latency
through the real Ilitek → wvkbd → foot path (`scripts/measure-vegas-input.py`) is the same with vfr
on or off on the GPU: tap → PTY median 6.4 ms both, commit → frame callback 17 vs 15 ms. The old
~100 ms vfr penalty (notes/direct-drm-latency-20261007.md) was specific to llvmpipe.

## X itself, and the refresh rate

On X (an alt account the user offered for scripted swipes; temporary profile copy, deleted
afterwards), libmali + vfr, 5 swipes each. Late = gap > 1.5 refresh periods:

| Panel rate | fps | median gap | late |
|---|---:|---:|---:|
| 120 Hz | 70 | 13 ms | 52% |
| 90 Hz | 61 | 11 ms | 38% |
| **60 Hz** | 54 | 16.7 ms | **10%** |

X's main thread is cheap (`ProxyMain::BeginMainFrame` ≈ 4 ms). Chrome's GPU draw
(`StartDrawToSwapStart`) is ≈ 11–12 ms, more than the 8.3 ms budget at 120 Hz, so frames alternate
between one and two refreshes. That is the judder. At 60 Hz nearly every frame is on time, and the
user found it "super smooth". **60 Hz is the direct-DRM default now** (`vegas-refresh 60|90|120`);
`vegas-desktop-drm` used to hard-code 120 Hz and ignore the setting.

## Where the time went

With Mesa kbase compositing, the page itself was cheap (`ProxyMain::BeginMainFrame` ≈ 5 ms). The
cost was in Chrome's GPU process: `Graphics.Pipeline.DrawAndSwap` ≈ 47 ms, of which
`StartDrawToSwapStart` ≈ 14 ms and `SwapStartToPresentation` ≈ 27 ms. ARM's driver cut the draw
to ≈ 9 ms. The remaining ~21 ms from swap to presentation was Hyprland: with `debug.vfr = false` it
redrew the whole screen on every vblank and competed with Chrome for the GPU. Chrome already shares
buffers with Hyprland as dmabufs (GBM on card0), so there was no CPU readback.

## Changes

- `looknfeel.lua`: `debug.vfr` follows `OMARCHY_GPU_MODE == "mali"` (stays off on llvmpipe);
  `cursor.hide_on_touch` follows `VEGAS_NATIVE_TOUCH`.
- Chrome launcher: GPU compositing (ANGLE/GLES, CPU raster) is the default on GPU desktop boots.
  `~/.config/vegas/chrome-software` opts out. `~/.config/vegas/chrome-libmali` uses the ARM r48
  compatibility trial (`/opt/vegas-gpu/libmali-trials/reproducible-v3`, JM stride-72 adapter
  preloaded) instead of Mesa.
- `native-init`: `/dev/dma_heap/system` gets `root:video 0660` at boot (libmali's GBM allocates
  from it).

## Next ideas

- Get Chrome's GPU draw under 8.3 ms for a steady 120 Hz: check why Hyprland's direct scanout
  did not engage for a fullscreen client, and try Hyprland on libmali too.

- Remaining late frames (~40%) with libmali: profile the GPU process and Hyprland again (the swap →
  presentation share), and try Hyprland itself on libmali (not yet tested as a compositor driver).
- Long-session stability of the libmali Chrome path is untested.
