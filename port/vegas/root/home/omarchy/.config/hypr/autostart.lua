-- Extra processes started with the nested session.
hl.on("hyprland.start", function()
  -- Do not start spice-vdagent. Its X11 clipboard path cannot connect to this
  -- native-Wayland session and can cause duplicate-agent churn under PRoot.
  -- Voxtype is optional until a model has been downloaded. Its small manager
  -- is a no-op without a model and replaces the unavailable systemd user unit.
  hl.exec_cmd("omarchy-voxtype-daemon start")
end)

hl.on("hyprland.start", function()
  -- On direct DRM, libinput already delivers touch; the bridge would grab it.
  if os.getenv("VEGAS_NATIVE_TOUCH") ~= "1" then
    hl.exec_cmd("flock -n $XDG_RUNTIME_DIR/vegas-touch.lock vegas-touch-pointer >>$HOME/.local/state/vegas/touch.log 2>&1")
  end
  hl.exec_cmd("flock -n $XDG_RUNTIME_DIR/vegas-keyboard.lock vegas-keyboard >>$HOME/.local/state/vegas/keyboard.log 2>&1")
  hl.exec_cmd("flock -n $XDG_RUNTIME_DIR/vegas-terminal.lock foot --title='Omarchy - Moto G Power 2025'")
end)
