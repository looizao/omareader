import QtQuick
import QtQuick.Shapes

// Transport icons drawn as shapes, so they look the same in every font
// (the Unicode media symbols come out as colour emoji).
Item {
    id: icon
    property string kind: "play" // play, pause, previous, next
    property color color: "white"
    implicitWidth: 20
    implicitHeight: 20

    Shape {
        anchors.fill: parent
        visible: icon.kind === "play"
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: icon.color
            strokeColor: "transparent"
            startX: icon.width * 0.22; startY: icon.height * 0.1
            PathLine { x: icon.width * 0.9; y: icon.height * 0.5 }
            PathLine { x: icon.width * 0.22; y: icon.height * 0.9 }
            PathLine { x: icon.width * 0.22; y: icon.height * 0.1 }
        }
    }

    Row {
        anchors.centerIn: parent
        visible: icon.kind === "pause"
        spacing: icon.width * 0.18
        Rectangle { width: icon.width * 0.24; height: icon.height * 0.8; radius: 1; color: icon.color }
        Rectangle { width: icon.width * 0.24; height: icon.height * 0.8; radius: 1; color: icon.color }
    }

    Item {
        anchors.fill: parent
        visible: icon.kind === "previous" || icon.kind === "next"
        // Drawn as "next" and mirrored for "previous".
        transform: Scale { origin.x: icon.width / 2; xScale: icon.kind === "previous" ? -1 : 1 }
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                fillColor: icon.color
                strokeColor: "transparent"
                startX: icon.width * 0.15; startY: icon.height * 0.15
                PathLine { x: icon.width * 0.7; y: icon.height * 0.5 }
                PathLine { x: icon.width * 0.15; y: icon.height * 0.85 }
                PathLine { x: icon.width * 0.15; y: icon.height * 0.15 }
            }
        }
        Rectangle {
            x: icon.width * 0.72
            y: icon.height * 0.15
            width: icon.width * 0.13
            height: icon.height * 0.7
            radius: 1
            color: icon.color
        }
    }
}
