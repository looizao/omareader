import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Every shortcut, on ?.
Rectangle {
    id: overlay
    objectName: "helpOverlay"
    signal closed()
    color: Qt.alpha(Colors.background, 0.92)

    readonly property var shortcuts: [
        ["Space", qsTr("Read aloud, pause, resume")],
        [qsTr("Click a word"), qsTr("Read aloud from there")],
        [qsTr("Right-click a word"), qsTr("Look it up in your dictionaries")],
        ["D", qsTr("Look up the word being read, or the one under the mouse")],
        [", / .", qsTr("Previous / next sentence")],
        ["[ / ]", qsTr("Slower / faster")],
        ["S", qsTr("Stop reading aloud")],
        ["F", qsTr("Back to the voice after scrolling")],
        ["Z", qsTr("Focus mode")],
        ["← / →", qsTr("Previous / next chapter")],
        ["↑ ↓ PgUp PgDn", qsTr("Scroll")],
        ["T", qsTr("Contents")],
        ["M", qsTr("Bookmark this place, or remove its bookmark")],
        [qsTr("Drag over text"), qsTr("Highlight it, add a note or copy it")],
        [qsTr("Click a highlight"), qsTr("Its note")],
        ["N", qsTr("Bookmarks, highlights and notes")],
        ["A", qsTr("Voice and appearance")],
        ["E", qsTr("Export as audio")],
        ["+ / −", qsTr("Bigger / smaller text")],
        ["O", qsTr("Open a book")],
        ["N", qsTr("Add pasted text to the library")],
        ["U", qsTr("Add a web page to the library")],
        ["Delete", qsTr("Remove the selected book from the library")],
        ["B", qsTr("Back to the library")],
        ["Esc", qsTr("Close panels and messages")],
        ["?", qsTr("Show or hide this help")],
        ["Q", qsTr("Quit")]
    ]

    MouseArea { anchors.fill: parent; onClicked: overlay.closed() }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(560, parent.width - 48)
        spacing: 16

        Label {
            text: qsTr("Keyboard")
            font.pixelSize: 24
            font.family: "serif"
            color: Colors.text
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 24
            rowSpacing: 6
            Repeater {
                model: overlay.shortcuts.length * 2
                Label {
                    required property int index
                    readonly property var row: overlay.shortcuts[Math.floor(index / 2)]
                    Layout.fillWidth: index % 2 === 1
                    text: index % 2 === 0 ? row[0] : row[1]
                    color: index % 2 === 0 ? theme.accent : Colors.body
                    font.family: index % 2 === 0 ? "monospace" : ""
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
