import QtQuick
import QtQuick.Controls

// A book's cover, or a typeset stand-in when the book has none.
Rectangle {
    id: cover
    property string source: ""
    property string title: ""
    property string author: ""

    radius: Math.min(4, theme.radius)
    color: Colors.raised
    border.color: Colors.line
    clip: true

    Image {
        id: image
        anchors.fill: parent
        source: cover.source
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        sourceSize.height: 480
        visible: status === Image.Ready
    }

    Column {
        visible: image.status !== Image.Ready
        anchors.fill: parent
        anchors.margins: 14
        spacing: 8
        Rectangle { width: 28; height: 3; color: theme.accent }
        Label {
            width: parent.width
            text: cover.title
            wrapMode: Text.Wrap
            maximumLineCount: 5
            elide: Text.ElideRight
            font.family: "serif"
            font.pixelSize: 17
            color: Colors.text
        }
        Label {
            width: parent.width
            text: cover.author
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            font.pixelSize: 12
            color: Colors.muted
        }
    }
}
