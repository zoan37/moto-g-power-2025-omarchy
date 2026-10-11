# Hardware video decode: research (2026-10-11)

Desk research only: no phone access, nothing was run on the device. The question is whether the stock
vendor decoder (`mtk-vcodec-dec-v1.ko`) can serve web video in Chrome or Chromium on Hyprland.

Source tree for the citations below: `third_party/kernel/kernel-mtk` (abbreviated `K/`). It builds the shipped
module. Both list the same `of:` aliases (`mediatek,mt6835-vcodec-dec` included), and the module's vermagic is
`5.15.180-android13-8`. `D/` = `artifacts/vegas-power-audit/stock-0.dts` (stock DTB, decompiled).
`V/` = files read out of `artifacts/vegas-linux-bringup/super/vendor_b.img` with `third_party/erofs-utils/dump/dump.erofs`.

## 1. What the decoder is

- **A stateful V4L2 M2M decoder, not stateless.** It registers a `VFL_DIR_M2M` device with
  `V4L2_CAP_VIDEO_M2M_MPLANE` named `mtk-vcodec-dec` (`K/drivers/media/platform/mtk-vcodec/mtk_vcodec_dec_drv.c:566-601`).
  It takes Annex-B bitstream on the OUTPUT queue and gives frames on CAPTURE. It implements `V4L2_EVENT_SOURCE_CHANGE`
  and `V4L2_EVENT_EOS` (`mtk_vcodec_dec.c:2671-2686`) and `V4L2_DEC_CMD_STOP/START` (`mtk_vcodec_dec.c:1957-2003`).
  It does not use the request API or the stateless codec controls. The parsing runs in firmware on the VCP (section 2).
- **Codecs and formats are not in the driver.** Supported formats and frame sizes come from the firmware at the first
  open (`GET_PARAM_VDEC_CAP_SUPPORTED_FORMATS` / `GET_PARAM_VDEC_CAP_FRAME_SIZES`, `mtk_vcodec_dec.c:141-210`,
  `vdec/vdec_vcp_if.c:1626-1690`). The real list therefore needs `VIDIOC_ENUM_FMT` on the device. Static evidence:
  - The DT `max-op-rate-table` (`D/:3825`) names MPG4, H263, H264, AVC1 (H.264 without start codes), HEVC, H265 and VP90.
  - Android's codec list (`V/etc/media_codecs_c2.xml`) has hardware `c2.mtk.{avc,hevc,vp9,h263,heif}.decoder`
    (plus `.secure` variants). There is **no AV1 and no VP8** in hardware: those are `c2.android.*` software decoders only.
  - **Maximum size is 2560x1440**: 1080p60 and 1440p30 performance points for AVC, HEVC and VP9; secure decode tops
    out at 1080p30.
- **CAPTURE formats.** The driver knows MTK block formats: `MM21` (defined as `V4L2_PIX_FMT_MT21`, fourcc 'MM21'),
  10-bit `MT2T/MT2R`, and compressed "UFO" variants `MT21C` ('MT21'), `M2CS`, `MTCT`, and others
  (`mtk-v4l2-vcodec.h:36-74`, `include/uapi/linux/videodev2.h:735`). It also knows AFBC/HyFBC variants
  (`mtk-v4l2-vcodec.h:78-94`). The Android HAL `V/lib64/libcodec2_mtk_vdec.so` references `NV12_BLK`, the UFO formats,
  and a separate "CC output ... NV12" colour-convert step. That suggests the decoder writes block or UFO formats, and
  NV12 comes from a separate converter. **Expect 8-bit MM21 for H.264 and VP9 profile 0; NV12 is unconfirmed.**
- **No usable hardware converter.** The vendor MDP (`mdp_drv_mt6835.ko`) is a CMDQ ioctl driver for Android's
  DpFramework, not a V4L2 mem2mem device. MM21-to-NV12 conversion has to happen on the CPU or the GPU, as on the MT8189
  Chromebook.

## 2. Firmware and prerequisites

