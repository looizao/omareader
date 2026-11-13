import QtQuick
import Omareader

// The glass window highlight: a pane four or five words wide that rests on
// the line a little ahead of the word being spoken and glides along as the
// voice reads, showing the words under it bright and slightly enlarged.
//
// The paragraph's lines are treated as one track laid end to end, so at the
// end of a line the pane slides off it while the same share slides in at the
// start of the next, instead of jumping down.
//
// Speech is uneven: the voice hurries over short words, lingers on long ones
// and stops at commas. Eyes following the text move at an even pace, so a
// pane that kept step with each word would keep running away from them.
// Instead the pane moves at the voice's average pace over the last few
// seconds, leans gently toward the spoken word when it drifts from it, and
// is held so the spoken word never leaves it.
Item {
    id: glass
    objectName: "readingGlass"

    // The paragraph's text, which the glass sits on; it takes its size and place.
    required property TextEdit body
    // Its highlight, which knows where each line's letters are drawn.
    required property ReadingHighlight layout
    property var styles: []
    property real lineHeight: 1
    // The word being spoken and the one after it, as positions in body's
    // text, and how long the voice takes from one to the other.
    property int wordStart: -1
    property int wordEnd: -1
    property int nextWordStart: -1
    property int nextWordEnd: -1
    property real wordSeconds: 0
    property bool playing: false

    readonly property real gap: 16
    readonly property real padX: Math.round(body.font.pixelSize * 0.5)
    readonly property real padY: 3
    readonly property real margin: 24
    // Slight: the words at the pane's edges are pushed out by this share of
    // half its width, and on a pane this wide more would crowd the page's
    // words just outside it.
    readonly property real magnify: 1.04
    // About four and a half ordinary words wide, which leaves the spoken word room to
    // be ahead of or behind where it should sit; only a longer word
    // stretches it.
    readonly property real baseWidth: Math.round(0.9 * (sample.advanceWidth + 2 * padX))
    // Where in the pane the spoken word sits, as a share of its width from
    // the left: toward the start, so most of the pane shows what comes next.
    readonly property real spot: 0.25
    readonly property real stride: body.width + 2 * gap
    readonly property real paneHeight: Math.ceil(metrics.height) + 2 * padY

    // Each line of the paragraph as laid out: {y, baseline, start}. Read
    // from the layout, not worked out from the first line, since a line can
    // be taller than the rest (a symbol from a fallback font, say).
    property var lines: []

    // Where the pane is on the track (its centre) and how wide, where the
    // voice wants it, and how fast it is moving.
    property real u: 0
    property real paneWidth: baseWidth
    property real speedU: 0
    property real speedWidth: 0
    property bool placed: false
    // The spoken word's and the next word's places on the track, and how far
    // the voice has come from one to the other.
    property real fromU: 0
    property real toU: 0
    property real fromWidth: baseWidth
    property real toWidth: baseWidth
    property real fromHalf: 0
    property real toHalf: 0
    property real elapsed: 0
    readonly property real progress: wordSeconds > 0 ? Math.min(1, elapsed / wordSeconds) : 0
    readonly property real targetU: fromU + (toU - fromU) * progress
    readonly property real targetWidth: fromWidth + (toWidth - fromWidth) * progress
    // Half the width of the word being spoken, on its way to the next.
    readonly property real targetHalf: fromHalf + (toHalf - fromHalf) * progress

    // The voice's pace along the track, in pixels a second, averaged over
    // about paceSeconds; how long it has been measured; and where the voice
    // was at the last step.
    property real pace: 0
    property real paceAge: 0
    property real lastTarget: 0
    readonly property real paceSeconds: 3
    // How strongly the pane leans toward its place for the spoken word, per
    // second of distance: enough to keep the word near its spot on average,
    // weak enough that the pace stays even. The hold in follow() does the rest.
    readonly property real pull: 2
    readonly property bool settled: placed && !moving()

    x: body.x
    y: body.y
    width: body.width
    height: body.height

    TextMetrics {
        id: sample
        font: glass.body.font
        text: "about five plain words wide"
    }
    FontMetrics {
        id: metrics
        font: glass.body.font
    }

    // The pane over line k sits on the line's baseline, as tall as the
    // body font's letters.
    function paneTop(k) {
        const line = lines[k]
        return line ? line.baseline - metrics.ascent - padY : 0
    }

    function measure() {
        lines = body.length > 0 && body.width > 0 ? layout.lines() : []
        placed = false
        aim()
    }

    function lineOf(position) {
        let k = 0
        while (k + 1 < lines.length && lines[k + 1].start <= position)
            ++k
        return k
    }

    // A word's centre on the track, and the width the pane takes over it.
    function place(start, end) {
        const a = body.positionToRectangle(start)
        const b = body.positionToRectangle(Math.max(start, end))
        const right = Math.abs(b.y - a.y) < 1 ? b.x : body.width
        return {
            u: lineOf(start) * stride + gap + (a.x + right) / 2,
            width: Math.max(baseWidth, right - a.x + 2 * padX + body.font.pixelSize * 0.5),
            half: (right - a.x) / 2
        }
    }

    function aim() {
        if (wordStart < 0 || lines.length === 0) {
            placed = false
            return
        }
        const word = place(wordStart, wordEnd)
        const next = nextWordStart >= 0 ? place(nextWordStart, nextWordEnd) : word
        fromU = word.u
        fromWidth = word.width
        fromHalf = word.half
        toU = next.u
        toWidth = next.width
        toHalf = next.half
        elapsed = 0
        // A first word, or one more than a line away (a skip, a click), is
        // a new place to start from, not somewhere to glide to.
        if (!placed || Math.abs(targetU - u) > stride * 1.5) {
            if (!placed)
                pace = paceAge = 0
            paneWidth = targetWidth
            u = aimAt(targetU)
            speedU = speedWidth = 0
            lastTarget = targetU
            placed = true
        }
    }

    // Where the pane's centre goes for the voice at spoken: the word at
    // spot, as far as the pane has room for it.
    function aimAt(spoken) {
        const room = Math.max(0, paneWidth / 2 - targetHalf - padX)
        return spoken + Math.max(-room, Math.min(room, (0.5 - spot) * paneWidth))
    }

    function moving() {
        return playing || Math.abs(aimAt(targetU) - u) >= 0.5 || Math.abs(targetWidth - paneWidth) >= 0.5
               || Math.abs(speedU) >= 1 || Math.abs(speedWidth) >= 1
    }

    // One step while the voice plays: on at the voice's average pace.
    function follow(dt) {
        const spoken = targetU
        const moved = spoken - lastTarget
        lastTarget = spoken
        // The voice's travel this step, early or late words included, so
        // the average comes out right; a skip says nothing about the pace.
        if (moved >= 0 && moved < stride) {
            // Until paceSeconds have been heard, a plain average of them,
            // counted from a second of standing still, so that one quick
            // first word doesn't send the pane racing ahead.
            paceAge += dt
            pace += (moved / dt - pace) * dt / Math.min(paceSeconds, paceAge + 1)
        }
        // Never backward: when the pane is ahead it waits for the voice.
        u += Math.max(0, pace + pull * (aimAt(spoken) - u)) * dt
        // However the pace runs, the spoken word stays inside the pane, if
        // only just: a closer hold than aimAt's leaves the pace more slack.
        const room = Math.max(0, paneWidth / 2 - targetHalf - 2)
        u = Math.min(Math.max(u, spoken - room), spoken + room)
        speedU = 0
    }

    // One step while the voice is paused: settle in place over the word.
    function settle(dt) {
        const w0 = 14
        const goal = aimAt(targetU)
        speedU += (w0 * w0 * (goal - u) - 2 * w0 * speedU) * dt
        u += speedU * dt
        if (Math.abs(goal - u) < 0.5 && Math.abs(speedU) < 1) {
            u = goal
            speedU = 0
        }
    }

    onPlayingChanged: lastTarget = targetU

    onWordStartChanged: aim()
    onNextWordStartChanged: aim()
    onWordSecondsChanged: aim()
    onBaseWidthChanged: aim()
    onWidthChanged: Qt.callLater(measure)
    Connections {
        target: glass.body
        function onContentHeightChanged() { Qt.callLater(glass.measure) }
        function onLineCountChanged() { Qt.callLater(glass.measure) }
    }
    Component.onCompleted: measure()

    // A timer rather than a FrameAnimation: while a FrameAnimation runs, the
    // window redraws on every refresh of the display (144 times a second or
    // more), whatever the animation changes. The pane moves a pixel or two
    // per step at the pace of speech, so 60 steps a second look as smooth
    // and the window only redraws when a step moves it.
    Timer {
        property real last: 0
        interval: 16
        repeat: true
        running: glass.visible && glass.placed && (glass.playing || !glass.settled)
        onRunningChanged: last = Date.now()
        onTriggered: {
            const now = Date.now()
            const dt = Math.min(0.05, Math.max(0, (now - last) / 1000))
            last = now
            if (glass.playing) {
                glass.elapsed += dt
                glass.follow(dt)
            } else {
                glass.settle(dt)
            }
            // The width eases to the word's on a light spring.
            const w0 = 14
            glass.speedWidth += (w0 * w0 * (glass.targetWidth - glass.paneWidth) - 2 * w0 * glass.speedWidth) * dt
            glass.paneWidth += glass.speedWidth * dt
            if (Math.abs(glass.targetWidth - glass.paneWidth) < 0.5 && Math.abs(glass.speedWidth) < 1) {
                glass.paneWidth = glass.targetWidth
                glass.speedWidth = 0
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
            readonly property real paneY: glass.paneTop(line)

            visible: glass.placed && line >= 0 && line < glass.lines.length && shown >= 1
            // It fades as it leaves a line, and grows clearer as it arrives.
            opacity: Math.min(1, 1.5 * shown / glass.paneWidth)
            x: -glass.gap
            y: paneY - glass.margin
            width: glass.stride
            height: glass.paneHeight + 2 * glass.margin
            clip: true

            // A soft shadow lifts the pane off the page.
            Repeater {
                model: [{ grow: 8, drop: 7, alpha: 0.10 }, { grow: 4, drop: 4, alpha: 0.16 }, { grow: 1, drop: 1, alpha: 0.30 }]
                delegate: Rectangle {
                    required property var modelData
                    x: strip.paneX - modelData.grow
                    y: glass.margin - modelData.grow + modelData.drop
                    width: glass.paneWidth + 2 * modelData.grow
                    height: glass.paneHeight + 2 * modelData.grow
                    radius: 9 + modelData.grow
                    color: Qt.rgba(0, 0, 0, modelData.alpha)
                }
            }

            Rectangle {
                objectName: "readingGlassPane"
                x: strip.paneX
                y: glass.margin
                width: glass.paneWidth
                height: glass.paneHeight
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
                    x: -(strip.paneX - glass.gap)
                    y: -strip.paneY
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
                        origin.y: strip.paneY + glass.paneHeight / 2
                        xScale: glass.magnify
                        yScale: glass.magnify
                    }

                    ReadingHighlight {
                        document: copy.textDocument
                        styles: glass.styles
                        lineHeight: glass.lineHeight
                        font: glass.body.font
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
