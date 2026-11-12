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
    // Focus mode holds the spoken line at one height and scrolls the text
    // under it, instead of jumping when the voice nears the edge.
    readonly property bool steady: reader.focusMode && reader.steadyLine
    readonly property real readingLine: 0.33
    // In focus mode the bars step out of sight while the voice plays, and
    // come back for a while on any mouse movement or key.
    property bool awake: true
    readonly property bool controlsHidden: reader.focusMode && reader.hideControls && !root.awake
                                           && narrator.state === Narrator.Playing
                                           && !contents.opened && !settings.opened

    function wake() {
        root.awake = true
        idle.restart()
    }

    Timer {
        id: idle
        interval: 2500
        onTriggered: root.awake = false
    }

    // Only a real movement counts: Qt Quick also sends hover events when the
    // text scrolls under a still mouse.
    HoverHandler {
        property point last
        onPointChanged: {
            const p = point.scenePosition
            if (Math.abs(p.x - last.x) + Math.abs(p.y - last.y) > 2) {
                last = p
                root.wake()
            }
        }
    }
    Keys.onShortcutOverride: event => {
        root.wake()
        event.accepted = false
    }
    Keys.onPressed: event => {
        root.wake()
        event.accepted = false
    }

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
        const maxY = list.originY + Math.max(0, list.contentHeight - list.height)
        if (root.steady) {
            // Every word asks; only a new line moves the text.
            const to = Math.max(list.originY, Math.min(maxY, y - list.height * root.readingLine))
            if (Math.abs((scroll.running ? scroll.to : list.contentY) - to) >= 1) {
                scroll.to = to
                scroll.restart()
            }
        } else if (y < top + list.height * 0.12 || y + r.height > top + list.height * 0.72) {
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
            if (narrator.state === Narrator.Playing)
                root.wake()
        }
    }

    Connections {
        target: reader
        function onFocusChanged() {
            root.wake()
            if (root.following)
                root.reveal()
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
    Shortcut { sequence: "Z"; enabled: root.keysEnabled; onActivated: reader.focusMode = !reader.focusMode }
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
            objectName: "titleBar"
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            color: Colors.background
            // Hidden bars keep their place, so the text never moves when they go.
            opacity: root.controlsHidden ? 0 : 1
            // Hidden, they take no hover, so no tooltip stays behind.
            enabled: !root.controlsHidden
            Behavior on opacity { NumberAnimation { duration: 300 } }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 4
                ToolButton {
                    text: "‹"
                    font.pixelSize: 22
                    onClicked: reader.closeBook()
                    ToolTip.visible: hovered && enabled
                    ToolTip.text: qsTr("Library (B)")
                }
                ToolButton {
                    text: "☰"
                    onClicked: contents.open()
                    ToolTip.visible: hovered && enabled
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
                    ToolTip.visible: hovered && enabled
                    ToolTip.text: qsTr("Export as audio (E)")
                }
                ToolButton {
                    objectName: "focusButton"
                    text: "◎"
                    font.pixelSize: 18
                    highlighted: reader.focusMode
                    onClicked: reader.focusMode = !reader.focusMode
                    ToolTip.visible: hovered && enabled
                    ToolTip.text: reader.focusMode ? qsTr("Focus mode is on (Z)") : qsTr("Focus mode (Z)")
                }
                ToolButton {
                    text: "Aa"
                    onClicked: settings.open()
                    ToolTip.visible: hovered && enabled
                    ToolTip.text: qsTr("Voice and appearance (A)")
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                objectName: "chapterList"
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
                    duration: root.steady ? 420 : 260
                    easing.type: root.steady ? Easing.InOutSine : Easing.OutCubic
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
            objectName: "narrationBar"
            Layout.fillWidth: true
            opacity: root.controlsHidden ? 0 : 1
            // Hidden, they take no hover, so no tooltip stays behind.
            enabled: !root.controlsHidden
            Behavior on opacity { NumberAnimation { duration: 300 } }
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
