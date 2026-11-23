import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The books you've opened, most recent first unless sorted otherwise, and
// the collections they are kept in.
FocusScope {
    id: root

    // Whether a dialog for adding text or a web page is up, which owns the
    // keys then.
    property bool dialogOpen: false
    // Whether the library's own dialogs leave its keys free.
    readonly property bool keysFree: root.visible && !root.dialogOpen && !removeDialog.opened
                                     && !picker.opened && !collectionDialog.opened && !collectionMenu.opened
    readonly property var sortOrders: [
        {id: "recent", label: qsTr("Recently opened")},
        {id: "title", label: qsTr("Title")},
        {id: "author", label: qsTr("Author")},
        {id: "progress", label: qsTr("Progress")}
    ]
    // Every book first, then each collection.
    readonly property var shelves: [{name: "", count: library.count}].concat(library.collections)
    signal addTextRequested(bool fromClipboard)
    // An empty address opens the dialog empty.
    signal addPageRequested(string address)
    signal statsRequested()

    Shortcut {
        sequence: "N"
        enabled: root.keysFree
        onActivated: root.addTextRequested(false)
    }
    Shortcut {
        sequence: "I"
        enabled: root.keysFree
        onActivated: root.statsRequested()
    }
    Shortcut {
        sequence: "U"
        enabled: root.keysFree
        onActivated: root.addPageRequested("")
    }
    // A copied link means its page; any other text is pasted as it is.
    Shortcut {
        sequences: [StandardKey.Paste]
        enabled: root.keysFree
        onActivated: {
            const address = webImport.clipboardAddress()
            if (address !== "")
                root.addPageRequested(address)
            else
                root.addTextRequested(true)
        }
    }

    Shortcut {
        sequences: ["/", StandardKey.Find]
        enabled: root.keysFree && library.count > 0
        onActivated: {
            filterField.forceActiveFocus()
            filterField.selectAll()
        }
    }
    Shortcut {
        sequence: "S"
        enabled: root.keysFree && library.count > 0
        onActivated: root.nextSortOrder()
    }
    Shortcut {
        sequence: "C"
        enabled: root.keysFree && grid.count > 0
        onActivated: picker.start(library.shelf[grid.currentIndex])
    }
    Shortcut {
        sequence: "["
        enabled: root.keysFree && library.count > 0
        onActivated: root.showShelf(-1)
    }
    Shortcut {
        sequence: "]"
        enabled: root.keysFree && library.count > 0
        onActivated: root.showShelf(1)
    }

    function nextSortOrder() {
        for (let i = 0; i < root.sortOrders.length; ++i)
            if (root.sortOrders[i].id === library.sortOrder) {
                library.sortOrder = root.sortOrders[(i + 1) % root.sortOrders.length].id
                return
            }
        library.sortOrder = "recent"
    }

    // Moves to the shelf before or after the one shown, round the ends.
    function showShelf(step) {
        let i = 0
        for (let j = 0; j < root.shelves.length; ++j)
            if (root.shelves[j].name === library.collection)
                i = j
        i = (i + step + root.shelves.length) % root.shelves.length
        library.collection = root.shelves[i].name
        shelfTabs.positionViewAtIndex(i, ListView.Contain)
        grid.currentIndex = 0
    }

    // Esc closes the library's own dialogs, which the window's Esc reaches
    // first; false when none is open.
    function closeDialogs() {
        for (const popup of [picker, collectionDialog, collectionMenu, removeDialog])
            if (popup.opened) {
                popup.close()
                return true
            }
        return false
    }

    // Esc empties the filter and goes back to the books; false when there
    // is nothing to undo.
    function clearFilter() {
        if (!root.keysFree || (filterField.text === "" && !filterField.activeFocus))
            return false
        filterField.clear()
        grid.forceActiveFocus()
        return true
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
            root.confirmRemove(library.shelf[grid.currentIndex])
            event.accepted = true
        }
    }

    function openCurrent() {
        const book = library.shelf[grid.currentIndex]
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
                objectName: "libraryStatsButton"
                flat: true
                text: qsTr("Statistics")
                onClicked: root.statsRequested()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Reading statistics (I)")
            }
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

        // The shelves, the filter and the order, once there are books.
        RowLayout {
            Layout.fillWidth: true
            visible: library.count > 0
            spacing: 16

            ListView {
                id: shelfTabs
                objectName: "shelfTabs"
                // As wide as its tabs, scrolling once they don't fit.
                Layout.fillWidth: true
                Layout.preferredWidth: contentWidth
                Layout.maximumWidth: contentWidth
                Layout.minimumWidth: Math.min(120, contentWidth)
                Layout.preferredHeight: 34
                orientation: ListView.Horizontal
                spacing: 6
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: root.shelves
                delegate: Rectangle {
                    id: tab
                    objectName: "shelfTab"
                    required property var modelData
                    required property int index
                    readonly property bool chosen: modelData.name === library.collection
                    width: tabLabel.implicitWidth + 28
                    height: 34
                    radius: Math.min(theme.radius, height / 2)
                    color: chosen ? theme.accent : tabHover.hovered ? Colors.raised : Colors.surface
                    border.color: chosen ? theme.accent : Colors.line
                    Label {
                        id: tabLabel
                        anchors.centerIn: parent
                        text: tab.modelData.name === ""
                              ? qsTr("All books · %1").arg(tab.modelData.count.toLocaleString(Qt.locale(), 'f', 0))
                              : qsTr("%1 · %2").arg(tab.modelData.name).arg(tab.modelData.count.toLocaleString(Qt.locale(), 'f', 0))
                        color: tab.chosen ? theme.accentForeground : Colors.body
                        font.pixelSize: 13
                    }
                    HoverHandler { id: tabHover }
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: mouse => {
                            library.collection = tab.modelData.name
                            grid.currentIndex = 0
                            grid.forceActiveFocus()
                            if (mouse.button === Qt.RightButton && tab.modelData.name !== "") {
                                collectionMenu.name = tab.modelData.name
                                collectionMenu.popup(tab, mouse.x, mouse.y)
                            }
                        }
                    }
                }
            }

            Button {
                id: newCollectionButton
                objectName: "newCollectionButton"
                Layout.preferredHeight: 34
                topInset: 0
                bottomInset: 0
                flat: true
                focusPolicy: Qt.NoFocus
                text: qsTr("New collection")
                onClicked: collectionDialog.startNew()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Right-click a collection to rename or delete it; [ and ] move between them")
            }
            Item { Layout.fillWidth: true }

            // Drawn to the tabs' height: Material's own field is much
            // taller, and its placeholder floats up out of a short box.
            TextField {
                id: filterField
                objectName: "libraryFilter"
                Layout.fillWidth: true
                Layout.preferredWidth: 240
                Layout.maximumWidth: 240
                Layout.minimumWidth: 120
                implicitHeight: 34
                topInset: 0
                bottomInset: 0
                topPadding: 0
                bottomPadding: 0
                leftPadding: 34
                rightPadding: 12
                verticalAlignment: TextInput.AlignVCenter
                font.pixelSize: 13
                color: Colors.text
                onTextChanged: library.filter = text
                Keys.onDownPressed: grid.forceActiveFocus()
                Keys.onReturnPressed: grid.forceActiveFocus()
                Keys.onEnterPressed: grid.forceActiveFocus()
                Accessible.name: qsTr("Filter by title or author")
                background: Rectangle {
                    radius: Math.min(theme.radius, height / 2)
                    color: Colors.surface
                    border.color: filterField.activeFocus ? theme.accent : Colors.line
                    SearchIcon {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        width: 13
                        height: 13
                        color: Colors.muted
                    }
                    Label {
                        x: filterField.leftPadding
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - x - filterField.rightPadding
                        visible: filterField.text === ""
                        text: qsTr("Filter by title or author (/)")
                        color: Colors.muted
                        font.pixelSize: 13
                        elide: Text.ElideRight
                    }
                }
            }

            Label {
                visible: root.width >= 900
                text: qsTr("Sort by")
                color: Colors.muted
                font.pixelSize: 13
            }
            ChoiceBox {
                id: sortBox
                objectName: "librarySort"
                Layout.preferredWidth: 190
                Layout.minimumWidth: 150
                implicitHeight: 34
                topInset: 0
                bottomInset: 0
                font.pixelSize: 13
                focusPolicy: Qt.NoFocus
                model: root.sortOrders
                selected: library.sortOrder
                onChosen: value => {
                    library.sortOrder = value
                    grid.forceActiveFocus()
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Change the order (S)")
                background: Rectangle {
                    radius: Math.min(theme.radius, height / 2)
                    color: sortBox.hovered ? Colors.raised : Colors.surface
                    border.color: sortBox.popup.visible ? theme.accent : Colors.line
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
            model: library.shelf
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
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: mouse => {
                        grid.currentIndex = card.index
                        grid.forceActiveFocus()
                        if (mouse.button === Qt.RightButton)
                            picker.start(card.modelData)
                        else
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

        // A collection with nothing in it yet, or a filter nothing matches.
        Item {
            visible: grid.count === 0 && library.count > 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            Column {
                anchors.centerIn: parent
                width: Math.min(520, parent.width)
                spacing: 12
                Label {
                    objectName: "emptyShelfTitle"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: library.filter.trim() !== "" ? qsTr("Nothing matches “%1”").arg(library.filter.trim())
                          : qsTr("Nothing in “%1” yet").arg(library.collection)
                    font.family: "serif"
                    font.pixelSize: 26
                    color: Colors.text
                }
                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: library.filter.trim() !== ""
                          ? (library.collection !== "" ? qsTr("Only “%1” is searched. Esc clears the filter.").arg(library.collection)
                                                       : qsTr("Esc clears the filter."))
                          : qsTr("Go to All books, then press C on a book, or right-click it, to put it here.")
                    color: Colors.muted
                }
            }
        }

        Item {
            visible: library.count === 0
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
            text: qsTr("Enter opens · C puts a book in collections · / filters · S sorts · Delete forgets a book (the file stays, pasted text goes to the trash) · ? shows every shortcut")
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

    CollectionPicker {
        id: picker
        onClosed: grid.forceActiveFocus()
    }

    CollectionDialog {
        id: collectionDialog
        onClosed: grid.forceActiveFocus()
    }

    // A collection's own actions, on a right-click of its tab.
    Menu {
        id: collectionMenu
        objectName: "collectionMenu"
        property string name: ""
        // Drawn here: the offscreen renderer drops Material's elevated
        // backgrounds, and this one matches the dialogs.
        background: Rectangle {
            implicitWidth: 200
            color: Colors.surface
            radius: theme.radius
            border.color: Colors.line
        }
        MenuItem {
            text: qsTr("Rename…")
            onTriggered: collectionDialog.startRename(collectionMenu.name)
        }
        MenuItem {
            text: qsTr("Delete collection")
            onTriggered: collectionDialog.startDelete(collectionMenu.name)
        }
    }
}
