import QtQuick
import Quickshell.Io
import qs.Ui
import qs.Commons

// Wi-Fi strength for wpa_supplicant setups. Reads the kernel's
// /proc/net/wireless (no process spawn) every `interval` seconds.
BarWidget {
  id: root
  moduleName: "vegas.wifi"

  readonly property string iface: String(setting("interface", "wlan0"))
  property int level: 0      // dBm, 0 = unknown
  property bool connected: false
  readonly property int bars: !connected ? 0 : level >= -55 ? 4 : level >= -67 ? 3 : level >= -78 ? 2 : 1
  // Nerd Font md-wifi-strength-{off-outline,1,2,3,4}
  readonly property var glyphs: [0xf092e, 0xf091f, 0xf0922, 0xf0925, 0xf0928]

  implicitWidth: label.implicitWidth + 12
  implicitHeight: barSize

  function parse(text) {
    var lines = String(text || "").split("\n")
    for (var i = 0; i < lines.length; i++) {
      var fields = lines[i].trim().split(/\s+/)
      if (fields[0] !== root.iface + ":") continue
      var dbm = parseFloat(fields[3])
      if (dbm > 0) dbm -= 256
      root.level = isFinite(dbm) ? Math.round(dbm) : 0
      root.connected = isFinite(dbm) && dbm < 0
      return
    }
    root.connected = false
  }

  Text {
    id: label
    anchors.centerIn: parent
    text: String.fromCodePoint(root.glyphs[root.bars])
    color: root.bar ? root.bar.foreground : Color.foreground
    opacity: root.connected ? 1 : 0.5
    font.family: root.bar ? root.bar.fontFamily : Style.font.family
    font.pixelSize: Style.bar.iconFont + 2
  }

  FileView {
    id: wireless
    path: "/proc/net/wireless"
    blockLoading: true
    printErrors: false
    onLoaded: root.parse(text())
  }

  Timer {
    interval: Math.max(5, Number(root.setting("interval", 20)) || 20) * 1000
    running: true
    repeat: true
    triggeredOnStart: true
    onTriggered: wireless.reload()
  }
}
