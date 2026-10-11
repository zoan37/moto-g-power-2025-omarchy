import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import qs.Commons
import qs.Ui

// Quick settings: the phone's pull-down shade.
//
// Opened by a swipe down on the status bar (vegas.bar), or
// `omarchy-shell shell toggle vegas.quick` / `qs ipc call quick toggle`.
// Closed by tapping outside the card, swiping the card up, or the nav bar's
// Back/Home (vegas.mobile treats it as a dismissable overlay).
//
//   time, date                         battery, charging, temperature
//   [ Wi-Fi ]          [ Do Not Disturb ]
//   [ Keyboard ]       [ Screenshot ]
//   [ Omarchy ]        [ Power ]
//   brightness ───────●──────
//   volume     ─────●────────  Speaker
//   (Wi-Fi networks, when the Wi-Fi tile is tapped)
//   recent notifications
//
// The window respects the bars' and the keyboard's exclusive zones, so the
// status bar and nav bar stay visible and usable while it is open. Nothing
// polls or animates; state is read once per open and after each action.
Item {
  id: root

  // Injected by the shell's panel loader.
  property var shell: null
  property string omarchyPath: Quickshell.env("OMARCHY_PATH")
  property var manifest: null

  property bool opened: false
  readonly property string home: Quickshell.env("HOME")
  readonly property string historyDir: home + "/.local/state/omarchy/notifications/history"

  // ------------------------------------------------------------ state
  property string backlight: ""
  property int brightnessRaw: 0
  property int brightnessMax: 0
  property real volume: 0
  property bool muted: false
  property string sink: ""
  property int battery: -1
  property string batteryStatus: ""
  property real batteryTemp: 0
  property string wifiState: ""
  property var networks: []
  property var notifications: []

  property bool wifiExpanded: false
  property bool scanning: false
  property string joining: ""          // network awaiting a password
  property string wifiMessage: ""

  readonly property var currentNetwork: {
    for (var i = 0; i < networks.length; i++) if (networks[i].current) return networks[i]
    return null
  }
  readonly property bool wifiConnected: wifiState === "COMPLETED"
  readonly property bool wifiOff: wifiState === "DISCONNECTED" || wifiState === "INACTIVE" || wifiState === ""

  readonly property var notificationService: shell && typeof shell.serviceFor === "function"
    ? shell.serviceFor("omarchy.notifications") : null
  readonly property bool dnd: notificationService !== null && notificationService.doNotDisturb === true

  // ------------------------------------------------------------ lifecycle
  function open(payloadJson) {
    root.wifiMessage = ""
    root.joining = ""
    root.opened = true
    // Like Android, the shade puts the keyboard away (wvkbd: SIGUSR1 hides,
    // SIGUSR2 shows); the password prompt brings it back.
    root.run(["pkill", "-USR1", "-x", "wvkbd-mobintl"])
    root.refresh()
  }
  function close() {
    root.opened = false
    root.wifiExpanded = false
    root.joining = ""
  }
  function toggleOpen() { if (root.opened) root.hideSelf(); else root.showSelf() }
  // Through the shell, so its open-panel bookkeeping stays right.
  function showSelf() {
    if (root.shell && typeof root.shell.summon === "function") root.shell.summon("vegas.quick", "")
    else root.open("")
  }
  function hideSelf() {
    if (root.shell && typeof root.shell.hide === "function") root.shell.hide("vegas.quick")
    else root.close()
  }

  IpcHandler {
    target: "quick"
    function toggle(): void { root.toggleOpen() }
  }

  // ------------------------------------------------------------ reading state
  function refresh() {
    if (!stateProc.running) stateProc.running = true
  }

  Process {
    id: stateProc
    command: ["bash", "-c",
      "for b in /sys/class/backlight/*; do echo \"bright=$b $(cat $b/brightness) $(cat $b/max_brightness)\"; break; done\n" +
      "p=/sys/class/power_supply/battery; echo \"bat=$(cat $p/capacity) $(cat $p/temp) $(cat $p/status)\"\n" +
      "echo \"vol=$(wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null)\"\n" +
      "echo \"sink=$(wpctl inspect @DEFAULT_AUDIO_SINK@ 2>/dev/null | sed -n 's/.*node.description = \"\\(.*\\)\"/\\1/p' | head -1)\"\n" +
      "echo \"wifi=$(wpa_cli -i wlan0 status 2>/dev/null | sed -n 's/^wpa_state=//p')\"\n" +
      "vegas-wifi list 2>/dev/null | sed 's/^/net=/'\n" +
      "ls -1t \"$1\"/*.json 2>/dev/null | head -5 | while read -r f; do printf 'note=%s\\n' \"$(head -1 \"$f\")\"; done",
      "--", root.historyDir]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: root.parseState(text)
    }
  }

  function parseNetwork(line, out) {
    var f = line.split("\t")
    if (f.length < 5 || f[4] === "") return
    out.push({ current: f[0] === "1", saved: f[1] === "1", dbm: Number(f[2]), secured: f[3] === "1",
               ssid: f.slice(4).join("\t") })
  }

  function parseState(text) {
    var nets = [], notes = []
    var lines = String(text || "").split("\n")
    for (var i = 0; i < lines.length; i++) {
      var line = lines[i], eq = line.indexOf("=")
      if (eq < 0) continue
      var key = line.substring(0, eq), value = line.substring(eq + 1)
      if (key === "bright") {
        var b = value.split(" ")
        root.backlight = b[0]
        root.brightnessRaw = Number(b[1]) || 0
        root.brightnessMax = Number(b[2]) || 0
      } else if (key === "bat") {
        var t = value.split(" ")
        root.battery = Number(t[0])
        root.batteryTemp = Number(t[1]) / 10
        root.batteryStatus = t.slice(2).join(" ")
      } else if (key === "vol") {
        var m = value.match(/([0-9.]+)/)
        if (!brightVolDrag.volume) root.volume = m ? Number(m[1]) : 0
        root.muted = value.indexOf("MUTED") !== -1
      } else if (key === "sink") {
        root.sink = value.replace(/^Built-in Audio /, "")
      } else if (key === "wifi") {
        root.wifiState = value
      } else if (key === "net") {
        root.parseNetwork(value, nets)
      } else if (key === "note") {
        try { notes.push(JSON.parse(value)) } catch (e) {}
      }
    }
    root.networks = nets
    root.notifications = notes
  }

  // ------------------------------------------------------------ actions
  function run(command) {
    Util.execArgv(command)
  }

  // Brightness and volume writes are throttled to one in flight while dragging.
  QtObject {
    id: brightVolDrag
    property bool volume: false
    property int pendingBrightness: -1
    property real pendingVolume: -1
  }
  function setBrightness(fraction) {
    if (root.brightnessMax <= 0) return
    // Never fully dark: 1% of the range keeps the panel visible.
    var raw = Math.max(Math.round(root.brightnessMax * 0.01), Math.round(fraction * root.brightnessMax))
    root.brightnessRaw = raw
    brightVolDrag.pendingBrightness = raw
    if (!brightProc.running) root.flushBrightness()
  }
  function flushBrightness() {
    if (brightVolDrag.pendingBrightness < 0 || root.backlight === "") return
    brightProc.command = ["sh", "-c", "echo \"$1\" > \"$2/brightness\"", "--",
                          String(brightVolDrag.pendingBrightness), root.backlight]
    brightVolDrag.pendingBrightness = -1
    brightProc.running = true
  }
  Process {
    id: brightProc
    onExited: root.flushBrightness()
  }

  function setVolume(value) {
    root.volume = value
    brightVolDrag.pendingVolume = value
    if (!volumeProc.running) root.flushVolume()
  }
  function flushVolume() {
    if (brightVolDrag.pendingVolume < 0) return
    volumeProc.command = ["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", brightVolDrag.pendingVolume.toFixed(3)]
    brightVolDrag.pendingVolume = -1
    volumeProc.running = true
  }
  Process {
    id: volumeProc
    onExited: root.flushVolume()
  }
  function toggleMute() {
    root.muted = !root.muted
    root.run(["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"])
  }

  function toggleDnd() {
    if (root.notificationService && typeof root.notificationService.setDoNotDisturb === "function")
      root.notificationService.setDoNotDisturb(!root.dnd)
  }
  function clearNotifications() {
    if (root.notificationService && typeof root.notificationService.clearHistory === "function")
      root.notificationService.clearHistory()
    root.notifications = []
  }

  function toggleKeyboard() {
    root.run(["/usr/local/bin/vegas-toggle-keyboard"])
  }

  // Close first, so the shade isn't in the picture.
  function screenshot() {
    root.hideSelf()
    root.run(["bash", "-c",
      "sleep 0.6; d=\"${XDG_PICTURES_DIR:-$HOME/Pictures}\"; mkdir -p \"$d\"; " +
      "f=\"$d/screenshot-$(date +%Y%m%d-%H%M%S).png\"; grim \"$f\" && " +
      "notify-send -a omarchy-action -t 3000 'Screenshot saved' \"${f/#$HOME/~}\""])
  }

  function openMenu(route) {
    root.hideSelf()
    if (root.shell && typeof root.shell.summon === "function")
      root.shell.summon("omarchy.menu", JSON.stringify({ menu: route }))
  }

  function wifiToggle() {
    root.wifiMessage = ""
    root.run(["vegas-wifi", root.wifiOff ? "on" : "off"])
    refreshLater.restart()
  }
  function scan() {
    if (scanProc.running) return
    root.scanning = true
    scanProc.running = true
  }
  Process {
    id: scanProc
    command: ["vegas-wifi", "list", "--scan"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        var nets = []
        var lines = String(text || "").split("\n")
        for (var i = 0; i < lines.length; i++) root.parseNetwork(lines[i], nets)
        root.networks = nets
        root.scanning = false
      }
    }
  }

  function pickNetwork(net) {
    root.wifiMessage = ""
    if (net.current) return
    if (net.secured && !net.saved) {
      root.joining = net.ssid
      root.run(["pkill", "-USR2", "-x", "wvkbd-mobintl"])
      return
    }
    root.join(net.ssid, "")
  }
  function join(ssid, password) {
    if (joinProc.running) return
    root.joining = ""
    root.wifiMessage = "Connecting to " + ssid + "…"
    joinProc.password = password
    joinProc.command = ["vegas-wifi", "join", ssid]
    joinProc.running = true
  }
  Process {
    id: joinProc
    property string password: ""
    stdinEnabled: true
    // The password goes over stdin, never on a command line.
    onStarted: { joinProc.write(joinProc.password + "\n"); joinProc.password = "" }
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        var out = String(text || "").trim()
        root.wifiMessage = out === "connected" ? "" : (out || "Could not connect")
        root.refresh()
      }
    }
  }

  Timer {
    id: refreshLater
    interval: 1500
    onTriggered: root.refresh()
  }

  // ------------------------------------------------------------ helpers
  function glyph(code) { return String.fromCodePoint(code) }
  function signalBars(dbm) { return dbm >= -55 ? 4 : dbm >= -67 ? 3 : dbm >= -78 ? 2 : 1 }
  readonly property var wifiGlyphs: [0xf092e, 0xf091f, 0xf0922, 0xf0925, 0xf0928]

  function ago(ms) {
    var s = Math.max(0, Math.round((Date.now() - Number(ms || 0)) / 1000))
    if (s < 60) return "now"
    if (s < 3600) return Math.round(s / 60) + " min"
    if (s < 86400) return Math.round(s / 3600) + " h"
    return Math.round(s / 86400) + " d"
  }

  function stripMarkup(s) { return String(s || "").replace(/<[^>]*>/g, "") }

  // ------------------------------------------------------------ surface
  PanelWindow {
    id: shade

    visible: root.opened
    anchors {
      top: true
      bottom: true
      left: true
      right: true
    }
    // Stay between the status bar, the nav bar and the keyboard.
    exclusionMode: ExclusionMode.Normal
    color: Util.alpha("#000000", 0.45)
    WlrLayershell.namespace: "vegas-quick"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.keyboardFocus: root.joining !== "" ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.None

    readonly property int pad: 12
    readonly property color fg: Color.foreground
    readonly property color muted: Util.alpha(Color.foreground, 0.6)
    readonly property color cardColor: Color.background
    readonly property color tileColor: Util.alpha(Color.foreground, 0.08)
    readonly property color tileOn: Color.accent
    readonly property color line: Util.alpha(Color.foreground, 0.14)

    // Tap outside the card closes (on release, see Home.qml).
    MouseArea {
      anchors.fill: parent
      onReleased: if (containsMouse) root.hideSelf()
    }

    Flickable {
      id: flick
      anchors.fill: parent
      contentHeight: card.height + 24
      interactive: contentHeight > height
      boundsBehavior: Flickable.StopAtBounds

      Rectangle {
        id: card
        x: shade.pad / 2
        width: flick.width - shade.pad
        height: content.implicitHeight + 2 * shade.pad
        color: shade.cardColor
        radius: Math.max(12, Style.cornerRadius)
        border.width: 1
        border.color: shade.line

        // Swallow taps on the card's background so they don't close it.
        MouseArea { anchors.fill: parent }

        // Swipe the card up to close.
        DragHandler {
          target: null
          xAxis.enabled: false
          enabled: !flick.interactive
          // activeTranslation is already reset when active turns false.
          property real lastDy: 0
          onActiveTranslationChanged: if (active) lastDy = activeTranslation.y
          onActiveChanged: {
            if (active) lastDy = 0
            else if (lastDy < -60) root.hideSelf()
          }
        }

        Column {
          id: content
          x: shade.pad
          y: shade.pad
          width: card.width - 2 * shade.pad
          spacing: 12

          // -------------------------------------------- header
          Item {
            width: parent.width
            height: 48

            SystemClock {
              id: clock
              precision: SystemClock.Minutes
            }
            Column {
              anchors.left: parent.left
              anchors.verticalCenter: parent.verticalCenter
              Text {
                text: Qt.formatTime(clock.date, "HH:mm")
                font.family: Style.font.family
                font.pixelSize: 28
                font.bold: true
                color: shade.fg
              }
              Text {
                text: Qt.formatDate(clock.date, "ddd, MMM d")
                font.family: Style.font.family
                font.pixelSize: Style.font.bodySmall
                color: shade.muted
              }
            }
            Column {
              anchors.right: parent.right
              anchors.verticalCenter: parent.verticalCenter
              Text {
                anchors.right: parent.right
                text: root.battery >= 0 ? root.battery + "%" : ""
                font.family: Style.font.family
                font.pixelSize: 20
                font.bold: true
                color: shade.fg
              }
              Text {
                anchors.right: parent.right
                text: (root.batteryStatus === "Charging" ? "Charging · " : root.batteryStatus === "Full" ? "Full · "
                   : root.batteryStatus === "Not charging" ? "Plugged in · " : "")
                  + (root.batteryTemp > 0 ? root.batteryTemp.toFixed(1) + " °C" : "")
                font.family: Style.font.family
                font.pixelSize: Style.font.bodySmall
                color: root.batteryTemp >= 37 ? Color.urgent : shade.muted
              }
            }
          }

          // -------------------------------------------- tiles
          Grid {
            id: tiles
            width: parent.width
            columns: 2
            spacing: 8
            readonly property real tileWidth: (width - spacing) / 2

            Repeater {
              model: [
                { key: "wifi" }, { key: "dnd" },
                { key: "keyboard" }, { key: "screenshot" },
                { key: "menu" }, { key: "power" }
              ]

              Rectangle {
                id: tile
                required property var modelData
                readonly property string key: modelData.key
                readonly property bool on: key === "wifi" ? root.wifiConnected
                  : key === "dnd" ? root.dnd : false
                readonly property string title: key === "wifi" ? "Wi-Fi"
                  : key === "dnd" ? "Do Not Disturb"
                  : key === "keyboard" ? "Keyboard"
                  : key === "screenshot" ? "Screenshot"
                  : key === "menu" ? "Omarchy" : "Power"
                readonly property string subtitle: key === "wifi"
                    ? (root.wifiConnected && root.currentNetwork ? root.currentNetwork.ssid
                       : root.wifiOff ? "Off" : root.wifiState === "COMPLETED" ? "Connected" : "Not connected")
                  : key === "dnd" ? (root.dnd ? "On" : "Off")
                  : key === "keyboard" ? "Show / hide"
                  : key === "screenshot" ? "Whole screen"
                  : key === "menu" ? "Menu" : "Restart, power off"
                readonly property string icon: key === "wifi"
                    ? root.glyph(root.wifiConnected && root.currentNetwork
                                 ? root.wifiGlyphs[root.signalBars(root.currentNetwork.dbm)] : 0xf05aa)
                  : key === "dnd" ? root.glyph(root.dnd ? 0xf009b : 0xf009a)
                  : key === "keyboard" ? root.glyph(0xf030c)
                  : key === "screenshot" ? root.glyph(0xf0100)
                  : key === "menu" ? "" : root.glyph(0xf0425)

                width: tiles.tileWidth
                height: 58
                radius: Math.max(10, Style.cornerRadius)
                color: tileTouch.pressed ? Util.alpha(Color.foreground, 0.2)
                  : on ? Util.alpha(shade.tileOn, 0.85) : shade.tileColor

                Text {
                  id: tileIcon
                  x: 12
                  anchors.verticalCenter: parent.verticalCenter
                  width: 26
                  horizontalAlignment: Text.AlignHCenter
                  text: tile.icon
                  font.family: tile.key === "menu" ? "omarchy" : Style.font.family
                  font.pixelSize: 22
                  color: tile.on ? Color.background : shade.fg
                }
                Column {
                  anchors.left: tileIcon.right
                  anchors.leftMargin: 10
                  anchors.right: parent.right
                  anchors.rightMargin: 8
                  anchors.verticalCenter: parent.verticalCenter
                  Text {
                    width: parent.width
                    elide: Text.ElideRight
                    text: tile.title
                    font.family: Style.font.family
                    font.pixelSize: Style.font.body
                    font.bold: true
                    color: tile.on ? Color.background : shade.fg
                  }
                  Text {
                    width: parent.width
                    elide: Text.ElideRight
                    text: tile.subtitle
                    font.family: Style.font.family
                    font.pixelSize: Style.font.bodySmall
                    color: tile.on ? Util.alpha(Color.background, 0.8) : shade.muted
                  }
                }

                MouseArea {
                  id: tileTouch
                  anchors.fill: parent
                  pressAndHoldInterval: 550
                  property bool held: false
                  onPressed: held = false
                  onPressAndHold: held = true
                  onReleased: {
                    if (!containsMouse) return
                    var k = tile.key
                    if (k === "wifi") {
                      // Tap: networks; long-press: on/off.
                      if (held) root.wifiToggle()
                      else { root.wifiExpanded = !root.wifiExpanded; if (root.wifiExpanded) root.scan() }
                    } else if (k === "dnd") root.toggleDnd()
                    else if (k === "keyboard") root.toggleKeyboard()
                    else if (k === "screenshot") root.screenshot()
                    else if (k === "menu") root.openMenu("root")
                    else root.openMenu("system")
                  }
                }
              }
            }
          }

          // -------------------------------------------- sliders
          Row {
            width: parent.width
            height: 40
            spacing: 8
            Text {
              width: 30
              height: parent.height
              horizontalAlignment: Text.AlignHCenter
              verticalAlignment: Text.AlignVCenter
              text: root.glyph(0xf00df)
              font.family: Style.font.family
              font.pixelSize: 22
              color: shade.fg
            }
            PanelSlider {
              width: parent.width - 38
              height: parent.height
              value: root.brightnessMax > 0 ? root.brightnessRaw / root.brightnessMax : 0
              trackHeight: 6
              knobSize: 22
              trackColor: Util.alpha(Color.foreground, 0.18)
              onMoved: function(v) { root.setBrightness(v) }
              onReleased: function(v) { root.setBrightness(v) }
            }
          }

          Row {
            width: parent.width
            height: 40
            spacing: 8
            Text {
              width: 30
              height: parent.height
              horizontalAlignment: Text.AlignHCenter
              verticalAlignment: Text.AlignVCenter
              text: root.glyph(root.muted ? 0xf0581 : root.sink.indexOf("Headphones") !== -1 ? 0xf02cb : 0xf057e)
              font.family: Style.font.family
              font.pixelSize: 22
              color: root.muted ? shade.muted : shade.fg
              MouseArea {
                anchors.fill: parent
                onReleased: if (containsMouse) root.toggleMute()
              }
            }
            PanelSlider {
              id: volumeSlider
              width: parent.width - 38 - sinkLabel.width - 8
              height: parent.height
              value: root.volume
              trackHeight: 6
              knobSize: 22
              trackColor: Util.alpha(Color.foreground, 0.18)
              onDraggingChanged: brightVolDrag.volume = dragging
              onMoved: function(v) { root.setVolume(v) }
              onReleased: function(v) { root.setVolume(v) }
            }
            Text {
              id: sinkLabel
              height: parent.height
              verticalAlignment: Text.AlignVCenter
              text: root.muted ? "Muted" : Math.round(root.volume * 100) + "%"
              font.family: Style.font.family
              font.pixelSize: Style.font.bodySmall
              color: shade.muted
            }
          }

          // -------------------------------------------- Wi-Fi networks
          Column {
            width: parent.width
            spacing: 4
            visible: root.wifiExpanded

            Item {
              width: parent.width
              height: 30
              Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.scanning ? "Wi-Fi · scanning…" : "Wi-Fi networks"
                font.family: Style.font.family
                font.pixelSize: Style.font.body
                font.bold: true
                color: shade.fg
              }
              ToggleSwitch {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                checked: !root.wifiOff
                onToggled: root.wifiToggle()
              }
            }

            Text {
              width: parent.width
              visible: root.wifiMessage !== ""
              wrapMode: Text.Wrap
              text: root.wifiMessage
              font.family: Style.font.family
              font.pixelSize: Style.font.bodySmall
              color: shade.muted
            }

            Repeater {
              model: root.networks

              Column {
                id: netRow
                required property var modelData
                width: parent.width
                spacing: 4

                Rectangle {
                  width: parent.width
                  height: 44
                  radius: Math.max(8, Style.cornerRadius)
                  color: netTouch.pressed ? Util.alpha(Color.foreground, 0.16)
                    : netRow.modelData.current ? Util.alpha(Color.accent, 0.25) : "transparent"

                  Text {
                    id: netIcon
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.glyph(root.wifiGlyphs[root.signalBars(netRow.modelData.dbm)])
                    font.family: Style.font.family
                    font.pixelSize: 20
                    color: shade.fg
                  }
                  Text {
                    anchors.left: netIcon.right
                    anchors.leftMargin: 10
                    anchors.right: netState.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: netRow.modelData.ssid
                    font.family: Style.font.family
                    font.pixelSize: Style.font.body
                    font.bold: netRow.modelData.current
                    color: shade.fg
                  }
                  Text {
                    id: netState
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: (netRow.modelData.current ? "Connected  " : netRow.modelData.saved ? "Saved  " : "")
                      + (netRow.modelData.secured ? root.glyph(0xf033e) : "")
                    font.family: Style.font.family
                    font.pixelSize: Style.font.bodySmall
                    color: shade.muted
                  }
                  MouseArea {
                    id: netTouch
                    anchors.fill: parent
                    onReleased: if (containsMouse) root.pickNetwork(netRow.modelData)
                  }
                }

                // Password prompt under the chosen network.
                Row {
                  visible: root.joining === netRow.modelData.ssid
                  width: parent.width
                  height: visible ? 40 : 0
                  spacing: 8
                  TextField {
                    id: passwordField
                    width: parent.width - joinButton.width - 8
                    height: parent.height
                    password: true
                    placeholderText: "Password"
                    onVisibleChanged: if (visible) { text = ""; forceActiveFocus() }
                    onAccepted: root.join(netRow.modelData.ssid, text)
                  }
                  Rectangle {
                    id: joinButton
                    width: 84
                    height: parent.height
                    radius: Math.max(8, Style.cornerRadius)
                    color: joinTouch.pressed ? Util.alpha(Color.accent, 0.6) : Color.accent
                    Text {
                      anchors.centerIn: parent
                      text: "Connect"
                      font.family: Style.font.family
                      font.pixelSize: Style.font.body
                      font.bold: true
                      color: Color.background
                    }
                    MouseArea {
                      id: joinTouch
                      anchors.fill: parent
                      onReleased: if (containsMouse) root.join(netRow.modelData.ssid, passwordField.text)
                    }
                  }
                }
              }
            }
          }

          // -------------------------------------------- notifications
          Rectangle {
            width: parent.width
            height: 1
            color: shade.line
          }

          Item {
            width: parent.width
            height: 26
            Text {
              anchors.verticalCenter: parent.verticalCenter
              text: "Notifications"
              font.family: Style.font.family
              font.pixelSize: Style.font.body
              font.bold: true
              color: shade.fg
            }
            Text {
              anchors.right: parent.right
              anchors.verticalCenter: parent.verticalCenter
              visible: root.notifications.length > 0
              text: "Clear all"
              font.family: Style.font.family
              font.pixelSize: Style.font.bodySmall
              color: Color.accent
              MouseArea {
                anchors.fill: parent
                anchors.margins: -10
                onReleased: if (containsMouse) root.clearNotifications()
              }
            }
          }

          Text {
            visible: root.notifications.length === 0
            text: root.dnd ? "None · Do Not Disturb is on" : "None"
            font.family: Style.font.family
            font.pixelSize: Style.font.bodySmall
            color: shade.muted
          }

          Repeater {
            model: root.notifications

            Column {
              id: note
              required property var modelData
              width: parent.width
              spacing: 1
              Text {
                width: parent.width
                elide: Text.ElideRight
                text: (note.modelData.app || "App") + " · " + root.ago(note.modelData.timestamp)
                font.family: Style.font.family
                font.pixelSize: Style.font.bodySmall
                color: shade.muted
              }
              Text {
                width: parent.width
                elide: Text.ElideRight
                text: root.stripMarkup(note.modelData.summary)
                font.family: Style.font.family
                font.pixelSize: Style.font.body
                font.bold: true
                color: shade.fg
              }
              Text {
                width: parent.width
                visible: text !== ""
                maximumLineCount: 2
                wrapMode: Text.Wrap
                elide: Text.ElideRight
                text: root.stripMarkup(note.modelData.body)
                font.family: Style.font.family
                font.pixelSize: Style.font.bodySmall
                color: shade.fg
              }
            }
          }

          // Grabber: a hint that the card swipes up.
          Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 40
            height: 4
            radius: 2
            color: shade.line
          }
        }
      }
    }
  }
}
