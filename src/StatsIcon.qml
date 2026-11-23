import QtQuick

// Three bars rising from a baseline, drawn so they look the same in every
// font: the statistics button's icon.
Item {
    id: icon
    property color color: "white"
    implicitWidth: 16
    implicitHeight: 16
    readonly property real bar: Math.max(2, Math.round(width / 5))
    readonly property real gap: (width - 3 * bar) / 2

    Repeater {
        model: [0.5, 1.0, 0.72]
        Rectangle {
            required property real modelData
            required property int index
            x: index * (icon.bar + icon.gap)
            y: icon.height - height
            width: icon.bar
            height: Math.round(icon.height * modelData)
            radius: 1
            antialiasing: true
            color: icon.color
        }
    }
}