- **Decoding runs on the VCP (a RISC-V "tinysys" core).** The other firmware interface, VCU (an Android `vpud`
  userspace daemon behind `/dev/vcu`), is not used here:
  - The DT marks the VCU off (`mediatek,vcu-off = <1>`, `D/:3784`).
  - No `mtk-vcu.ko` ships.
  - `vdec_vcp_probe()` sets VCP mode when VCU functions are absent (`vdec/vdec_vcp_if.c:1057-1058`).
  - Every decode is an IPI to the VCP (`vdec_vcp_ipi_send`, `vdec_vcp_if.c:110-160`).
  - No `vpud` or vcodec service exists in `V/etc/init`. `init.mt6835.rc:423-439` only chmods `/dev/video0`,
    `/dev/video1` and `/dev/vdec-fmt` and creates log directories.
- **The VCP image is loaded by LK, not by the kernel.** The `vcp.img` → `vcp_a/vcp_b` partitions hold it
  (`firmware/candidates/TMO-W1VES36H.10-12-1/flashfile.xml:35`; `vcp_a/vcp_b` in `backups/vegas-gpt-20261010/sdc.sfdisk`).
  LK contains `app_load_vcp(fdt, b_mode)`, `load vcp` and `me_vcp_reserved` (strings in `lk.img`).
  - LK copies the image into reserved DRAM and adds the reserved-memory nodes to the FDT. The static DTB has no
    `me_vcp_reserved` or `reserve-memory-vcp_share` node; the kernel looks them up at
    `K/drivers/misc/mediatek/vcp/rv/vcp_helper.c:1645-1656` and `1726-1770`.
  - The kernel only writes the DRAM base and releases reset through TF-A (`MTK_SIP_TINYSYS_VCP_CONTROL`,
    `vcp_helper.c:1134-1163`). It does this on demand, when the first feature user turns the VCP on
    (`vcp_enable_pm_clk`, `vcp_helper.c:842-885`).
  - So the requirements are to keep booting through stock LK and to keep LK's FDT.
- **No Android userspace is needed.** There is no daemon, HIDL/AIDL, SELinux or property dependency in the VCP path.
  `mtk_vdec_property` and `mtk_vdec_vcp_log` are optional module parameters (`mtk_vcodec_dec_drv.c:33-64`).
- **Firmware working buffers come from the `mtk_mm` dma-heap** (`mtk_vcodec_util.c:679`). The vendor `system_heap.ko`
  provides that heap. The secure heaps (`mtk_svp_*`, `trusted_mem`, TEE) matter only for secure (DRM) decode, which is
  controlled by `V4L2_CID_MPEG_MTK_SEC_DECODE`.
- **For `/dev/videoN` to appear**, these modules must be loaded:
  - `vcp.ko`, which needs `mtk_tinysys_ipi`, `vcp_status` (vendor_boot), `aee_aed`, `mrdump`, `mtk-mbox`,
    `device-apc-common`, `irq-dbg`, `clk-common` and `mtk-smi-dbg`.
  - `mtk-vcodec-common.ko`.
  - `mtk-vcodec-dec-v1.ko`, which needs `iommu_debug`, `mtk-smi`, `mtk-mmdvfs-v3`, `emi` and `mtk-icc-core`.
  - `clk-mt6835-vcodec.ko`, the VDEC power domain, and the `mtk_iommu` domain. Probe returns `-EPROBE_DEFER`
    without the IOMMU domain (`mtk_vcodec_dec_drv.c:630-634`).

  The node is registered before the VCP ever runs (`:601` vs `:668`). **A device node existing does not prove decode
  works.** The first real test is opening it: `vcp_register_feature(VDEC_FEATURE_ID)` (`:84-96`), then the format query
  over IPI, which blocks until `is_vcp_ready()` (`vdec_vcp_if.c:158`).

## 3. Deviations from a generic stateful client

- **CAPTURE QBUF reads `v4l2_buffer.reserved` as a "general buffer" dma-buf fd** (`mtk_vcodec_dec.c:2515-2556`).
  `0xFFFFFFFF` means none. Generic clients leave 0, so the driver tries `dma_buf_get(0)`. That fails harmlessly unless
  fd 0 happens to be a dma-buf (`:1056-1066`, `:3312-3322`).
