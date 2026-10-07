-- Native Hyprland is nested in the phone's DRM Weston surface. Set its
-- portrait dimensions explicitly, using the bundle's usual scale variables.
local output_width = tonumber(os.getenv("OMARCHY_OUTPUT_WIDTH"))
local output_height = tonumber(os.getenv("OMARCHY_OUTPUT_HEIGHT"))
local refresh_mhz = tonumber(os.getenv("OMARCHY_REFRESH_MHZ"))
-- Keep Omarchy's conventional variable names: its Display panel updates
-- these declarations in place, which lets scale changes survive a restart.
-- Single source of truth for the UI scale: the Display panel and vegas-scale
-- rewrite this line, and the launchers read it for the keyboard and bar.
local omarchy_monitor_scale = 2.4
local omarchy_gdk_scale = math.floor(omarchy_monitor_scale + 0.5)
local output_mode = "preferred"

hl.env("GDK_SCALE", tostring(omarchy_gdk_scale))

if output_width and output_height and refresh_mhz then
  output_mode = string.format("%dx%d@%.3f", output_width, output_height, refresh_mhz / 1000)
end

if os.getenv("VEGAS_DIRECT_DRM") == "1" then
  -- Direct KMS: the MediaTek driver also reports a phantom DP-1. Driving it
  -- (the catch-all rule did) starves the DSI pipeline: RDMA0 underflows and
  -- the panel shows vertical stripes. Keep it off and name the panel.
  hl.monitor({ output = "DP-1", disabled = true })
  hl.monitor({ output = "DSI-1", mode = output_mode, position = "0x0", scale = omarchy_monitor_scale, transform = 0 })
else
  hl.monitor({ output = "", mode = output_mode, position = "auto", scale = omarchy_monitor_scale, transform = 0 })
end
