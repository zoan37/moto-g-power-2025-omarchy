import QtQuick
import qs.Ui
import qs.Commons

BarWidget {
  id: root
  moduleName: "vegas.keyboard"
  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight
  WidgetButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "\uf11c"  // Nerd Font keyboard icon
    fontSize: Style.font.body
    horizontalMargin: 12
    tooltipText: "Show or hide keyboard"
    onPressed: if (root.bar) root.bar.run("vegas-toggle-keyboard")
  }
}
