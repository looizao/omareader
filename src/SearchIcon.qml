import QtQuick
import QtQuick.Shapes

// A magnifying glass, drawn as a shape so it looks the same in every font:
// a ring at the top left and a handle running to the bottom right corner.
Item {
    id: icon
    property color color: "white"
    implicitWidth: 16
    implicitHeight: 16
    readonly property real stroke: Math.max(1.5, width / 9)
    // The ring's centre and radius, kept inside the item with its stroke.
    readonly property real radius: width * 0.3
    readonly property real centre: icon.stroke / 2 + radius

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: "transparent"
            strokeColor: icon.color
            strokeWidth: icon.stroke
            startX: icon.centre + icon.radius; startY: icon.centre
            PathArc {
                x: icon.centre - icon.radius; y: icon.centre
                radiusX: icon.radius; radiusY: icon.radius
            }
            PathArc {
                x: icon.centre + icon.radius; y: icon.centre
                radiusX: icon.radius; radiusY: icon.radius
            }
        }
        ShapePath {
            fillColor: "transparent"
            strokeColor: icon.color
            strokeWidth: icon.stroke
            capStyle: ShapePath.RoundCap
            startX: icon.centre + icon.radius * 0.72; startY: icon.centre + icon.radius * 0.72
            PathLine { x: icon.width - icon.stroke / 2; y: icon.height - icon.stroke / 2 }
        }
    }
}
