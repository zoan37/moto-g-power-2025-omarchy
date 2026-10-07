-- Software rendering on the native phone display.
hl.config({
  decoration = { blur = { enabled = false }, shadow = { enabled = false } },
  -- llvmpipe: colour-management shaders and FP16 buffers dominate frame time.
  render = { direct_scanout = false, cm_enabled = false, use_fp16 = 0 },
  cursor = { no_hardware_cursors = true, use_cpu_buffer = true,
             hide_on_key_press = false, hide_on_touch = false, inactive_timeout = 0 },
  animations = { enabled = false },
  xwayland = { enabled = false },
  debug = { damage_tracking = 2, vfr = false, enable_stdout_logs = true },
})
hl.env("NO_AT_BRIDGE", "1")
hl.env("QT_ACCESSIBILITY", "0")
hl.env("GTK_A11Y", "none")
hl.env("LANG", "C.UTF-8")
hl.env("LC_ALL", "C.UTF-8")
hl.env("XCURSOR_THEME", "Adwaita")
hl.env("XCURSOR_SIZE", "36")
hl.env("HYPRCURSOR_SIZE", "36")
