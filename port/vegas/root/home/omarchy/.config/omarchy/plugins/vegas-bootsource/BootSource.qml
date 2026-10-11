import QtQuick
import Quickshell.Io
import qs.Ui
import qs.Commons

// Takes no space on a normal internal-storage boot. When the ramdisk fell back
// to (or was told to use) the microSD rescue copy, shows a red SD badge.
BarWidget {
  id: root
  moduleName: "vegas.bootsource"

  property bool onCard: false
  visible: onCard
  implicitWidth: onCard ? button.implicitWidth : 0
  implicitHeight: button.implicitHeight

  WidgetButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "SD"
    fontSize: Style.font.body
    horizontalMargin: 10
    active: true
    tooltipText: "Running from the SD card rescue copy (slow)"
  }

  Process {
    running: true
    command: ["cat", "/run/vegas-boot-source"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: root.onCard = text.trim().split(" ")[0] === "card"
    }
  }
}
