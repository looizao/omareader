import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The table of contents, and the book's bookmarks, highlights and notes,
// with the folder they are kept in step through.
Drawer {
    id: panel
    objectName: "contentsPanel"
    edge: Qt.LeftEdge
    width: Math.min(420, parent ? parent.width * 0.8 : 420)
    Material.roundedScale: theme.radius
    background: Rectangle { color: Colors.surface }

    // 0 for the contents, 1 for the notes.
    property int tab: 0

    // Opens on a tab, or closes when it is already showing that one.
    function toggle(which) {
        if (panel.opened && panel.tab === which) {
            close()
            return
        }
        panel.tab = which
        if (panel.opened)
            panel.focusList()
        else
            open()
    }

    function focusList() {
        if (panel.tab === 0) {
            chapters.currentIndex = reader.chapter
            chapters.positionViewAtIndex(reader.chapter, ListView.Center)
            chapters.forceActiveFocus()
        } else {
            notes.forceActiveFocus()
        }
    }

    onOpened: focusList()

    // The reader's own T and N stay quiet while the drawer is open, as every
    // key outside a popup does, so the drawer answers them itself.
    Shortcut { sequence: "T"; enabled: panel.opened; onActivated: panel.toggle(0) }
    Shortcut { sequence: "N"; enabled: panel.opened; onActivated: panel.toggle(1) }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: reader.author !== "" ? qsTr("%1 by %2").arg(reader.title).arg(reader.author) : reader.title
            color: Colors.muted
            wrapMode: Text.Wrap
            font.pixelSize: 12
        }

        TabBar {
            id: tabs
            Layout.fillWidth: true
            currentIndex: panel.tab
            onCurrentIndexChanged: if (panel.tab !== currentIndex) { panel.tab = currentIndex; panel.focusList() }
            background: Rectangle { color: "transparent" }
            TabButton { text: qsTr("Contents (T)") }
            TabButton { objectName: "notesTab"; text: qsTr("Notes (N)") }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: panel.tab

            ListView {
                id: chapters
                clip: true
                model: reader.chapterTitles
                keyNavigationEnabled: true
                highlightMoveDuration: 0
                ScrollBar.vertical: ScrollBar {}

                Keys.onReturnPressed: { reader.goToChapter(currentIndex); panel.close() }
                Keys.onEnterPressed: { reader.goToChapter(currentIndex); panel.close() }

                delegate: ItemDelegate {
                    id: item
                    required property string modelData
                    required property int index
                    width: ListView.view.width
                    highlighted: ListView.isCurrentItem
                    contentItem: RowLayout {
                        spacing: 10
                        Rectangle {
                            Layout.preferredWidth: 3
                            Layout.fillHeight: true
                            color: item.index === reader.chapter ? theme.accent : "transparent"
                        }
                        Label {
                            Layout.fillWidth: true
                            text: item.modelData
                            elide: Text.ElideRight
                            color: item.index === reader.chapter ? Colors.text : Colors.body
                        }
                    }
                    onClicked: {
                        reader.goToChapter(index)
                        panel.close()
                    }
                }
            }

            ColumnLayout {
                spacing: 12

                ListView {
                    id: notes
                    objectName: "notesList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: annotations.items
                    keyNavigationEnabled: true
                    highlightMoveDuration: 0
                    spacing: 2
                    ScrollBar.vertical: ScrollBar {}

                    function goTo(entry) {
                        reader.goTo(entry.chapter, entry.paragraph, entry.start)
                        panel.close()
                    }

                    Keys.onReturnPressed: if (currentIndex >= 0) goTo(model[currentIndex])
                    Keys.onEnterPressed: if (currentIndex >= 0) goTo(model[currentIndex])
                    Keys.onDeletePressed: if (currentIndex >= 0) annotations.remove(model[currentIndex].id)

                    Label {
                        anchors.fill: parent
                        anchors.margins: 8
                        visible: notes.count === 0
                        text: qsTr("No bookmarks or highlights yet. Press M to bookmark your place, or drag over the text to highlight it and add a note.")
                        wrapMode: Text.Wrap
                        color: Colors.muted
                        font.pixelSize: 13
                    }

                    delegate: ItemDelegate {
                        id: note
                        required property var modelData
                        required property int index
                        readonly property bool bookmark: modelData.kind === "bookmark"
                        width: ListView.view.width
                        highlighted: ListView.isCurrentItem
                        topPadding: 10
                        bottomPadding: 10
                        rightPadding: 44
                        contentItem: RowLayout {
                            spacing: 10
                            Item {
                                Layout.preferredWidth: 10
                                Layout.fillHeight: true
                                BookmarkIcon {
                                    visible: note.bookmark
                                    y: 2
                                    width: 10
                                    height: 14
                                    color: theme.accent
                                }
                                Rectangle {
                                    visible: !note.bookmark
                                    x: 3
                                    width: 3
                                    height: parent.height
                                    radius: 1.5
                                    color: theme.accent
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                Label {
                                    Layout.fillWidth: true
                                    text: note.modelData.placed
                                          ? note.modelData.chapterTitle
                                          : qsTr("%1 · not found in this copy of the book").arg(note.modelData.chapterTitle)
                                    elide: Text.ElideRight
                                    color: Colors.muted
                                    font.pixelSize: 11
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: note.modelData.text
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    maximumLineCount: note.bookmark ? 2 : 4
                                    elide: Text.ElideRight
                                    font.family: reader.serif ? "serif" : "sans-serif"
                                    font.pixelSize: 14
                                    color: Colors.body
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: note.modelData.note !== ""
                                    text: note.modelData.note
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    maximumLineCount: 4
                                    elide: Text.ElideRight
                                    font.pixelSize: 13
                                    color: Colors.text
                                }
                            }
                        }
                        onClicked: notes.goTo(note.modelData)

                        ToolButton {
                            objectName: "removeAnnotation"
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: 2
                            text: "✕"
                            font.pixelSize: 12
                            onClicked: annotations.remove(note.modelData.id)
                            ToolTip.visible: hovered
                            ToolTip.text: note.bookmark ? qsTr("Remove the bookmark") : qsTr("Remove the highlight and its note")
                        }
                    }
                }

                // Where the notes are kept in step with other computers.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: Colors.line
                }
                Label {
                    Layout.fillWidth: true
                    text: annotations.syncFolder === ""
                          ? qsTr("Keep bookmarks and notes in step across your computers through a folder you sync, with Syncthing or Nextcloud, say. omareader itself never goes online for it.")
                          : annotations.syncedDevices > 0
                            ? qsTr("In step through %1 with %n other computer(s).", "", annotations.syncedDevices).arg(annotations.syncFolder)
                            : qsTr("In step through %1. No other computer has synced this book yet.").arg(annotations.syncFolder)
                    wrapMode: Text.Wrap
                    color: Colors.muted
                    font.pixelSize: 12
                }
                RowLayout {
                    spacing: 4
                    Button {
                        objectName: "chooseSyncFolder"
                        flat: true
                        text: annotations.syncFolder === "" ? qsTr("Choose a sync folder…") : qsTr("Change…")
                        onClicked: annotations.chooseSyncFolder()
                    }
                    Button {
                        visible: annotations.syncFolder !== ""
                        flat: true
                        text: qsTr("Stop syncing")
                        onClicked: annotations.stopSyncing()
                    }
                }
            }
        }
    }
}
