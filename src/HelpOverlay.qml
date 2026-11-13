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

    // Side by side when the window is wide enough, the reader's keys on the
    // left; in a narrow window one list that scrolls if it has to.
    readonly property bool twoColumns: width >= 980
    readonly property int half: Math.ceil(shortcuts.length / 2)

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: Math.max(height, content.implicitHeight + 48)
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        MouseArea {
            width: flick.contentWidth
            height: flick.contentHeight
            onClicked: overlay.closed()
        }

        ColumnLayout {
            id: content
            x: (flick.width - width) / 2
            y: Math.max(24, (flick.height - implicitHeight) / 2)
            width: Math.min(overlay.twoColumns ? 1040 : 560, flick.width - 48)
            spacing: 16

            Label {
                text: qsTr("Keyboard")
                font.pixelSize: 24
                font.family: "serif"
                color: Colors.text
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 48
                Repeater {
                    model: overlay.twoColumns
                           ? [overlay.shortcuts.slice(0, overlay.half), overlay.shortcuts.slice(overlay.half)]
                           : [overlay.shortcuts]
                    GridLayout {
                        id: table
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.alignment: Qt.AlignTop
                        columns: 2
                        columnSpacing: 24
                        rowSpacing: 6
                        Repeater {
                            model: table.modelData.length * 2
                            Label {
                                required property int index
                                readonly property var row: table.modelData[Math.floor(index / 2)]
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
        }
    }
}
