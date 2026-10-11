import QtQuick
import Quickshell
import Quickshell.Wayland
import qs.Commons

// Home screen: what an empty workspace shows. It sits on the bottom layer
// and is mapped only while the focused workspace has no app, so it costs
// nothing while an app is on screen.
//
// Top: glanceable clock and date. Bottom (thumb reach): open apps, then the
// pinned row. Tap an open app to return to it, swipe it sideways or tap x to
// close it. Tap a pinned app to open it (or switch to it if already open);
// long-press opens another instance.
PanelWindow {
  id: home

  property var mobile: null

  readonly property int pad: 16
  readonly property int rowHeight: 60
  readonly property int pinnedColumns: Math.max(1, Math.min(5, mobile ? mobile.pinned.length : 5))
  readonly property color fg: Color.foreground
  readonly property color muted: Util.alpha(Color.foreground, 0.6)
  readonly property color card: Util.alpha(Color.background, 0.9)
  readonly property color cardBorder: Util.alpha(Color.foreground, 0.16)

  visible: mobile !== null && mobile.homeEnabled && mobile.onHome
  anchors {
    top: true
    bottom: true
    left: true
    right: true
  }
  exclusionMode: ExclusionMode.Normal
  color: Util.alpha(Color.background, 0.35)
  WlrLayershell.namespace: "vegas-home"
  WlrLayershell.layer: WlrLayer.Bottom
  WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

  SystemClock {
    id: clock
    precision: SystemClock.Minutes
  }

  // ------------------------------------------------------------ clock
  Column {
    id: clockBlock
    anchors.top: parent.top
    anchors.topMargin: 36
    anchors.horizontalCenter: parent.horizontalCenter
    spacing: 2

    Text {
      anchors.horizontalCenter: parent.horizontalCenter
      text: Qt.formatTime(clock.date, "HH:mm")
      font.family: Style.font.family
      font.pixelSize: 64
      font.bold: true
      color: home.fg
    }
    Text {
      anchors.horizontalCenter: parent.horizontalCenter
      text: Qt.formatDate(clock.date, "dddd, MMMM d")
      font.family: Style.font.family
      font.pixelSize: Style.font.heading
      color: home.fg
    }
  }

  // ------------------------------------------------------------ pinned
  Rectangle {
    id: pinnedCard
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.bottom: parent.bottom
    anchors.margins: home.pad
    height: pinnedGrid.implicitHeight + 16
    color: home.card
    border.width: 1
    border.color: home.cardBorder
    radius: Style.cornerRadius

    Grid {
      id: pinnedGrid
      anchors.centerIn: parent
      columns: home.pinnedColumns
      readonly property real cellWidth: (pinnedCard.width - 8) / home.pinnedColumns

      Repeater {
        model: home.mobile ? home.mobile.pinned : []

        Item {
          id: pin
          required property var modelData
          width: pinnedGrid.cellWidth
          height: 84

          Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            visible: pinTouch.pressed
            color: Util.alpha(Color.foreground, 0.12)
            radius: Style.cornerRadius
          }

          AppIcon {
            id: pinIcon
            anchors.horizontalCenter: parent.horizontalCenter
            y: 8
            size: 44
            name: String(pin.modelData.name || "")
            source: pin.modelData.icon ? home.mobile.iconSource(pin.modelData.icon) : ""
            glyph: String(pin.modelData.glyph || "")
            glyphFont: String(pin.modelData.font || "")
          }

          Text {
            anchors.top: pinIcon.bottom
            anchors.topMargin: 6
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - 4
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: String(pin.modelData.name || "")
            font.family: Style.font.family
            font.pixelSize: Style.font.bodySmall
            color: home.fg
          }

          MouseArea {
            id: pinTouch
            anchors.fill: parent
            pressAndHoldInterval: 550
            property bool held: false
            // Act on release only: unmapping home (the app opens over it)
            // while the finger is still down leaves Qt's touch point stuck
            // and swallows the next touch.
            onPressed: held = false
            onPressAndHold: held = true
            onReleased: if (held && containsMouse) home.mobile.launch(pin.modelData, true)
            onClicked: if (!held) home.mobile.launch(pin.modelData, false)
          }
        }
      }
    }
  }

  // ------------------------------------------------------------ open apps
  Text {
    id: openHeader
    anchors.left: parent.left
    anchors.leftMargin: home.pad + 4
    anchors.bottom: openList.top
    anchors.bottomMargin: 8
    text: home.mobile && home.mobile.apps.length > 0
      ? "OPEN APPS  ·  " + home.mobile.apps.length
      : "NO OPEN APPS"
    font.family: Style.font.family
    font.pixelSize: Style.font.caption
    font.bold: true
    font.letterSpacing: 1
    color: home.fg
  }

  Flickable {
    id: openList
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.bottom: pinnedCard.top
    anchors.leftMargin: home.pad
    anchors.rightMargin: home.pad
    anchors.bottomMargin: 20
    readonly property real available: pinnedCard.y - 20 - (clockBlock.y + clockBlock.height + 40)
    height: Math.max(0, Math.min(contentHeight, available))
    contentHeight: openColumn.implicitHeight
    clip: contentHeight > height
    interactive: contentHeight > height
    boundsBehavior: Flickable.StopAtBounds

    Column {
      id: openColumn
      width: openList.width
      spacing: 8

      Repeater {
        model: home.mobile ? home.mobile.apps : []

        Item {
          id: row
          required property var modelData
          width: openColumn.width
          height: home.rowHeight
          clip: true

          Rectangle {
            id: rowCard
            width: parent.width
            height: parent.height
            color: rowTouch.pressed && !rowTouch.drag.active ? Qt.tint(home.card, Util.alpha(Color.foreground, 0.1)) : home.card
            readonly property bool last: home.mobile && row.modelData.workspaceId === home.mobile.lastAppWorkspaceId
            border.width: last ? 2 : 1
            border.color: last ? Color.accent : home.cardBorder
            radius: Style.cornerRadius
            opacity: 1 - Math.min(0.6, Math.abs(x) / width)

            AppIcon {
              id: rowIcon
              anchors.left: parent.left
              anchors.leftMargin: 12
              anchors.verticalCenter: parent.verticalCenter
              size: 36
              name: row.modelData.name
              source: row.modelData.icon
              glyph: row.modelData.glyph
              glyphFont: row.modelData.glyphFont
            }

            Column {
              anchors.left: rowIcon.right
              anchors.leftMargin: 12
              anchors.right: closeButton.left
              anchors.verticalCenter: parent.verticalCenter
              spacing: 2

              Text {
                width: parent.width
                elide: Text.ElideRight
                text: row.modelData.name
                font.family: Style.font.family
                font.pixelSize: Style.font.title
                font.bold: true
                color: home.fg
              }
              Text {
                width: parent.width
                elide: Text.ElideRight
                visible: text !== "" && text !== row.modelData.name
                text: row.modelData.title
                font.family: Style.font.family
                font.pixelSize: Style.font.bodySmall
                color: home.muted
              }
            }

            MouseArea {
              id: rowTouch
              anchors.fill: parent
              anchors.rightMargin: closeButton.width
              drag.target: rowCard
              drag.axis: Drag.XAxis
              drag.threshold: 12
              drag.filterChildren: false
              onClicked: if (!drag.active && Math.abs(rowCard.x) < 12) home.mobile.switchTo(row.modelData)
              onReleased: {
                if (Math.abs(rowCard.x) > rowCard.width * 0.35) home.mobile.closeApp(row.modelData)
                else rowCard.x = 0
              }
              onCanceled: rowCard.x = 0
            }

            Item {
              id: closeButton
              anchors.right: parent.right
              anchors.verticalCenter: parent.verticalCenter
              width: 52
              height: parent.height

              Rectangle {
                anchors.centerIn: parent
                width: 36
                height: 36
                radius: 18
                color: closeTouch.pressed ? Util.alpha(Color.urgent, 0.35) : Util.alpha(Color.foreground, 0.08)
              }
              Text {
                anchors.centerIn: parent
                text: ""
                font.family: Style.font.family
                font.pixelSize: Style.font.heading
                color: home.fg
              }
              MouseArea {
                id: closeTouch
                anchors.fill: parent
                onClicked: home.mobile.closeApp(row.modelData)
              }
            }
          }
        }
      }
    }
  }
}
