import QtQuick
import Omareader

// One paragraph of the chapter. The sentence being read and the word being
// spoken are coloured in place; clicking a word reads aloud from there.
Item {
    id: root
    required property int index
    required property var modelData

    readonly property int level: modelData.level
    readonly property bool reading: narrator.active && narrator.chapter === reader.chapter
                                    && narrator.paragraph === root.index

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
        document: body.textDocument
        lineHeight: reader.lineHeight
        sentenceStart: root.reading ? narrator.sentenceStart : -1
        sentenceEnd: root.reading ? narrator.sentenceEnd : -1
        wordStart: root.reading ? narrator.wordStart : -1
        wordEnd: root.reading ? narrator.wordEnd : -1
        sentenceColor: Qt.alpha(theme.accent, 0.14)
        wordColor: theme.accent
        wordTextColor: theme.accentForeground
    }

    MouseArea {
        anchors.fill: body
        cursorShape: Qt.PointingHandCursor
        onClicked: mouse => reader.readFrom(root.index, body.positionAt(mouse.x, mouse.y))
    }
}
