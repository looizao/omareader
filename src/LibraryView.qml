import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The books you've opened, most recent first.
FocusScope {
    id: root

    // Whether the dialog for pasted text is up, which owns the keys then.
    property bool pasteOpen: false
    signal addTextRequested(bool fromClipboard)

    Shortcut {
        sequence: "N"
        enabled: root.visible && !root.pasteOpen
        onActivated: root.addTextRequested(false)
    }
    Shortcut {
        sequence: StandardKey.Paste
        enabled: root.visible && !root.pasteOpen
        onActivated: root.addTextRequested(true)
    }

    Keys.onPressed: event => {
        if (grid.count === 0)
            return
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.openCurrent()
            event.accepted = true
        } else if (event.key === Qt.Key_Delete) {
            const book = library.books[grid.currentIndex]
            if (book)
                library.remove(book.path)
            event.accepted = true
        }
    }

    function openCurrent() {
        const book = library.books[grid.currentIndex]
        if (book)
            reader.open(book.path)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 32
        spacing: 24

        RowLayout {
            Layout.fillWidth: true
            spacing: 16
            Label {
                text: "omareader"
                font.family: "serif"
                font.pixelSize: 30
                color: Colors.text
            }
            Rectangle { width: 8; height: 8; radius: 4; color: theme.accent; Layout.alignment: Qt.AlignBaseline }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "addTextButton"
                flat: true
                text: qsTr("Add text")
                onClicked: root.addTextRequested(false)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Paste text to read aloud (N, or Ctrl+V to paste right away)")
            }
            AccentButton {
                text: qsTr("Open book")
                onClicked: reader.openDialog()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Open a book (O)")
            }
        }

        // Read-aloud needs Kokoros on this machine; say so up front.
        Rectangle {
            Layout.fillWidth: true
            visible: !narrator.available
            implicitHeight: setupColumn.implicitHeight + 28
            radius: theme.radius
            color: Colors.surface
            border.color: Colors.line
            ColumnLayout {
                id: setupColumn
                anchors.fill: parent
                anchors.margins: 14
                spacing: 6
                Label {
                    Layout.fillWidth: true
                    text: narrator.problem
                    wrapMode: Text.Wrap
                    color: Colors.text
                }
                RowLayout {
                    spacing: 12
                    Label {
                        text: "omareader setup"
                        font.family: "monospace"
                        color: theme.accent
                    }
                    Button {
                        text: qsTr("Check again")
                        flat: true
                        onClicked: narrator.refreshSetup()
                    }
                }
            }
        }

        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: count > 0
            clip: true
            focus: true
            model: library.books
            cellWidth: 196
            cellHeight: 344
            keyNavigationEnabled: true
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: card
                required property var modelData
                required property int index
                width: grid.cellWidth
                height: grid.cellHeight
                opacity: modelData.missing ? 0.45 : 1

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 6
                    radius: theme.radius
                    color: grid.currentIndex === card.index && grid.activeFocus ? Colors.raised : "transparent"
                    border.color: grid.currentIndex === card.index && grid.activeFocus ? theme.accent : "transparent"
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 8

                    BookCover {
                        Layout.preferredWidth: 164
                        Layout.preferredHeight: 240
                        source: card.modelData.cover
                        title: card.modelData.title
                        author: card.modelData.author
                    }
                    Rectangle {
                        Layout.preferredWidth: 164
                        Layout.preferredHeight: 3
                        radius: 1
                        color: Colors.line
                        Rectangle {
                            width: parent.width * card.modelData.progress
                            height: parent.height
                            radius: 1
                            color: theme.accent
                        }
                    }
                    Label {
                        Layout.preferredWidth: 164
                        text: card.modelData.title
                        elide: Text.ElideRight
                        color: Colors.text
                        font.pixelSize: 14
                    }
                    Label {
                        Layout.preferredWidth: 164
                        text: card.modelData.missing
                              ? qsTr("File not found")
                              : qsTr("%1 · %2%").arg(card.modelData.format)
                                    .arg(Math.round(card.modelData.progress * 100).toLocaleString(Qt.locale(), 'f', 0))
                        elide: Text.ElideRight
                        color: Colors.muted
                        font.pixelSize: 12
                    }
                    Item { Layout.fillHeight: true }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        grid.currentIndex = card.index
                        grid.forceActiveFocus()
                        reader.open(card.modelData.path)
                    }
                }
            }
        }

        Item {
            visible: grid.count === 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            Column {
                anchors.centerIn: parent
                width: Math.min(520, parent.width)
                spacing: 12
                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("No books yet")
                    font.family: "serif"
                    font.pixelSize: 26
                    color: Colors.text
                }
                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: qsTr("Press O or drop a file here to open an EPUB, PDF, FB2, DOCX, ODT, Markdown, HTML or text book. Kindle formats open through Calibre. Press N to paste text instead.")
                    color: Colors.muted
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: grid.count > 0
            text: qsTr("Enter opens · N adds text · Delete forgets a book (the file stays, pasted text goes to the trash) · ? shows every shortcut")
            color: Colors.muted
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }
    }
}
