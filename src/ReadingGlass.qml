import QtQuick
import Omareader

// The glass window highlight: a pane about three words wide that rests on
// the line over the word being spoken and glides along as the voice reads,
// showing the words under it bright and slightly enlarged.
//
// The paragraph's lines are treated as one track laid end to end, so at the
// end of a line the pane slides off it while the same share slides in at the
// start of the next, instead of jumping down. It follows the word on a
// critically damped spring, which evens out the uneven pace of speech.
Item {
    id: glass
    objectName: "readingGlass"

    // The paragraph's text, which the glass sits on; it takes its size and place.
    required property TextEdit body
    property var styles: []
    property real lineHeight: 1
    // The word being spoken, as positions in body's text.
    property int wordStart: -1
    property int wordEnd: -1

    readonly property real gap: 16
    readonly property real padX: Math.round(body.font.pixelSize * 0.5)
    readonly property real padY: 3
    readonly property real margin: 24
    readonly property real magnify: 1.07
    // About three ordinary words wide; only a longer word stretches it.
    readonly property real baseWidth: Math.ceil(sample.advanceWidth) + 2 * padX
    readonly property real stride: body.width + 2 * gap

    // The track's layout: the first line's top, how far apart lines are and
    // how tall a line's letters are.
    property real firstTop: 0
    property real pitch: 1
    property real letterHeight: 0

    // Where the pane is on the track (its centre) and how wide, and where
    // the spoken word wants it.
    property real u: 0
    property real paneWidth: baseWidth
    property real targetU: 0
    property real targetWidth: baseWidth
    property real speedU: 0
    property real speedWidth: 0
    property bool placed: false
    readonly property bool settled: placed && Math.abs(targetU - u) < 0.5 && Math.abs(targetWidth - paneWidth) < 0.5
                                    && Math.abs(speedU) < 1 && Math.abs(speedWidth) < 1

    x: body.x
    y: body.y
    width: body.width
    height: body.height

    TextMetrics {
        id: sample
        font: glass.body.font
        text: "three plain words"
    }

    function lineTop(k) {
        return firstTop + k * pitch
    }

    function aim() {
        if (wordStart < 0 || body.width <= 0) {
            placed = false
            return
        }
        const first = body.positionToRectangle(0)
        const last = body.positionToRectangle(body.length)
        firstTop = first.y
        letterHeight = first.height
        pitch = body.lineCount > 1 ? (last.y - first.y) / (body.lineCount - 1) : first.height
        const start = body.positionToRectangle(wordStart)
        const end = body.positionToRectangle(Math.max(wordStart, wordEnd))
        const right = Math.abs(end.y - start.y) < 1 ? end.x : body.width
        const k = Math.max(0, Math.round((start.y - first.y) / pitch))
        targetU = k * stride + gap + (start.x + right) / 2
        targetWidth = Math.max(baseWidth, right - start.x + 2 * padX + body.font.pixelSize * 0.5)
        // A first word, or one more than a line away (a skip, a click), is
        // a new place to start from, not somewhere to glide to.
        if (!placed || Math.abs(targetU - u) > stride * 1.5) {
            u = targetU
            paneWidth = targetWidth
            speedU = speedWidth = 0
            placed = true
        }
    }

    onWordStartChanged: aim()
    onWordEndChanged: aim()
    onWidthChanged: { placed = false; aim() }
    onBaseWidthChanged: aim()
    Connections {
        target: glass.body
        function onLineCountChanged() { glass.placed = false; glass.aim() }
        function onContentHeightChanged() { glass.placed = false; glass.aim() }
    }
    Component.onCompleted: aim()

    FrameAnimation {
        running: glass.visible && glass.placed && !glass.settled
        onTriggered: {
            const dt = Math.min(0.05, frameTime)
            // Stiffness: higher follows the voice more tightly, lower is calmer.
            const w0 = 7.5
            glass.speedU += (w0 * w0 * (glass.targetU - glass.u) - 2 * w0 * glass.speedU) * dt
            glass.u += glass.speedU * dt
            glass.speedWidth += (w0 * w0 * (glass.targetWidth - glass.paneWidth) - 2 * w0 * glass.speedWidth) * dt
            glass.paneWidth += glass.speedWidth * dt
            if (Math.abs(glass.targetU - glass.u) < 0.5 && Math.abs(glass.speedU) < 1
                    && Math.abs(glass.targetWidth - glass.paneWidth) < 0.5 && Math.abs(glass.speedWidth) < 1) {
                glass.u = glass.targetU
                glass.paneWidth = glass.targetWidth
                glass.speedU = glass.speedWidth = 0
            }
        }
    }

    // One strip per line the pane touches; the pane is narrower than a
    // line, so it touches two at most. Each strip clips the pane at its
    // line's ends.
    Repeater {
        model: 2
        delegate: Item {
            id: strip
            required property int index
            readonly property int line: Math.floor((glass.u - glass.paneWidth / 2) / glass.stride) + index
            // The pane's left edge in this strip, and how much of it is on this line.
            readonly property real paneX: glass.u - glass.paneWidth / 2 - line * glass.stride
            readonly property real shown: Math.max(0, Math.min(paneX + glass.paneWidth, glass.stride) - Math.max(paneX, 0))
            readonly property real paneHeight: glass.letterHeight + 2 * glass.padY

            visible: glass.placed && line >= 0 && line < glass.body.lineCount && shown >= 1
            // It fades as it leaves a line, and grows clearer as it arrives.
            opacity: Math.min(1, 1.5 * shown / glass.paneWidth)
            x: -glass.gap
            y: glass.lineTop(line) - glass.padY - glass.margin
            width: glass.stride
            height: paneHeight + 2 * glass.margin
            clip: true

            // A soft shadow lifts the pane off the page.
            Repeater {
                model: [{ grow: 8, drop: 7, alpha: 0.10 }, { grow: 4, drop: 4, alpha: 0.16 }, { grow: 1, drop: 1, alpha: 0.30 }]
                delegate: Rectangle {
                    required property var modelData
                    x: strip.paneX - modelData.grow
                    y: glass.margin - modelData.grow + modelData.drop
                    width: glass.paneWidth + 2 * modelData.grow
                    height: strip.paneHeight + 2 * modelData.grow
                    radius: 9 + modelData.grow
                    color: Qt.rgba(0, 0, 0, modelData.alpha)
                }
            }

            Rectangle {
                objectName: "readingGlassPane"
                x: strip.paneX
                y: glass.margin
                width: glass.paneWidth
                height: strip.paneHeight
                radius: 9
                clip: true
                // Opaque, so only the enlarged words show through it, not
                // the page's own as well.
                color: Qt.tint(Colors.surface, Qt.alpha(theme.accent, 0.08))
                border.width: 1
                border.color: Qt.rgba(0.75, 0.8, 1, 0.2)

                // The words under the pane: the paragraph again, in the
                // brightest text colour, laid out exactly as the page's and
                // enlarged about the pane's centre.
                TextEdit {
                    id: copy
                    readonly property real originX: strip.paneX - glass.gap
                    readonly property real originY: glass.lineTop(strip.line) - glass.padY
                    x: -originX
                    y: -originY
                    width: glass.body.width
                    text: glass.body.text
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.Wrap
                    horizontalAlignment: glass.body.horizontalAlignment
                    font: glass.body.font
                    color: Colors.text
                    readOnly: true
                    enabled: false
                    transform: Scale {
                        origin.x: strip.paneX - glass.gap + glass.paneWidth / 2
                        origin.y: glass.lineTop(strip.line) + glass.letterHeight / 2
                        xScale: glass.magnify
                        yScale: glass.magnify
                    }

                    ReadingHighlight {
                        document: copy.textDocument
                        styles: glass.styles
                        lineHeight: glass.lineHeight
                    }
                }

                // Light catching the glass: a sheen down from the top and a
                // brighter rim along the top edge.
                Rectangle {
                    anchors.fill: parent
                    radius: parent.radius
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 0.12) }
                        GradientStop { position: 0.45; color: Qt.rgba(1, 1, 1, 0.03) }
                        GradientStop { position: 0.5; color: Qt.rgba(1, 1, 1, 0) }
                        GradientStop { position: 1.0; color: Qt.rgba(1, 1, 1, 0.02) }
                    }
                }
                Rectangle {
                    x: parent.radius
                    width: parent.width - 2 * parent.radius
                    height: 1
                    color: Qt.rgba(0.9, 0.93, 1, 0.4)
                }
            }
        }
    }
}
