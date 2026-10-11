import QtQuick
import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import qs.Commons

// Omarchy mobile: phone navigation for a one-app-per-workspace Hyprland.
//
// Model: every app lives full-screen on its own workspace (hyprland.lua's
// `workspace = "empty"` rule). "Home" is simply an empty workspace, and the
// home screen is drawn on the bottom layer there, so it only exists when no
// app covers it. The navigation bar at the bottom edge is always present:
//   tap   left = Back, centre = Home, right = keyboard
//   swipe left/right along it = next/previous app, swipe up = Home
//   long-press centre = Omarchy menu
//
// Settings are read from this plugin's entry in shell.json `plugins[]`:
//   { "id": "vegas.mobile", "navHeight": 44, "home": true, "pinned": [...] }
// Everything is event driven (Hyprland IPC events); nothing animates or polls.
Item {
  id: root

  // Injected by the shell's panel loader.
  property var shell: null
  property string omarchyPath: Quickshell.env("OMARCHY_PATH")
  property var manifest: null

  readonly property string pluginId: "vegas.mobile"
  readonly property string homeDir: Quickshell.env("HOME")

  // ------------------------------------------------------------ settings
  readonly property var settings: {
    var config = root.shell ? root.shell.shellConfig : null
    var list = config && Array.isArray(config.plugins) ? config.plugins : []
    for (var i = 0; i < list.length; i++)
      if (list[i] && list[i].id === root.pluginId) return list[i]
    return ({})
  }
  function setting(name, fallback) {
    var value = root.settings[name]
    return value === undefined || value === null ? fallback : value
  }
  function clamp(value, low, high, fallback) {
    var n = Number(value)
    return isFinite(n) ? Math.max(low, Math.min(high, Math.round(n))) : fallback
  }

  readonly property int navHeight: clamp(setting("navHeight", 44), 32, 96, 44)
  readonly property bool homeEnabled: setting("home", true) !== false
  readonly property bool debug: setting("debug", false) === true
  readonly property bool gesturesEnabled: setting("gestures", true) !== false
  readonly property string keyboardToggle: String(setting("keyboardToggle", "/usr/local/bin/vegas-toggle-keyboard"))
  readonly property string keyboardNamespace: String(setting("keyboardNamespace", "wvkbd"))

  readonly property var defaultPinned: [
    { "name": "Terminal", "glyph": "", "exec": "foot", "match": "^foot$" },
    { "name": "Grok", "icon": "~/.local/share/icons/hicolor/256x256/apps/grok.png",
      "exec": "/usr/local/bin/omarchy-launch-webapp https://grok.com", "match": "^chrome-grok\\.com" },
    { "name": "X", "icon": "~/.local/share/applications/icons/X.png",
      "exec": "/usr/local/bin/omarchy-launch-webapp https://x.com/", "match": "^chrome-x\\.com" },
    { "name": "Apps", "glyph": "", "action": "apps" },
    { "name": "Omarchy", "glyph": "", "font": "omarchy", "action": "menu" }
  ]
  readonly property var pinned: Array.isArray(settings.pinned) ? settings.pinned : defaultPinned

  // Back is a key sent to the focused app; the first rule whose `match`
  // (regex on the app id) fits wins. Browsers go back a page, terminals and
  // everything else get Escape.
  readonly property var defaultBack: [
    { "match": "^(chrome|google-chrome|chromium|brave|firefox)", "mods": "ALT", "key": "Left" },
    { "match": ".*", "mods": "", "key": "Escape" }
  ]
  readonly property var backRules: Array.isArray(settings.back) ? settings.back : defaultBack

  // ------------------------------------------------------------ state
  readonly property int focusedWorkspaceId: Hyprland.focusedWorkspace ? Hyprland.focusedWorkspace.id : -1

  // Open apps in workspace order (the order swipes walk through).
  readonly property var apps: {
    var values = Hyprland.toplevels ? Hyprland.toplevels.values : []
    var out = []
    for (var i = 0; i < values.length; i++) {
      var t = values[i]
      if (!t || !t.workspace || t.workspace.id <= 0) continue
      var appId = t.wayland && t.wayland.appId ? String(t.wayland.appId)
        : String((t.lastIpcObject && t.lastIpcObject["class"]) || "")
      var info = root.appInfo(appId, String(t.title || ""))
      out.push({
        toplevel: t,
        workspaceId: t.workspace.id,
        address: String(t.address || ""),
        appId: appId,
        title: String(t.title || ""),
        name: info.name,
        icon: info.icon,
        glyph: info.glyph,
        glyphFont: info.glyphFont
      })
    }
    out.sort(function(a, b) {
      return a.workspaceId !== b.workspaceId ? a.workspaceId - b.workspaceId
        : (a.address < b.address ? -1 : 1)
    })
    return out
  }

  readonly property int currentIndex: {
    for (var i = 0; i < root.apps.length; i++)
      if (root.apps[i].workspaceId === root.focusedWorkspaceId) return i
    return -1
  }
  readonly property bool onHome: currentIndex === -1
  readonly property var currentApp: currentIndex >= 0 ? apps[currentIndex] : null

  property bool keyboardVisible: false

  // The app you were in before going home: marked on the home screen, so
  // "close this app" is swipe up, then x on the marked row.
  property int lastAppWorkspaceId: -1
  onCurrentIndexChanged: if (currentIndex >= 0) lastAppWorkspaceId = apps[currentIndex].workspaceId

  // ------------------------------------------------------------ app lookup
  function expandPath(path) {
    var p = String(path || "")
    if (p.indexOf("~/") === 0) return root.homeDir + p.substring(1)
    if (p.indexOf("$HOME/") === 0) return root.homeDir + p.substring(5)
    return p
  }

  function iconSource(icon) {
    var value = root.expandPath(icon)
    if (value === "") return ""
    if (value.indexOf("file://") === 0 || value.indexOf("image://") === 0) return value
    if (value.charAt(0) === "/") return Util.fileUrl(value)
    if (root.shell && root.shell.appLibrary) return root.shell.appLibrary.iconSource(value)
    return Quickshell.iconPath(value, true)
  }

  function matches(pattern, value) {
    if (!pattern) return false
    try { return new RegExp(String(pattern), "i").test(String(value || "")) } catch (e) { return false }
  }

  function pinnedFor(appId) {
    for (var i = 0; i < root.pinned.length; i++) {
      var p = root.pinned[i]
      if (p && p.match && root.matches(p.match, appId)) return p
    }
    return null
  }

  function appInfo(appId, title) {
    var p = root.pinnedFor(appId)
    if (p) return { name: String(p.name || appId), icon: p.icon ? root.iconSource(p.icon) : "",
                    glyph: String(p.glyph || ""), glyphFont: String(p.font || "") }
    var entry = null
    try { entry = DesktopEntries.heuristicLookup(appId) } catch (e) { entry = null }
    if (entry) return { name: String(entry.name || appId), icon: entry.icon ? root.iconSource(entry.icon) : "",
                        glyph: "", glyphFont: "" }
    var name = appId.replace(/^chrome-/, "").replace(/__.*$/, "").replace(/_/g, "/")
    return { name: name || title || "App", icon: "", glyph: "", glyphFont: "" }
  }

  // ------------------------------------------------------------ actions
  function dispatch(lua, legacy) {
    Hyprland.dispatch(Hyprland.usingLua ? lua : legacy)
  }

  function luaString(value) {
    return '"' + String(value).replace(/\\/g, "\\\\").replace(/"/g, '\\"') + '"'
  }

  function focusWorkspace(id) {
    root.dispatch("hl.dsp.focus({ workspace = " + root.luaString(id) + " })", "workspace " + id)
  }

  // Back and Home first dismiss a shell overlay (the Omarchy menu, app
  // launcher) if one is open, like Android's back closing a dialog.
  readonly property var dismissable: ["vegas.quick", "omarchy.menu", "omarchy.clipboard", "omarchy.emojis"]
  function openOverlay() {
    if (!root.shell || typeof root.shell.isPluginOpen !== "function") return ""
    for (var i = 0; i < root.dismissable.length; i++) {
      var open = false
      try { open = root.shell.isPluginOpen(root.dismissable[i]) } catch (e) { open = false }
      if (open) return root.dismissable[i]
    }
    return ""
  }
  function dismissOverlay() {
    var id = root.openOverlay()
    if (id !== "") { root.shell.hide(id); return true }
    // A status-bar popup (battery, calendar, ...).
    var popout = root.shell && root.shell.bar ? root.shell.bar.activePopout : null
    if (popout && typeof popout.close === "function") { popout.close(); return true }
    return false
  }

  // Home has no text field, but wvkbd only hides on an input-method
  // deactivate, which Hyprland does not send when focus moves to an empty
  // workspace or a focused overlay (the menu's search) closes. Re-check
  // shortly after those events and put the keyboard away.
  onOnHomeChanged: keyboardCheck.restart()
  onKeyboardVisibleChanged: keyboardCheck.restart()
  Timer {
    id: keyboardCheck
    interval: 250
    onTriggered: if (root.onHome && root.keyboardVisible && root.openOverlay() === "") root.toggleKeyboard()
  }

  function goHome() {
    root.dismissOverlay()
    if (root.onHome) { keyboardCheck.restart(); return }
    root.focusWorkspace("empty")
  }

  function switchTo(app) {
    if (app) root.focusWorkspace(String(app.workspaceId))
  }

  // dir > 0: the app to the right (next workspace), dir < 0: to the left.
  // From home, right goes to the first app and left to the last.
  function neighbour(dir) {
    var n = root.apps.length
    if (n === 0) return null
    var i = root.currentIndex
    if (i < 0) return dir > 0 ? root.apps[0] : root.apps[n - 1]
    var j = i + (dir > 0 ? 1 : -1)
    return j >= 0 && j < n ? root.apps[j] : null
  }

  function step(dir) {
    var target = root.neighbour(dir)
    if (target) root.switchTo(target)
  }

  function closeApp(app) {
    if (!app) return
    var t = app.toplevel
    if (t && t.wayland && typeof t.wayland.close === "function") {
      t.wayland.close()
      return
    }
    var address = app.address.indexOf("0x") === 0 ? app.address : "0x" + app.address
    root.dispatch("hl.dsp.window.close({ window = " + root.luaString("address:" + address) + " })",
                  "closewindow address:" + address)
  }

  function closeCurrent() { root.closeApp(root.currentApp) }

  property var pendingKeyUp: null
  function sendKey(mods, key) {
    var m = root.luaString(mods || ""), k = root.luaString(key)
    root.dispatch("hl.dsp.send_key_state({ mods = " + m + ", key = " + k + ", state = \"down\" })",
                  "sendshortcut " + (mods || "") + ", " + key + ",")
    if (!Hyprland.usingLua) return
    // Release separately: Hyprland's one-shot send can leave keys repeating.
    root.pendingKeyUp = "hl.dsp.send_key_state({ mods = " + m + ", key = " + k + ", state = \"up\" })"
    keyUpTimer.restart()
  }
  Timer {
    id: keyUpTimer
    interval: 40
    onTriggered: if (root.pendingKeyUp) { Hyprland.dispatch(root.pendingKeyUp); root.pendingKeyUp = null }
  }

  function back() {
    if (root.dismissOverlay()) return
    var app = root.currentApp
    if (!app) return
    for (var i = 0; i < root.backRules.length; i++) {
      var rule = root.backRules[i]
      if (rule && rule.key && root.matches(rule.match || ".*", app.appId)) {
        root.sendKey(String(rule.mods || ""), String(rule.key))
        return
      }
    }
  }

  function toggleKeyboard() {
    Util.execDetached(root.keyboardToggle)
  }

  function openMenu(route) {
    if (root.shell && typeof root.shell.toggle === "function")
      root.shell.toggle("omarchy.menu", JSON.stringify({ menu: route || "root" }))
  }

  function launch(item, forceNew) {
    if (!item) return
    if (item.action === "menu") { root.openMenu("root"); return }
    if (item.action === "apps") { root.openMenu("apps"); return }
    if (item.action === "home") { root.goHome(); return }
    if (!forceNew && item.match) {
      for (var i = 0; i < root.apps.length; i++) {
        if (root.matches(item.match, root.apps[i].appId)) { root.switchTo(root.apps[i]); return }
      }
    }
    if (item.exec) Util.execDetached("setsid " + String(item.exec) + " >/dev/null 2>&1 &")
  }

  // ------------------------------------------------------------ keyboard state
  Connections {
    target: Hyprland
    function onRawEvent(event) {
      if (!event) return
      if (event.name === "openlayer" && event.data === root.keyboardNamespace) root.keyboardVisible = true
      else if (event.name === "closelayer" && event.data === root.keyboardNamespace) root.keyboardVisible = false
      else if (event.name === "closelayer") keyboardCheck.restart()
    }
  }

  // The keyboard may already be up when the shell (re)starts: ask once.
  Process {
    running: true
    command: ["bash", "-lc", "hyprctl layers -j"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        try {
          var monitors = JSON.parse(text)
          for (var m in monitors) {
            var levels = monitors[m].levels || {}
            for (var l in levels)
              for (var i = 0; i < levels[l].length; i++)
                if (levels[l][i].namespace === root.keyboardNamespace) root.keyboardVisible = true
          }
        } catch (e) {}
      }
    }
  }

  // ------------------------------------------------------------ shell IPC
  // omarchy-shell shell summon vegas.mobile '{"action":"home"}' (or back,
  // next, previous, close, keyboard) — handy for hardware keys and scripts.
  function open(payloadJson) {
    var payload = ({})
    try { payload = JSON.parse(payloadJson || "{}") } catch (e) { payload = ({}) }
    root.perform(String(payload.action || "home"))
  }
  function close() {}
  function perform(action) {
    if (action === "home") root.goHome()
    else if (action === "back") root.back()
    else if (action === "next") root.step(1)
    else if (action === "previous") root.step(-1)
    else if (action === "close") root.closeCurrent()
    else if (action === "keyboard") root.toggleKeyboard()
    else if (action === "menu") root.openMenu("root")
    else if (action === "state") return JSON.stringify({
      focusedWorkspace: root.focusedWorkspaceId, currentIndex: root.currentIndex, onHome: root.onHome,
      keyboardVisible: root.keyboardVisible,
      apps: root.apps.map(function(a) { return { workspace: a.workspaceId, appId: a.appId, name: a.name, title: a.title } })
    })
    else return "unknown"
    return "ok"
  }

  IpcHandler {
    target: "phone"
    function act(action: string): string { return root.perform(action) }
  }

  // ------------------------------------------------------------ surfaces
  NavBar {
    mobile: root
  }

  Home {
    mobile: root
  }
}
