import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The books you've opened, most recent first.
FocusScope {
    id: root

    // Whether a dialog for adding text or a web page is up, which owns the
    // keys then.
    property bool dialogOpen: false
    signal addTextRequested(bool fromClipboard)
    // An empty address opens the dialog empty.
    signal addPageRequested(string address)

    Shortcut {
        sequence: "N"
        enabled: root.visible && !root.dialogOpen && !removeDialog.opened
        onActivated: root.addTextRequested(false)
    }
    Shortcut {
        sequence: "U"
        enabled: root.visible && !root.dialogOpen && !removeDialog.opened
        onActivated: root.addPageRequested("")
    }
    // A copied link means its page; any other text is pasted as it is.
    Shortcut {
        sequences: [StandardKey.Paste]
        enabled: root.visible && !root.dialogOpen && !removeDialog.opened
        onActivated: {
            const address = webImport.clipboardAddress()
            if (address !== "")
                root.addPageRequested(address)
            else
                root.addTextRequested(true)
        }
    }

    // Asks before a book leaves the library, since a click on the card's
    // button is easy to make by accident.
    function confirmRemove(book) {
        if (!book)
            return
        removeDialog.book = book
        removeDialog.open()
    }

    Keys.onPressed: event => {
        if (grid.count === 0)
            return
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.openCurrent()
            event.accepted = true
        } else if (event.key === Qt.Key_Delete) {
            root.confirmRemove(library.books[grid.currentIndex])
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
                objectName: "addPageButton"
                flat: true
                text: qsTr("Add web page")
                onClicked: root.addPageRequested("")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Add an article from its web address (U, or Ctrl+V with a link copied)")
            }
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

        // Read-aloud needs the chosen engine on this machine; say so up front.
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
                        text: narrator.engine === "qwen" ? "omareader setup --engine qwen" : "omareader setup"
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
                              : qsTr("%1 · %2%").arg(card.modelData.site !== "" ? card.modelData.site : card.modelData.format)
                                    .arg(Math.round(card.modelData.progress * 100).toLocaleString(Qt.locale(), 'f', 0))
                        elide: Text.ElideRight
                        color: Colors.muted
                        font.pixelSize: 12
                    }
                    Item { Layout.fillHeight: true }
                }

                HoverHandler { id: cardHover }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        grid.currentIndex = card.index
                        grid.forceActiveFocus()
                        reader.open(card.modelData.path)
                    }
                }

                // Removes the book; shown on the card under the pointer and
                // on the one the keyboard is on.
                Button {
                    id: removeButton
                    objectName: "removeBookButton"
                    x: 16 + 164 - width - 6
                    y: 22
                    width: 30
                    height: 30
                    padding: 0
                    topInset: 0
                    bottomInset: 0
                    leftInset: 0
                    rightInset: 0
                    focusPolicy: Qt.NoFocus
                    visible: cardHover.hovered || (grid.currentIndex === card.index && grid.activeFocus)
                    onClicked: {
                        grid.currentIndex = card.index
                        root.confirmRemove(card.modelData)
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Remove from the library (Delete)")
                    Accessible.name: qsTr("Remove from the library")
                    // The cross is drawn on the circle itself: Material lays a
                    // button's content out with paddings of its own, which
                    // pushed a contentItem off centre.
                    contentItem: Item {}
                    background: Rectangle {
                        radius: width / 2
                        color: removeButton.hovered ? theme.accent : Qt.alpha(Colors.background, 0.85)
                        border.color: removeButton.hovered ? theme.accent : Colors.line
                        // Two bars, so it looks the same in every font.
                        Repeater {
                            model: [45, -45]
                            Rectangle {
                                required property int modelData
                                anchors.centerIn: parent
                                width: 12
                                height: 2
                                radius: 1
                                rotation: modelData
                                antialiasing: true
                                color: removeButton.hovered ? theme.accentForeground : Colors.text
                            }
                        }
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
                    text: qsTr("Press O or drop a file here to open an EPUB, PDF, FB2, DOCX, ODT, Markdown, HTML or text book. Kindle formats open through Calibre. Press N to paste text, or U to add a web page.")
                    color: Colors.muted
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: grid.count > 0
            text: qsTr("Enter opens · N adds text · U adds a web page · Delete forgets a book (the file stays, pasted text goes to the trash) · ? shows every shortcut")
            color: Colors.muted
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }
    }

    Popup {
        id: removeDialog
        objectName: "removeDialog"
        property var book: null
        readonly property bool pasted: book ? book.pasted : false
        modal: true
        focus: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(440, (parent ? parent.width : 440) - 48)
        padding: 24
        background: Rectangle {
            color: Colors.surface
            radius: theme.radius
            border.color: Colors.line
        }

        function accept() {
            if (removeDialog.book)
                library.remove(removeDialog.book.path)
            close()
        }

        onClosed: {
            removeDialog.book = null
            grid.forceActiveFocus()
        }

        contentItem: ColumnLayout {
            spacing: 12
            focus: true
            Keys.onReturnPressed: removeDialog.accept()
            Keys.onEnterPressed: removeDialog.accept()
            Keys.onEscapePressed: removeDialog.close()

            Label {
                Layout.fillWidth: true
                text: removeDialog.pasted ? qsTr("Delete this text?") : qsTr("Remove this book?")
                font.pixelSize: 20
                font.family: "serif"
                color: Colors.text
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: !removeDialog.book ? ""
                      : removeDialog.pasted
                      ? qsTr("“%1” leaves the library and goes to the trash, where you can still restore it.").arg(removeDialog.book.title)
                      : qsTr("“%1” leaves the library. The file itself stays where it is.").arg(removeDialog.book.title)
                color: Colors.muted
                wrapMode: Text.Wrap
                font.pixelSize: 14
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 8
                spacing: 8
                Item { Layout.fillWidth: true }
                Button {
                    flat: true
                    text: qsTr("Cancel")
                    onClicked: removeDialog.close()
                }
                AccentButton {
                    objectName: "confirmRemoveButton"
                    text: removeDialog.pasted ? qsTr("Delete") : qsTr("Remove")
                    onClicked: removeDialog.accept()
                }
            }
        }
    }
}
