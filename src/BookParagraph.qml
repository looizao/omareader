import QtQuick
import Omareader

// One paragraph of the chapter. The sentence being read and the word being
// spoken are marked in place, by a glass window over the word or by colour;
// clicking a word reads aloud from there. In focus mode the paragraphs
// around the voice dim, and what was read fades.
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

    MouseArea {
        anchors.fill: body
        cursorShape: Qt.PointingHandCursor
        onClicked: mouse => reader.readFrom(root.index, body.positionAt(mouse.x, mouse.y))
    }
}
