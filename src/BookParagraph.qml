import QtQuick
import Omareader

// One paragraph of the chapter. The sentence being read and the word being
// spoken are marked in place, by a glass window over the word or by colour;
// clicking a word reads aloud from there, and right-clicking it looks it up.
// In focus mode the paragraphs around the voice dim, and what was read fades.
//
// The reader's highlights are tinted with the accent, those with a note
// underlined, and a bookmark shows as a ribbon in the margin. Dragging over
// the text selects it, across paragraphs; the chapter's list keeps the
// selection, since it can run past this paragraph.
Item {
    id: root
    objectName: "bookParagraph"
    required property int index
    required property var modelData

    readonly property int level: modelData.level
    readonly property bool narrating: narrator.active && narrator.chapter === reader.chapter
    readonly property bool reading: narrating && narrator.paragraph === root.index
    readonly property bool focusing: narrating && reader.focusMode
    readonly property bool fading: focusing && reader.fadeRead
    readonly property bool spotlit: focusing && reader.spotlight !== "off"
    readonly property bool glassy: reader.highlight === "glass"
    // The text and its highlight, under names the glass can reach (its own
    // are body and highlight too).
    readonly property TextEdit page: body
    readonly property ReadingHighlight pageLayout: highlight
    // How much of the text still shows: read text fades further than the
    // text still to come, so the eye is drawn forward.
    readonly property real readOpacity: 0.22
    readonly property real dimOpacity: 0.38
    // The chapter's list, which keeps the selection and is told about clicks
    // on a highlight.
    readonly property var view: ListView.view
    // This paragraph's highlights and bookmark.
    readonly property var annotated: annotations.chapterMarks[root.index] || ({})
    readonly property bool selected: !!root.view && root.view.hasSelection
                                     && root.index >= root.view.selectionStartParagraph
                                     && root.index <= root.view.selectionEndParagraph

    opacity: root.reading ? 1
           : root.fading && root.index < narrator.paragraph ? root.readOpacity
           : root.spotlit ? root.dimOpacity : 1
    Behavior on opacity { NumberAnimation { duration: 240; easing.type: Easing.OutQuad } }

    width: ListView.view ? ListView.view.width : 0
    height: body.height + (level > 0 ? reader.fontSize * 1.4 : reader.fontSize * 0.8)

    // Where a character sits, in this item's coordinates, so the view can
    // keep the spoken word on screen.
    function rectFor(position) {
        const r = body.positionToRectangle(Math.max(0, position))
        return Qt.rect(body.x + r.x, body.y + r.y, r.width, r.height)
    }

    // The character at a point in this item's coordinates.
    function positionAt(x, y) {
        return body.positionAt(x - body.x, y - body.y)
    }

    TextEdit {
        id: body
        x: (root.width - width) / 2
        y: root.level > 0 ? reader.fontSize * 0.9 : 0
        width: Math.min(reader.contentWidth, root.width - 64)
        text: root.modelData.text
        textFormat: TextEdit.PlainText
        wrapMode: TextEdit.Wrap
        readOnly: true
        selectByMouse: false
        activeFocusOnPress: false
        horizontalAlignment: root.level > 0 ? Text.AlignLeft : Text.AlignJustify
        color: root.level > 0 ? Colors.text : Colors.body
        font.family: reader.serif ? "serif" : "sans-serif"
        font.pixelSize: root.level === 1 ? Math.round(reader.fontSize * 1.6)
                      : root.level === 2 ? Math.round(reader.fontSize * 1.4)
                      : root.level > 2 ? Math.round(reader.fontSize * 1.15)
                      : reader.fontSize
        font.weight: root.level > 0 ? Font.DemiBold : Font.Normal
    }

    ReadingHighlight {
        id: highlight
        document: body.textDocument
        styles: root.modelData.styles || []
        lineHeight: reader.lineHeight
        font: body.font
        sentenceStart: root.reading ? narrator.sentenceStart : -1
        sentenceEnd: root.reading ? narrator.sentenceEnd : -1
        wordStart: root.reading ? narrator.wordStart : -1
        wordEnd: root.reading ? narrator.wordEnd : -1
        readEnd: root.reading && root.fading
                 ? (narrator.wordStart >= 0 ? narrator.wordStart : narrator.sentenceStart) : -1
        readColor: Qt.alpha(body.color, root.readOpacity)
        // The glass marks the sentence by light alone: the rest of the
        // paragraph is a shade quieter than it.
        quietColor: !root.reading ? "transparent"
                    : root.spotlit && reader.spotlight === "sentence" ? Qt.alpha(body.color, root.dimOpacity)
                    : root.glassy ? Qt.alpha(body.color, 0.62) : "transparent"

        // The highlight marks the word, the sentence or both, as strongly as
        // the reader asked. A subtle word is coloured, not boxed. The glass
        // draws its own window instead.
        readonly property bool showSentence: !root.glassy && reader.highlight !== "word"
        readonly property bool showWord: !root.glassy && reader.highlight !== "sentence"
        readonly property bool subtle: reader.highlightStrength === "subtle"
        sentenceColor: !showSentence ? "transparent"
                     : Qt.alpha(theme.accent, subtle ? 0.07 : reader.highlightStrength === "strong" ? 0.28 : 0.14)
        wordColor: showWord && !subtle ? theme.accent : "transparent"
        wordTextColor: !showWord ? "transparent" : subtle ? theme.accent : theme.accentForeground

        marks: root.annotated.marks || []
        markColor: Qt.alpha(theme.accent, 0.3)
        noteColor: theme.accent
        selectionStart: !root.selected ? -1
                        : root.index === root.view.selectionStartParagraph ? root.view.selectionStart : 0
        selectionEnd: !root.selected ? -1
                      : root.index === root.view.selectionEndParagraph ? root.view.selectionEnd : root.modelData.text.length
        selectionColor: Qt.alpha(Colors.text, 0.24)
    }

    Loader {
        active: root.reading && root.glassy && narrator.wordStart >= 0
        sourceComponent: ReadingGlass {
            body: root.page
            layout: root.pageLayout
            styles: root.modelData.styles || []
            lineHeight: reader.lineHeight
            wordStart: narrator.wordStart
            wordEnd: narrator.wordEnd
            nextWordStart: narrator.nextWordStart
            nextWordEnd: narrator.nextWordEnd
            wordSeconds: narrator.wordSeconds
            playing: narrator.state === Narrator.Playing
        }
    }

    // A bookmark, in the margin beside the paragraph's first line, standing
    // on its baseline and about as tall as a capital letter. The baseline
    // comes from the laid out text, since a stretched line moves its
    // letters down.
    Loader {
        active: !!root.annotated.bookmark
        sourceComponent: BookmarkIcon {
            id: ribbon
            objectName: "paragraphBookmark"
            property real firstBaseline: body.font.pixelSize
            function measure() {
                const lines = highlight.lines()
                if (lines.length > 0)
                    ribbon.firstBaseline = lines[0].baseline
            }
            width: Math.round(height * 0.7)
            height: Math.round(body.font.pixelSize * 0.7)
            x: body.x - width - 14
            y: body.y + Math.round(ribbon.firstBaseline) - height
            color: theme.accent
            Component.onCompleted: Qt.callLater(measure)
            Connections {
                target: body
                function onContentHeightChanged() { Qt.callLater(ribbon.measure) }
                function onLineCountChanged() { Qt.callLater(ribbon.measure) }
            }
        }
    }

    // A click reads aloud from the word, or opens the highlight under it; a
    // right-click looks the word up; a drag selects. The drag is kept from the list, which would otherwise
    // take it for scrolling.
    MouseArea {
        anchors.fill: body
        cursorShape: Qt.PointingHandCursor
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        preventStealing: true
        property point pressPoint
        property bool dragged: false
        onPressed: mouse => {
            pressPoint = Qt.point(mouse.x, mouse.y)
            dragged = false
            if (root.view && mouse.button === Qt.LeftButton)
                root.view.beginSelection(root.index, body.positionAt(mouse.x, mouse.y))
        }
        onPositionChanged: mouse => {
            if (!root.view || !(mouse.buttons & Qt.LeftButton)
                    || (!dragged && Math.abs(mouse.x - pressPoint.x) + Math.abs(mouse.y - pressPoint.y) < 6))
                return
            dragged = true
            const p = mapToItem(root.view, mouse.x, mouse.y)
            root.view.dragSelection(p.x, p.y)
        }
        onReleased: {
            if (dragged && root.view)
                root.view.finishSelection()
        }
        onCanceled: {
            if (root.view)
                root.view.finishSelection()
        }
        onClicked: mouse => {
            if (dragged)
                return
            const position = body.positionAt(mouse.x, mouse.y)
            if (mouse.button === Qt.RightButton) {
                if (root.view)
                    root.view.lookUpRequested(root, position)
                return
            }
            const highlight = annotations.highlightAt(root.index, position)
            if (highlight !== "" && root.view)
                root.view.highlightClicked(highlight)
            else
                reader.readFrom(root.index, position)
        }
    }
}
