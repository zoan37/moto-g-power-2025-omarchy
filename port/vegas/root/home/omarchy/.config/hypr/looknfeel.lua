-- Phone display. vegas-desktop-drm sets OMARCHY_GPU_MODE=mali when Hyprland
-- renders on the Mali, otherwise it runs on llvmpipe.
local gpu = os.getenv("OMARCHY_GPU_MODE") == "mali"
-- Native touch (direct DRM): no pointer arrow while using a finger.
local native_touch = os.getenv("VEGAS_NATIVE_TOUCH") == "1"
hl.config({
  decoration = { blur = { enabled = false }, shadow = { enabled = false } },
  -- llvmpipe: colour-management shaders and FP16 buffers dominate frame time.
  render = { direct_scanout = false, cm_enabled = false, use_fp16 = 0 },
  cursor = { no_hardware_cursors = true, use_cpu_buffer = true,
             hide_on_key_press = false, hide_on_touch = native_touch,
             -- touch phone: no arrow sitting on screen after boot
             inactive_timeout = native_touch and 1 or 0 },
  animations = { enabled = false },
  xwayland = { enabled = false },
  -- vfr: redraw only on damage. On the Mali it lifts Chrome touch scrolling
  -- from ~41 to ~66 fps (the continuous 120 Hz redraw competed for the GPU)
  -- with unchanged typing latency (tap -> PTY 6.4 ms either way). On llvmpipe
  -- it added ~100 ms to input handling, so it stays off there.
  debug = { damage_tracking = 2, vfr = gpu, enable_stdout_logs = true },
})
hl.env("NO_AT_BRIDGE", "1")
hl.env("QT_ACCESSIBILITY", "0")
hl.env("GTK_A11Y", "none")
hl.env("LANG", "C.UTF-8")
hl.env("LC_ALL", "C.UTF-8")
hl.env("XCURSOR_THEME", "Adwaita")
hl.env("XCURSOR_SIZE", "36")
hl.env("HYPRCURSOR_SIZE", "36")

-- Phone UI: apps fill the screen edge to edge, like Android/iOS. No gaps,
-- borders or rounding around tiled windows (one app per workspace, so there
-- is never a second window to tell apart); the status bar, navigation bar
-- and keyboard reserve their own space as layer-shell exclusive zones.
hl.config({
  general = { gaps_in = 0, gaps_out = 0, border_size = 0 },
  decoration = { rounding = 0 },
})
-- Omarchy makes windows 98.5%/96% opaque. On a phone that only blends the
-- wallpaper into every frame of every app; keep apps fully opaque.
hl.window_rule({ match = { tag = "default-opacity" }, opacity = "1 1" })
