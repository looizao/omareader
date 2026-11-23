import QtQuick
import QtQuick.Shapes

// A crescent moon for the sleep timer, drawn as a shape so it looks the same
// in every font: an outer arc round the left, and a shallower one back.
Item {
    id: icon
    property color color: "white"
    implicitWidth: 18
    implicitHeight: 18
    // The outer circle and where its horns are. The crescent runs from the
    // circle's left edge to its horns, half a radius right of its centre, so
    // the circle sits a quarter of a radius right of the item's centre to
    // centre the crescent itself.
    readonly property real radius: Math.min(width, height) * 0.46
    readonly property real hornX: width / 2 + radius * 0.75
    readonly property real hornY: radius * Math.sqrt(1 - 0.25)

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: icon.color
            strokeColor: "transparent"
            startX: icon.hornX; startY: icon.height / 2 - icon.hornY
            PathArc {
                x: icon.hornX; y: icon.height / 2 + icon.hornY
                radiusX: icon.radius; radiusY: icon.radius
                useLargeArc: true
                direction: PathArc.Counterclockwise
            }
            PathArc {
                x: icon.hornX; y: icon.height / 2 - icon.hornY
                radiusX: icon.radius * 0.95; radiusY: icon.radius * 0.95
            }
        }
    }
}
