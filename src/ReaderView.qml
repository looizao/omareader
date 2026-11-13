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
        id: pointer
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
        card.close()
        contents.close()
        settings.close()
    }

    // Looks up the word at a character of a paragraph on screen, in a card
    // beside it. The voice pauses while the card is open and goes on when
    // it closes.
    function lookUpAt(paragraph, position) {
        const word = lookup.wordAt(paragraph.modelData.text, position)
        if (word.text === undefined)
            return
        const start = paragraph.rectFor(word.start)
        const end = paragraph.rectFor(word.start + word.length)
        const at = paragraph.mapToItem(page, start.x, start.y)
        card.wordRect = Qt.rect(at.x, at.y, end.y === start.y ? Math.max(4, end.x - start.x) : 16, start.height)
        if (narrator.state === Narrator.Playing || narrator.state === Narrator.Preparing) {
            narrator.pause()
            card.resumeOnClose = true
        }
        resume.stop()
        scroll.stop()
        card.openedAt = list.contentY
        lookup.lookUp(word.text)
        card.open()
    }

    // D: the word being read, or else the word under the mouse.
    function lookUpWord() {
        if (card.opened) {
            card.close()
            return
        }
        if (narrator.active && narrator.chapter === reader.chapter && narrator.paragraph >= 0
                && narrator.wordStart >= 0) {
            const spoken = list.itemAtIndex(narrator.paragraph)
            if (spoken) {
                root.lookUpAt(spoken, narrator.wordStart)
                return
            }
        }
        if (!pointer.hovered)
            return
        const p = list.mapFromItem(null, pointer.last.x, pointer.last.y)
        const item = list.itemAtIndex(list.indexAt(p.x + list.contentX, p.y + list.contentY))
        if (!item)
            return
        const local = item.mapFromItem(null, pointer.last.x, pointer.last.y)
        root.lookUpAt(item, item.positionAt(local.x, local.y))
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
            // Pausing or playing by hand while the card is open settles it.
            if (card.opened && narrator.state !== Narrator.Paused)
                card.resumeOnClose = false
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
        function onBookChanged() {
            card.close()
        }
    }

    Connections {
        target: reader
        function onChapterChanged() {
            card.close()
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
    Shortcut { sequence: "D"; enabled: root.keysEnabled; onActivated: root.lookUpWord() }
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
            id: page
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                objectName: "chapterList"
                // A paragraph asks for the word at a character to be looked up.
                signal lookUpRequested(Item paragraph, int position)
                onLookUpRequested: (paragraph, position) => root.lookUpAt(paragraph, position)
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
                onContentYChanged: {
                    visibleTimer.restart()
                    // The card belongs beside its word, so scrolling closes it.
                    if (card.opened && Math.abs(list.contentY - card.openedAt) > 2)
                        card.close()
                }

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

            // The word being looked up, outlined while its card is open.
            Rectangle {
                objectName: "lookedUpWord"
                visible: card.opened
                x: card.wordRect.x - 3
                y: card.wordRect.y - 1
                width: card.wordRect.width + 6
                height: card.wordRect.height + 2
                radius: 4
                color: Qt.alpha(theme.accent, 0.16)
                border.color: theme.accent
                border.width: 1.5
            }

            WordCard {
                id: card
                parent: page
                // Where the list was when the card opened, and whether the
                // voice was paused for it.
                property real openedAt: 0
                property bool resumeOnClose: false
                onClosed: {
                    lookup.clear()
                    // A right-click on another word closes the card on the
                    // press and opens it again on the release; wait for that
                    // before the voice goes on.
                    if (resumeOnClose)
                        resume.restart()
                }
            }

            Timer {
                id: resume
                interval: 250
                onTriggered: {
                    card.resumeOnClose = false
                    if (narrator.state === Narrator.Paused)
                        narrator.resume()
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
