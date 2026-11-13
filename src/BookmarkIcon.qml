import QtQuick
import QtQuick.Shapes

// A bookmark ribbon, drawn as a shape so it looks the same in every font:
// filled when the place is marked, an outline when it isn't.
Item {
    id: icon
    property bool filled: true
    property color color: "white"
    implicitWidth: 12
    implicitHeight: 16
    // An outline is drawn inside the shape, so both take the same room.
    readonly property real inset: filled ? 0 : 0.75

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: icon.filled ? icon.color : "transparent"
            strokeColor: icon.filled ? "transparent" : icon.color
            strokeWidth: 1.5
            joinStyle: ShapePath.MiterJoin
            startX: icon.inset; startY: icon.inset
            PathLine { x: icon.width - icon.inset; y: icon.inset }
            PathLine { x: icon.width - icon.inset; y: icon.height - icon.inset }
            PathLine { x: icon.width / 2; y: icon.height * 0.7 }
            PathLine { x: icon.inset; y: icon.height - icon.inset }
            PathLine { x: icon.inset; y: icon.inset }
        }
    }
}