- **MTK controls and events are optional.** `V4L2_CID_MPEG_MTK_*` (`mtk_vcodec_dec.c:4294-4404`) include
  `DECODE_MODE`, `SEC_DECODE` and `FIXED_MAX_FRAME_BUFFER`. `V4L2_CID_VDEC_NO_REORDER` (`mtk-v4l2-vcodec.h:235`) gives
  decode-order output, which helps a VA wrapper. `V4L2_EVENT_MTK_VDEC_ERROR` and `NOHEADER` are extra events.
  Nothing has to be set before streaming.
- **Buffers and IOMMU.** Both queues advertise `VB2_DMABUF | VB2_MMAP` (`mtk_vcodec_dec.c:4917,4954`). OUTPUT buffers
  map into the VCP's IOMMU domain (`vcp_get_io_device(VCP_IOMMU_VDEC_512MB1)`, `:4928-4935`) and CAPTURE into the
  decoder's own domain. The driver does cache maintenance only for DMABUF (`:3348`).
  - `bringup/vdec-test.c` (new, untracked) states that MMAP buffers don't give the VCP an address. It uses
    `/dev/dma_heap/mtk_mm` dma-bufs on both queues, as Android does.
  - **This is unverified in source** (MMAP buffers get an IOVA through `vb2_dma_contig_plane_dma_addr`, `:1727`), but
    it matters a lot: Chromium's stateful decoder always uses **MMAP for OUTPUT**
    (`media/gpu/v4l2/v4l2_stateful_video_decoder.cc:415`).
- **Pixel formats.** FFmpeg's `*_v4l2m2m` decoders only take formats in `libavcodec/v4l2_fmt.c`, which has NV12, YUV420
  and similar but **no MM21**. So FFmpeg, and anything built on it (Firefox, `mpv --hwdec=v4l2m2m`), fails if the
  firmware offers only MM21. GStreamer's stateful `v4l2h264dec` and `v4l2vp9dec` map MM21 to `NV12_16L32S`
  (`gst-plugins-good/sys/v4l2/gstv4l2object.c:235`), and `videoconvert` can detile it. **GStreamer is the best
  standalone test client.**

## 4. Paths to browser hardware decode

### a) A VA-API driver wrapping the stateful decoder

**Nothing usable exists.**
- `mxsrc/libva-v4l2` (the revived Bootlin driver) is stateless-only; stateful support is a roadmap item.
- `libva-v4l2_request` (sofus13, Kwiboo, and the 2026 xnux.eu rewrite) is stateless-only.

**Writing one is feasible but bespoke work:**
- **VP9 is the easy case.** Chrome's VA VP9 path submits the whole compressed frame (uncompressed header included) as
  slice data, so it can go to a `VP90` OUTPUT buffer unchanged. Hidden frames and `show_existing_frame` need care.
- **H.264 is harder.** VA slice buffers carry whole slice NAL units, but SPS and PPS are not passed through. They must
  be rebuilt from `VAPictureParameterBufferH264` and the IQ matrix; profile, level, VUI and crop must be guessed. The
  stateful decoder keeps its own DPB, so decode-order output (`NO_REORDER`) is needed for a one-to-one surface
  mapping.
- **MM21 must be detiled into the VA surface.** That costs ~1.7–4 ms per 1080p frame on the CPU (MT8189 numbers).

**Reusable from the Chromebook port** (`lenovo-slim3-chromebook-omarchy`, `notes/video-decode-20261009.md`):
- the NEON MM21 detiler;
- the NV12 backing from `/dev/dma_heap`;
- multiplanar QBUF fixes;
- the GBM import-constant shim. Google Chrome's `gbm_bo_import` uses Mesa's `GBM_BO_IMPORT_FD_MODIFIER` = 0x5504,
  while minigbm uses 0x5505.

