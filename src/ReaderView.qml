import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omareader

// The open book: the chapter as one scrolling column, the narration controls
// along the bottom, contents and settings in side panels.
FocusScope {
    id: root

    // Main owns the export dialog, so its progress shows from the library too.
    signal exportRequested()

    // The view keeps the spoken word in sight until the user scrolls away.
    property bool following: true
    // Reading keys step aside while a panel has the keyboard.
    readonly property bool keysEnabled: visible && !contents.opened && !settings.opened

    function closePanels() {
        contents.close()
        settings.close()
    }

    function scrollBy(amount) {
        scroll.stop()
        const maxY = list.originY + Math.max(0, list.contentHeight - list.height)
        list.contentY = Math.max(list.originY, Math.min(maxY, list.contentY + amount))
        if (narrator.active)
            root.following = false
    }

    function reveal() {
        if (!narrator.active || narrator.chapter !== reader.chapter || narrator.paragraph < 0)
            return
        const item = list.itemAtIndex(narrator.paragraph)
        if (!item) {
            list.positionViewAtIndex(narrator.paragraph, ListView.Center)
            return
        }
        const position = narrator.wordStart >= 0 ? narrator.wordStart : narrator.sentenceStart
        const r = item.rectFor(position)
        const y = item.y + r.y
        const top = list.contentY
        if (y < top + list.height * 0.12 || y + r.height > top + list.height * 0.72) {
            const maxY = list.originY + Math.max(0, list.contentHeight - list.height)
            scroll.to = Math.max(list.originY, Math.min(maxY, y - list.height * 0.3))
            scroll.restart()
        }
    }

    function follow() {
        root.following = true
        root.reveal()
    }

    Connections {
        target: narrator
        function onWordChanged() {
            if (root.following)
                root.reveal()
        }
        function onStateChanged() {
            if (narrator.state === Narrator.Preparing)
                root.following = true
        }
    }

    Connections {
        target: reader
        function onChapterChanged() {
            Qt.callLater(() => {
                list.positionViewAtIndex(Math.max(0, reader.anchorParagraph), ListView.Beginning)
                if (root.following)
                    root.reveal()
            })
        }
    }

    Shortcut { sequence: "Space"; enabled: root.keysEnabled; onActivated: reader.togglePlay() }
    Shortcut { sequence: "Return"; enabled: root.keysEnabled; onActivated: reader.togglePlay() }
    Shortcut { sequences: ["Down", "J"]; enabled: root.keysEnabled; onActivated: root.scrollBy(80) }
    Shortcut { sequences: ["Up", "K"]; enabled: root.keysEnabled; onActivated: root.scrollBy(-80) }
    Shortcut { sequence: "PgDown"; enabled: root.keysEnabled; onActivated: root.scrollBy(list.height * 0.9) }
    Shortcut { sequence: "PgUp"; enabled: root.keysEnabled; onActivated: root.scrollBy(-list.height * 0.9) }
    Shortcut { sequence: "Home"; enabled: root.keysEnabled; onActivated: { root.following = !narrator.active; list.positionViewAtBeginning() } }
    Shortcut { sequence: "End"; enabled: root.keysEnabled; onActivated: { root.following = !narrator.active; list.positionViewAtEnd() } }
    Shortcut { sequences: ["Right", "L"]; enabled: root.keysEnabled; onActivated: reader.nextChapter() }
    Shortcut { sequences: ["Left", "H"]; enabled: root.keysEnabled; onActivated: reader.previousChapter() }
    Shortcut { sequence: "."; enabled: root.keysEnabled; onActivated: narrator.skip(1) }
    Shortcut { sequence: ","; enabled: root.keysEnabled; onActivated: narrator.skip(-1) }
    Shortcut { sequence: "]"; enabled: root.keysEnabled; onActivated: narrator.speed = narrator.speed + 0.1 }
    Shortcut { sequence: "["; enabled: root.keysEnabled; onActivated: narrator.speed = narrator.speed - 0.1 }
    Shortcut { sequences: ["=", "+", StandardKey.ZoomIn]; enabled: root.keysEnabled; onActivated: reader.fontSize = reader.fontSize + 1 }
    Shortcut { sequences: ["-", StandardKey.ZoomOut]; enabled: root.keysEnabled; onActivated: reader.fontSize = reader.fontSize - 1 }
    Shortcut { sequence: "F"; enabled: root.keysEnabled; onActivated: root.follow() }
    Shortcut { sequence: "S"; enabled: root.keysEnabled; onActivated: narrator.stop() }
    Shortcut { sequence: "T"; enabled: root.visible && !settings.opened; onActivated: contents.opened ? contents.close() : contents.open() }
    Shortcut { sequence: "Ctrl+,"; enabled: root.visible && !contents.opened; onActivated: settings.opened ? settings.close() : settings.open() }
    Shortcut { sequence: "A"; enabled: root.visible && !contents.opened; onActivated: settings.opened ? settings.close() : settings.open() }
    Shortcut { sequence: "E"; enabled: root.keysEnabled; onActivated: root.exportRequested() }
    Shortcut { sequences: ["Backspace", "B"]; enabled: root.keysEnabled; onActivated: { root.closePanels(); reader.closeBook() } }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Title bar: back to the library, the book and chapter, the panels.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            color: Colors.background
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 4
                ToolButton {
                    text: "‹"
                    font.pixelSize: 22
                    onClicked: reader.closeBook()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Library (B)")
                }
                ToolButton {
                    text: "☰"
                    onClicked: contents.open()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Contents (T)")
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: reader.chapterTitle !== "" && reader.chapterTitle !== reader.title
                          ? qsTr("%1 · %2").arg(reader.title).arg(reader.chapterTitle) : reader.title
                    elide: Text.ElideRight
                    color: Colors.muted
                    font.pixelSize: 13
                }
                ToolButton {
                    text: exporter.running ? qsTr("%1%").arg(Math.floor(exporter.progress * 100)) : "⤓"
                    font.pixelSize: exporter.running ? 12 : 18
                    onClicked: root.exportRequested()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Export as audio (E)")
                }
                ToolButton {
                    text: "Aa"
                    onClicked: settings.open()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Voice and appearance (A)")
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                anchors.fill: parent
                clip: true
                model: reader.paragraphs
                cacheBuffer: 2400
                boundsBehavior: Flickable.StopAtBounds
                header: Item { height: 32; width: 1 }
                footer: Item {
                    width: list.width
                    height: 160
                    Button {
                        anchors.centerIn: parent
                        visible: reader.chapter + 1 < reader.chapterCount
                        flat: true
                        text: qsTr("Next chapter →")
                        onClicked: reader.nextChapter()
                    }
                }
                delegate: BookParagraph {}
                ScrollBar.vertical: ScrollBar {}

                onMovementStarted: if (narrator.active) root.following = false
                onContentYChanged: visibleTimer.restart()

                NumberAnimation {
                    id: scroll
                    target: list
                    property: "contentY"
                    duration: 260
                    easing.type: Easing.OutCubic
                }
            }

            // The paragraph at the top is the reading position while nothing is read aloud.
            Timer {
                id: visibleTimer
                interval: 400
                onTriggered: {
                    const index = list.indexAt(list.width / 2, list.contentY + 40)
                    if (index >= 0)
                        reader.setVisibleParagraph(index)
                }
            }

            AccentButton {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 16
                visible: narrator.active && !root.following
                text: qsTr("Back to the voice (F)")
                onClicked: root.follow()
            }
        }

        NarrationBar {
            Layout.fillWidth: true
        }
    }

    ContentsPanel {
        id: contents
        height: root.height
    }

    SettingsPanel {
        id: settings
        objectName: "settingsPanel"
        height: root.height
    }
}
