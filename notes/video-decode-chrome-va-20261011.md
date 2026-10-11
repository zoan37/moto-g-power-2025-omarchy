# Hardware video decode in Google Chrome via VA-API (2026-10-11)

Result: Google Chrome 155 (official arm64, VA-API only on Linux) decodes **H.264 (Constrained
Baseline/Main/High) and VP9 profile 0** on the phone's stateful MediaTek decoder through a new VA-API
driver, `mtk_stateful`. Chrome picks `VaapiVideoDecoder`, the picture is correct, and 1080p30
playback costs Chrome **0.6 core instead of 1.4 (H.264) / 2.0 (VP9)**. FFmpeg's VA-API output is
**bit-exact** with software decode on every test clip, including B-pyramid, custom scaling matrices,
multi-slice and weighted prediction.

Background: `notes/video-decode-research-20261011.md`. The decoder is `mtk-vcodec-dec-v1` (`/dev/video0`),
stateful, with CAPTURE in `M21S` (MM21 layout) only.

## Driver: `port/vegas/va-stateful/`

- **Source.** Built from sofus13/libva-v4l2_request 1.3 plus the Chromebook's MM21 patch
  (lenovo-slim3-chromebook-omarchy). It kept:
  - VA object handles, buffers, images, the NV12 dma-heap surfaces and `vaExportSurfaceHandle`;
  - the NEON MM21→NV12 detiler, now one helper thread per band for both planes.

  The request-API engine and the GL detile path are gone, so it ships as its own directory, not a
  patch. It is GPL-3.0+ (`COPYING`, `THIRD_PARTY.md`).
- **Decode flow.** Each VA picture becomes **one OUTPUT buffer** whose timestamp carries a sequence
  number.
  - OUTPUT buffers: 8 dma-bufs from `/dev/dma_heap/mtk_mm`. The node is `0644 root`, and a
    read-only open is enough to allocate, so unprivileged processes can use it.
  - CAPTURE is set up on `SOURCE_CHANGE`: `MIN_BUFFERS_FOR_CAPTURE + 4` dma-bufs from `mtk_mm`.
  - A dequeued frame is matched to its surface by timestamp, detiled into that surface's NV12 dma-buf
    (`/dev/dma_heap/system`, clamped to the surface size: 1088 decoded vs 1080 exported) and requeued
    at once. The firmware keeps its own DPB.
  - `vaSyncSurface` services the queues until the surface's frame arrives. It times out after 1 s,
    and three timeouts in a row return `DECODING_ERROR`. There is no worker thread.
- **H.264.** SPS and PPS are synthesized and resent only when their bytes change:
  - SPS id 0, level 5.1, with a VUI whose `bitstream_restriction` sets `max_num_reorder_frames=0` and
    `max_dec_frame_buffering=num_ref_frames`;
  - PPS id parsed from the slice header;
  - explicit scaling lists in the PPS (the VA IQ matrix is in **raster** order, as `va.h` says;
    bit-exact with x264 `cqm=jvt`);
  - `num_ref_idx_lX_default_active_minus1` learned from slices that don't override it (the slice
    header is parsed up to the override flag).

  Slices are appended with start codes. Chrome's and FFmpeg's slice data starts at the NAL header
  (0x65, `slice_data_bit_offset` 36).

  Rejected streams: interlaced, POC type 1, non-4:2:0 and >8-bit.
- **VP9.** The slice data is the whole frame and is fed unchanged. Frames with `show_frame=0` aren't
  waited for.
- **Build and test.**
  - `scripts/build-vegas-va.sh` pushes the source to `/opt/vegas-video/src/va-stateful`, builds it
    with meson on the phone and installs `/opt/vegas-video/va/mtk_stateful_drv_video.so`. Then
    `vainfo` lists the 4 profiles.
  - `scripts/test-vegas-va.sh CLIP…` compares per-frame MD5s with software decode and times the
    hardware decode.
  - Environment: `LIBVA_DRIVERS_PATH=/opt/vegas-video/va LIBVA_DRIVER_NAME=mtk_stateful`.
    `MTK_STATEFUL_TRACE=1` prints a per-picture trace.

## FFmpeg results (`-hwaccel vaapi`, frames downloaded as NV12)

| clip (1080p unless noted) | bit-exact | HW fps / cores | SW fps / cores |
|---|---|---|---|
| x264 defaults, High, B-pyramid (`high.mp4`) | 240/240 | 138 / 1.1 | 208 / 5.6 |
| bench `h264.mp4` | 600/600 | 151 / 1.0 | 236 / 6.2 |
| VP9 `-auto-alt-ref` (`vp9arf.webm`) | 120/120 | 104 / 0.8 | 63 / 2.4 |
| bench `vp9.webm` (5 hidden frames) | 300/300 | 119 / 0.8 | 170 / 4.1 |

Also bit-exact:

- IDR-only, Main and Baseline 1080p (240/240 each);
- `cqm=jvt` with 4 slices;
- 720p with `weightp=2`, 5 B-frames and 6 refs;
- VP9 854x480;
- an H.264 stream switching 720p→1080p→720p (300/300). FFmpeg, like Chrome, recreates the VA context
  on a size change.