It also lets the user stay on Google Chrome. The estimated effort is high, and HEVC is not worth doing (Chrome on
Linux doesn't use it for web video anyway).

### b) Chromium built with `use_v4l2_codec=true`

- **Arch Linux ARM's own `extra/chromium` already is one.** Its PKGBUILD (pkgver 153.0.8010.52) comments
  "disable vaapi, enable v4l2" and sets `use_vaapi=false use_v4l2_codec=true`. It installs with pacman on this rootfs.
- **Raspberry Pi OS Chromium** (RPi-Distro/chromium `pios/trixie`, 154.0.8037.92 rpt1, 2026-10-01) ships V4L2
  stateful decode **on Wayland** (labwc, `--ozone-platform=wayland`) for the Pi 4's bcm2835-codec. The H.264
  regressions from rpt1 were fixed in rpt2 in Sep 2026 (raspberrypi/trixie-feedback#101). Its Linux patches (in
  `debian/patches/rpi/`) are:
  - `enable-AcceleratedVideoDecodeLinux.patch`: turns on the `AcceleratedVideoDecoder` feature by default;
  - `v4l2-sandbox-allow-decoder-devices.patch`: the Linux GPU broker otherwise only allows ChromeOS `/dev/video-dec*`
    names, so `/dev/videoN` opens fail silently and decode falls back to software;
  - `v4l2-do-not-advertise-stateless-codecs.patch`: notes that "outside ChromeOS the sole decoder wired up is
    V4L2StatefulVideoDecoder";
  - stateful-instance limits and a POLLERR re-arm fix.

  The patches are generic Chromium patches, not Pi-specific, and could be added to an ALARM build.
- **Upstream Chromium on Linux already handles MM21.**
  - It scans `/dev/video*` (`media/gpu/v4l2/v4l2_device.cc:864-875`).
  - The stateful decoder offers the CAPTURE formats to `VideoDecoderPipeline`, which inserts an image processor.
    `LibYUVImageProcessorBackend` has `MM21→NV12` (`media/gpu/chromeos/libyuv_image_processor_backend.cc:63,441`),
    or there is GL detiling on ARM with the `PreferGLImageProcessor` feature (`image_processor_factory.cc:356-371`).
  - Decoded NV12 is imported as an EGL dma-buf when `supports_nv12_gl_native_pixmap`
    (`media/mojo/services/gpu_mojo_media_client_linux.cc:54-66`), with an AR24 copy as fallback.
  - With GL compositing, `AcceleratedVideoDecodeLinuxGL` must be enabled (`:95-103`).
- **Flags for unpatched ALARM Chromium:**
  `--enable-features=AcceleratedVideoDecoder,AcceleratedVideoDecodeLinuxGL` plus `--disable-gpu-sandbox` (or rebuild
  with the RPi sandbox patch).
- **Risks:**
  - the OUTPUT=MMAP question in section 3;
  - libmali's GBM and NV12 dma-buf import, which needed a shim on the Chromebook;
  - YouTube preferring AV1, which this chip lacks. Chromium then software-decodes AV1 unless the site falls back.

### c) Alternatives

- **Firefox.** Its V4L2-M2M support (Firefox 116, 2023) goes through FFmpeg's `h264_v4l2m2m`: H.264 only, and
  NV12/YUV420 CAPTURE is required. It won't work with MM21-only output without patching FFmpeg. Firefox 153's Vulkan
  Video path doesn't apply (Mali has no Vulkan video).
- **mpv.** Same FFmpeg limitation for `v4l2m2m`. A GStreamer player (`v4l2h264dec`) works for local files or
  yt-dlp, but not for autoplay inside X.
- **Cheap mitigation now.** Keep X autoplay off. In the browser, prefer H.264/VP9 over AV1 (h264ify-style) so a hardware
  path, once it exists, can take the stream.

## Recommendation

1. **Prove the decoder outside the browser.** Run `bringup/vdec-test.c` and `gst-launch-1.0 … v4l2h264dec !
   videoconvert`, then `VIDIOC_ENUM_FMT` on CAPTURE, to get the real codec and format list. Also test MMAP vs DMABUF
   on OUTPUT.
2. **Then try ALARM's `chromium` (b)** with the flags above. It is the shortest path: no driver code, the stateful
   decoder is the one Chromium supports on Linux, and MM21 is handled. If OUTPUT MMAP fails, fix it in the
   decoder module (rebuildable from `K/`) or with a small preload shim, not in Chromium.
3. **Keep Google Chrome** for everything else if needed. Write a stateful VA driver (a) only if Google Chrome itself
   must decode in hardware; VP9 first.

## Sources (web)

- ALARM chromium PKGBUILD: https://github.com/archlinuxarm/PKGBUILDs/blob/master/extra/chromium/PKGBUILD
- RPi Chromium patches: https://github.com/RPi-Distro/chromium/tree/pios/trixie/debian/patches/rpi
- Pi 4 V4L2 Chromium regression: https://github.com/raspberrypi/trixie-feedback/issues/101
- Chromium sources (main): `media/gpu/v4l2/`, `media/gpu/chromeos/`, `media/mojo/services/gpu_mojo_media_client_linux.cc`
- libva-v4l2: https://github.com/mxsrc/libva-v4l2 · v4l2_request rewrite: https://xnux.eu/log/102.html
- FFmpeg `libavcodec/v4l2_fmt.c`; GStreamer `sys/v4l2/gstv4l2object.c`
- Firefox V4L2-M2M (116): https://www.omglinux.com/firefox-hardware-acceleration-raspberry-pi/ ; Firefox 153 Vulkan
  Video: https://www.omgubuntu.co.uk/2026/07/firefox-vulkan-video-decoding-nvidia

## On-device results (2026-10-11)

- **Bring-up:** `insmod` vcp.ko → mtk-vcodec-common.ko → mtk-vcodec-dec-v1.ko → vdec-fmt.ko (from
  vendor_dlkm; all other dependencies are already loaded by the vendor_boot set). The driver logs
  `decoder registered as /dev/video0`. There's no devtmpfs, so create the node with `mknod` from
  `/sys/class/video4linux/video0/dev`. The VCP starts on first open.
- **Firmware capability table** (dumped with `mtk_v4l2_dbg_level=1` at insmod): one frame format,
  `M21S`, 1 plane; bitstream formats H264, H265, HEIF, MPG4, H263, VP90 (max 2560×1440). No AV1, no
  VP8.
- **`M21S` is MM21:** a decoded 1080p frame detiled as 16×32 luma / 16×16 chroma tiles (Y then
  CbCr, 1920×1088, 3,133,440 bytes) matches a software decode of the same frame.
- **Decoding works** with `bringup/vdec-test.c` (stateful flow, one access unit per OUTPUT buffer):
  1080p H.264 at 64 fps with dmabufs from the `mtk_mm` heap, 39 fps from `system`.
- **MMAP OUTPUT buffers** fail with `src_addr is NULL` unless `mtk_v4l2_dbg_level > 0`: the driver
  only takes `vb2_plane_vaddr()` in debug mode (mtk_vcodec_dec.c ~1727). Setting the level to 1 makes
  them work (47 fps). With plain MMAP and no debug level, the VCP rejects the IPI (`ack fail -5`).
- **Never `rmmod` / reload `mtk-vcodec-dec-v1`:** unloading and reloading it rebooted the phone
  (same lesson as the Chromebook's mtk-vcodec).

### Chromium (Arch Linux ARM `chromium` 151, built with `use_v4l2_codec`)

Flags: `--enable-features=AcceleratedVideoDecoder,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL --disable-gpu-sandbox`.
Chromium's Linux device scan opens `/dev/video0`…`/dev/video255` and keeps a decoder only if one
of its CAPTURE formats is a pixel format Chromium knows (`Fourcc::FromUint32`). That list includes
MM21 but not `M21S`, so the device was skipped.

- `bringup/v4l2-mm21-shim.c` (LD_PRELOAD) reports `M21S` as `MM21`. Gotcha: Chromium passes
  ioctl requests as a signed `int`, so they arrive sign-extended (0xFFFFFFFFC0405602). Compare only
  the low 32 bits, as the kernel does.
- With the shim, Chromium picks `V4L2VideoDecoder` and handles the resolution change (MM21
  1920×1088, 9 CAPTURE buffers), then fails in `V4L2FormatToVideoFrameLayout()`: for a single
  buffer it only derives the second plane for NV12/YUV420, not MM21. MM21 has to come with 2 planes.
- The firmware IPI already carries separate luma/chroma addresses when a frame has 2 planes
  (`vdec_vcp_if.c` ~1427, `c_fb_dma`). Next step: rebuild `mtk-vcodec-dec-v1` with M21S → MM21
  2-plane and the MMAP address fix (`port/vegas/patches/`, built on the SER8).
