import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Puts one book in collections and takes it out of them, and makes a new
// collection for it. Opened with C or a right-click on the book's card.
Popup {
    id: picker
    objectName: "collectionPicker"
    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(400, (parent ? parent.width : 400) - 48)
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: Colors.surface
        radius: theme.radius
        border.color: Colors.line
    }

    // The book's path; its entry is looked up again whenever the library
    // changes, so the ticks follow what was just chosen.
    property string path: ""
    readonly property var book: {
        const books = library.books
        for (let i = 0; i < books.length; ++i)
            if (books[i].path === picker.path)
                return books[i]
        return null
    }

    function start(book) {
        if (!book)
            return
        picker.path = book.path
        picker.problem = ""
        nameField.clear()
        open()
    }

    property string problem: ""

    function toggle(name) {
        if (picker.book)
            picker.report(library.setInCollection(picker.path, name, picker.book.collections.indexOf(name) < 0))
    }

    function create() {
        if (nameField.text.trim() === "")
            return
        if (picker.report(library.setInCollection(picker.path, nameField.text, true)))
            nameField.clear()
    }

    function report(saved) {
        picker.problem = saved ? "" : qsTr("The library couldn't be saved, so nothing changed.")
        return saved
    }

    onOpened: {
        if (list.count > 0) {
            list.currentIndex = 0
            list.forceActiveFocus()
        } else {
            nameField.forceActiveFocus()
        }
    }
    onClosed: picker.path = ""

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: picker.book ? qsTr("Collections for “%1”").arg(picker.book.title) : ""
            textFormat: Text.PlainText
            font.pixelSize: 20
            font.family: "serif"
            color: Colors.text
            wrapMode: Text.Wrap
        }

        ListView {
            id: list
            objectName: "collectionChoices"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 280)
            visible: count > 0
            clip: true
            model: library.collections
            keyNavigationEnabled: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            KeyNavigation.tab: nameField
            Keys.onSpacePressed: picker.toggle(library.collections[currentIndex].name)
            Keys.onReturnPressed: picker.close()
            Keys.onEnterPressed: picker.close()
            delegate: CheckDelegate {
                id: choice
                objectName: "collectionChoice"
                required property var modelData
                required property int index
                width: ListView.view.width
                focusPolicy: Qt.NoFocus
                // The library decides and the tick follows it, so a click
                // never leaves the tick out of step.
                checkable: false
                text: modelData.name
                checked: picker.book !== null && picker.book.collections.indexOf(modelData.name) >= 0
                highlighted: list.activeFocus && list.currentIndex === index
                // Material's own label reads a name that looks like markup as
                // rich text, which can load images; names are plain text.
                contentItem: Label {
                    leftPadding: choice.mirrored ? choice.indicator.width + choice.spacing : 0
                    rightPadding: choice.mirrored ? 0 : choice.indicator.width + choice.spacing
                    text: choice.text
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                    color: Colors.text
                }
                onClicked: {
                    list.currentIndex = index
                    picker.toggle(modelData.name)
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: list.count === 0
            text: qsTr("No collections yet. Name one to start it with this book.")
            color: Colors.muted
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            TextField {
                id: nameField
                objectName: "newCollectionField"
                Layout.fillWidth: true
                placeholderText: qsTr("New collection")
                KeyNavigation.backtab: list
                onAccepted: picker.create()
                Keys.onUpPressed: event => {
                    if (list.count > 0)
                        list.forceActiveFocus()
                    else
                        event.accepted = false
                }
            }
            Button {
                flat: true
                text: qsTr("Add")
                enabled: nameField.text.trim() !== ""
                onClicked: picker.create()
            }
        }

        Label {
            Layout.fillWidth: true
            visible: picker.problem !== ""
            text: picker.problem
            color: theme.accent
            wrapMode: Text.Wrap
            font.pixelSize: 13
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            Label {
                Layout.fillWidth: true
                text: list.count > 0 ? qsTr("Space ticks a collection · Enter closes") : ""
                color: Colors.muted
                font.pixelSize: 12
                elide: Text.ElideRight
            }
            AccentButton {
                objectName: "collectionPickerDone"
                text: qsTr("Done")
                onClicked: picker.close()
            }
        }
    }
}