The CPU figure includes FFmpeg's demux, parse and download. Detiling takes 2.0 ms per 1080p frame
with 4 threads and 3.4 ms with 1. `mtk_mm` is cached; `mtk_mm-uncached` takes 12.8 ms, and `system`
takes the same as `mtk_mm`.

## Chrome results

Separate test instance (`scripts/bench/chrome-va.sh`: profile `/tmp/chrome-va`, the normal launcher's
libmali GPU environment, `--enable-features=VaapiVideoDecoder,VaapiIgnoreDriverChecks,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL --disable-features=UseChromeOSDirectVideoDecoder`).
It played the looping 1080p30 pages `bench-video/{h264,vp9}.html`. CPU was measured over 15 s
(`scripts/bench/chrome-cpu.sh`); the decoder and quality came from DevTools (`scripts/bench/mediawatch.py`).
The software baseline used `--disable-accelerated-video-decode`.

| | decoder | GPU proc | renderer | Chrome total | dropped |
|---|---|---|---|---|---|
| H.264 HW | VaapiVideoDecoder | 0.37 | 0.08 | **0.64** | 0 / 554 |
| H.264 SW | FFmpegVideoDecoder | 0.18 | 1.08 | 1.37 | 0 / 520 |
| VP9 HW | VaapiVideoDecoder | 0.36 | 0.08 | **0.58** | 2 / 520 |
| VP9 SW | VpxVideoDecoder | 0.16 | 1.72 | 1.97 | 3 / 513 |

- **Picture.** Screenshots show correct colors and motion. Detiling in Chrome takes 3.2–3.5 ms per
  frame (lower clocks at 30 fps). That is most of the GPU process's extra ~0.2 core.
- **GBM / NV12 import: no shim needed on either GPU stack.**
  - ARM libmali (the user's launcher setting): the GPU process doesn't even map a libgbm.
  - Mesa kbase (`LD_LIBRARY_PATH=/opt/vegas-gpu/mesa-kbase/lib`): uses its own `libgbm.so.1.0.0`.

  On both, the exported NV12 surfaces import and play, with 2–5 drops in the first 9 s. The
  Chromebook's minigbm constant mismatch doesn't arise here.
- **Seeks.** 12 random seeks per page, and playback resumed at ~30 fps after each. A seek takes up to
  1.6 s for H.264, because x264's 250-frame GOP leaves up to ~220 frames to decode, and up to 0.9 s
  for VP9. There were no sync timeouts.
- **Device access.** The GPU process opens `/dev/video0`, `/dev/dma_heap/{mtk_mm,system}` and
  `/dev/dri/card0`. Its `Seccomp` is 0, the same as the user's running Chrome, so the GPU sandbox
  doesn't restrict this.
- **No render node needed for Chrome.** It passes `--render-node-override=/dev/dri/card0` and libva
  works on `card0`. Only `vainfo` and FFmpeg were run on a `renderD128` node, made with
  `mknod c 226 0` (runtime only, gone after a reboot).

## Decoder behavior learned

- **`V4L2_CID_VDEC_NO_REORDER` returns EINVAL.** Its VCP `set_param` has no case for it. It isn't
  needed: with the synthesized VUI, frames come back in decode order, 5–20 ms after submission, and
  the original timestamps are kept (timestamp detection is off by default).
- **One picture per buffer.** Every H.264 access unit and every shown VP9 frame produces exactly one
  frame. **Hidden VP9 frames produce none.** A `show_existing_frame` of a hidden frame would
  therefore show stale content. libvpx's default encodes don't do that.
- **Errors.**
  - A non-fatal error returns the OUTPUT buffer with `V4L2_BUF_FLAG_ERROR`, but a concealed frame
    usually still follows. With noise in an H.264 stream there were 13 errors and all 240 frames came
    out, with no stalls.
  - A fatal error ("vcodec not support the source", seen with a corrupted VP9 header) raises
    `V4L2_EVENT_MTK_VDEC_ERROR`. The driver then fails the context, and FFmpeg aborted after 1.4 s
    with no hang.
  - The next instance decodes normally.

## Not done / limits

- **Not wired into the user's Chrome yet.** Doing so needs the launcher to export
  `LIBVA_DRIVERS_PATH=/opt/vegas-video/va LIBVA_DRIVER_NAME=mtk_stateful` and pass the two feature
  flags above. Chrome keeps only the last `--enable-features`, so they have to be merged with
  `chrome-flags.conf` as on the Chromebook.
- **Missing codecs.** HEVC (the firmware has `H265`) isn't implemented: VPS/SPS/PPS synthesis would be
  needed. 10-bit isn't supported (the firmware only outputs 8-bit `M21S`). There's no AV1 or VP8 in
  hardware. YouTube prefers AV1, so H.264/VP9 must be forced (e.g. h264ify) to use this.
- **Untested paths.**
  - A resolution change *inside* one context: CAPTURE is drained up to the `LAST` buffer and rebuilt.
    FFmpeg and Chrome recreate the context instead, so this path didn't run.
  - The other agent's 2-plane `MM21` module: the driver also accepts a 2-plane `MM21` CAPTURE format,
    but no such module was tested.
