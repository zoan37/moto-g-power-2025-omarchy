import QtQuick
import qs.Commons

// An app icon on a tile, so dark glyph-style icons (Grok) stay readable on
// a dark theme. Without an image, a glyph or the name's initial is drawn.
Item {
  id: root

  property string source: ""
  property string glyph: ""
  property string glyphFont: ""
  property string name: ""
  property int size: 40

  implicitWidth: size
  implicitHeight: size

  Rectangle {
    anchors.fill: parent
    radius: Math.round(root.size * 0.22)
    color: Util.alpha(Color.foreground, image.status === Image.Ready ? 0.85 : 0.12)
    border.width: 1
    border.color: Util.alpha(Color.foreground, 0.18)
  }

  Text {
    anchors.centerIn: parent
    visible: image.status !== Image.Ready
    text: root.glyph !== "" ? root.glyph : String(root.name || "?").charAt(0).toUpperCase()
    font.family: root.glyphFont !== "" ? root.glyphFont : Style.font.family
    font.pixelSize: Math.round(root.size * 0.48)
    font.bold: root.glyph === ""
    color: Color.foreground
  }

  Image {
    id: image
    anchors.centerIn: parent
    width: Math.round(root.size * 0.72)
    height: width
    source: root.source
    visible: status === Image.Ready
    sourceSize.width: width * 3
    sourceSize.height: height * 3
    fillMode: Image.PreserveAspectFit
    asynchronous: true
    smooth: true
    mipmap: true
  }
}
