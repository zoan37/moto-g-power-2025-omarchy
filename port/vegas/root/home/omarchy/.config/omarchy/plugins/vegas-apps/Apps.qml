import QtQuick
import Quickshell
import qs.Ui
import qs.Commons

// Pinned web apps. A tap focuses the app's window if it is already open (the
// bar then shows its workspace), otherwise opens it on the next empty
// workspace through the phone's Chrome --app launcher.
BarWidget {
  id: root
  moduleName: "vegas.apps"

  readonly property string home: Quickshell.env("HOME")
  // match: the Chrome --app window class; icon: path under $HOME.
  readonly property var apps: setting("apps", [
    { "name": "Grok", "url": "https://grok.com", "match": "chrome-grok.com",
      "icon": ".local/share/icons/hicolor/256x256/apps/grok.png" },
    { "name": "X", "url": "https://x.com/", "match": "chrome-x.com",
      "icon": ".local/share/applications/icons/X.png" }
  ])
  readonly property int iconSize: Math.round(barSize * 0.5)

  implicitWidth: row.implicitWidth
  implicitHeight: barSize

  function open(app) {
    if (!bar) return
    var launch = "/usr/local/bin/omarchy-launch-webapp " + Util.shellQuote(app.url)
    bar.run("omarchy-launch-or-focus " + Util.shellQuote(app.match) + " " + Util.shellQuote(launch))
  }

  Row {
    id: row
    anchors.verticalCenter: parent.verticalCenter

    Repeater {
      model: root.apps

      Item {
        required property var modelData
        width: root.iconSize + 18
        height: root.barSize

        Image {
          anchors.centerIn: parent
          width: root.iconSize
          height: root.iconSize
          sourceSize.width: root.iconSize * 2
          sourceSize.height: root.iconSize * 2
          fillMode: Image.PreserveAspectFit
          smooth: true
          mipmap: true
          source: Util.fileUrl(root.home + "/" + parent.modelData.icon)
          opacity: tap.pressed ? 0.6 : 1.0
        }

        MouseArea {
          id: tap
          anchors.fill: parent
          cursorShape: Qt.PointingHandCursor
          onClicked: root.open(parent.modelData)
        }
      }
    }
  }
}
