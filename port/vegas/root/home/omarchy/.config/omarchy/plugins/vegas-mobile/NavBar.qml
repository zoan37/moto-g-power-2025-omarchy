import QtQuick
import Quickshell
import Quickshell.Wayland
import qs.Commons

// Bottom navigation bar. One opaque strip reserved as an exclusive zone, so
// it never covers app content and its touches never compete with the app's.
//
//   [ ‹ Back ]   [ home handle + open-app dots ]   [ keyboard ]
//
// Gestures anywhere on the strip: drag left/right to step through open apps
// (the label previews the target), drag up to go home, long-press the
// centre for the Omarchy menu. Nothing animates; the strip repaints only on
// touch and on app/workspace changes.
PanelWindow {
  id: nav

  property var mobile: null

  // Live drag state, used only for the preview label.
  property real dragDx: 0
  property real dragDy: 0
  property string dragAxis: ""
  property int pressedZone: -1

  readonly property color fg: Color.bar.text
  readonly property color muted: Util.alpha(Color.bar.text, 0.4)
  readonly property color accent: Color.accent
  readonly property int iconSize: Math.round(nav.height * 0.42)
  readonly property int swipeDistance: 40
  readonly property int swipeUpDistance: 18
  readonly property int slop: 8

  readonly property var previewTarget: dragAxis === "h" && Math.abs(dragDx) > slop && mobile
    ? mobile.neighbour(dragDx < 0 ? 1 : -1) : null
  readonly property bool previewing: dragAxis === "h" && Math.abs(dragDx) > slop
  readonly property bool armed: (dragAxis === "h" && Math.abs(dragDx) >= swipeDistance && previewTarget !== null)
    || (dragAxis === "v" && -dragDy >= swipeUpDistance)

  anchors {
    left: true
    right: true
    bottom: true
  }
  implicitHeight: mobile ? mobile.navHeight : 44
  exclusionMode: ExclusionMode.Auto
  color: Color.bar.background
  WlrLayershell.namespace: "vegas-nav"
  WlrLayershell.layer: WlrLayer.Top
  WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

  function zoneAt(x) {
    return x < nav.width / 3 ? 0 : (x > nav.width * 2 / 3 ? 2 : 1)
  }

  function resetDrag() {
    nav.dragDx = 0
    nav.dragDy = 0
    nav.dragAxis = ""
    nav.pressedZone = -1
  }

  // Pressed-zone highlight.
  Rectangle {
    visible: nav.pressedZone >= 0
    x: nav.pressedZone * nav.width / 3
    width: nav.width / 3
    height: nav.height
    color: Util.alpha(Color.bar.text, 0.12)
  }

  // Back.
  Text {
    x: 0
    width: nav.width / 3
    height: nav.height
    horizontalAlignment: Text.AlignHCenter
    verticalAlignment: Text.AlignVCenter
    text: ""
    font.family: Style.font.family
    font.pixelSize: nav.iconSize
    color: nav.mobile && !nav.mobile.onHome ? nav.fg : nav.muted
    visible: !nav.previewing
  }

  // Home handle and one dot per open app (the current one is a dash), so the
  // strip always shows what is running and where a swipe will land.
  Item {
    id: centre
    x: nav.width / 3
    width: nav.width / 3
    height: nav.height
    visible: !nav.previewing

    Rectangle {
      id: handle
      anchors.horizontalCenter: parent.horizontalCenter
      y: Math.round(parent.height / 2 - height - 3)
      width: Math.round(nav.width * 0.16)
      height: 4
      radius: 2
      color: nav.mobile && nav.mobile.onHome ? nav.accent : nav.fg
    }

    Row {
      anchors.horizontalCenter: parent.horizontalCenter
      anchors.top: handle.bottom
      anchors.topMargin: 6
      spacing: 5

      Repeater {
        model: nav.mobile ? nav.mobile.apps.length : 0
        Rectangle {
          required property int index
          readonly property bool current: nav.mobile && nav.mobile.currentIndex === index
          width: current ? 12 : 5
          height: 5
          radius: 2.5
          color: current ? nav.accent : nav.muted
        }
      }
    }
  }

  // Keyboard: highlighted while the on-screen keyboard is up.
  Text {
    x: nav.width * 2 / 3
    width: nav.width / 3
    height: nav.height
    horizontalAlignment: Text.AlignHCenter
    verticalAlignment: Text.AlignVCenter
    text: ""
    font.family: Style.font.family
    font.pixelSize: nav.iconSize
    color: nav.mobile && nav.mobile.keyboardVisible ? nav.accent : nav.fg
    visible: !nav.previewing
  }

  // Swipe preview: where releasing will go.
  Text {
    anchors.fill: parent
    anchors.leftMargin: 16
    anchors.rightMargin: 16
    visible: nav.previewing
    horizontalAlignment: nav.dragDx < 0 ? Text.AlignRight : Text.AlignLeft
    verticalAlignment: Text.AlignVCenter
    elide: Text.ElideMiddle
    text: nav.previewTarget
      ? (nav.dragDx < 0 ? nav.previewTarget.name + "  " : "  " + nav.previewTarget.name)
      : "No more apps"
    font.family: Style.font.family
    font.pixelSize: Style.font.title
    font.bold: nav.armed
    color: nav.armed ? nav.accent : nav.muted
  }

  MouseArea {
    id: touch
    anchors.fill: parent
    preventStealing: true
    pressAndHoldInterval: 550

    property real startX: 0
    property real startY: 0
    property bool held: false

    onPressed: function(mouse) {
      startX = mouse.x
      startY = mouse.y
      held = false
      nav.dragAxis = ""
      nav.dragDx = 0
      nav.dragDy = 0
      nav.pressedZone = nav.zoneAt(mouse.x)
    }

    onPositionChanged: function(mouse) {
      if (nav.mobile && nav.mobile.debug) console.log("vegas.mobile nav move", mouse.x, mouse.y)
      var dx = mouse.x - startX
      var dy = mouse.y - startY
      if (nav.dragAxis === "" && nav.mobile && nav.mobile.gesturesEnabled
          && (Math.abs(dx) > nav.slop || Math.abs(dy) > nav.slop)) {
        nav.dragAxis = Math.abs(dx) >= Math.abs(dy) ? "h" : "v"
        nav.pressedZone = -1
        held = false
      }
      if (nav.dragAxis === "h") nav.dragDx = dx
      else if (nav.dragAxis === "v") nav.dragDy = dy
    }

    // Long-press only arms the action; it runs on release, so no surface
    // appears or disappears while a finger is still down.
    onPressAndHold: {
      if (nav.dragAxis !== "" || !nav.mobile) return
      held = nav.pressedZone === 1
    }

    onReleased: function(mouse) {
      var m = nav.mobile
      if (m && m.debug) console.log("vegas.mobile nav release", mouse.x, mouse.y, "axis", nav.dragAxis, "dx", nav.dragDx, "dy", nav.dragDy, "held", held)
      if (m && held) {
        m.openMenu("root")
      } else if (m) {
        if (nav.dragAxis === "h") {
          if (Math.abs(nav.dragDx) >= nav.swipeDistance) m.step(nav.dragDx < 0 ? 1 : -1)
        } else if (nav.dragAxis === "v") {
          if (-nav.dragDy >= nav.swipeUpDistance) m.goHome()
        } else {
          var zone = nav.zoneAt(mouse.x)
          if (zone === 0) m.back()
          else if (zone === 1) m.goHome()
          else m.toggleKeyboard()
        }
      }
      nav.resetDrag()
    }

    onCanceled: {
      if (nav.mobile && nav.mobile.debug) console.log("vegas.mobile nav canceled")
      nav.resetDrag()
    }
  }
}
