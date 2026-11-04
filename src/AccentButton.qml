import QtQuick
import QtQuick.Controls

// The one prominent button on a screen, filled with the theme accent.
Button {
    id: control
    implicitHeight: 40
    leftPadding: 18
    rightPadding: 18
    contentItem: Label {
        text: control.text
        color: theme.accentForeground
        font.pixelSize: 14
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: Math.min(8, theme.radius)
        color: !control.enabled ? Colors.line
             : control.down ? Qt.darker(theme.accent, 1.15)
             : control.hovered ? Qt.lighter(theme.accent, 1.08) : theme.accent
    }
}
