import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omareader

// RSVP: the book one flash at a time in place of the page. The flash's
// recognition point, tinted in the accent, always sits on the same spot,
// marked by a tick above and below, so the eye never has to move. While
// paused, the sentence it is in shows beneath it, faintly, to find the place
// again or move to another of its words with a click.
Item {
    id: stage
    objectName: "rsvpView"

    // Where the recognition point sits: a little left of the middle, as the
    // point is a little left of the middle of a word.
    readonly property real pivotX: Math.round(width * 0.42)
    readonly property real lineY: Math.round(height * 0.4)
    readonly property bool playing: rsvp.running || narrator.state === Narrator.Playing
                                    || narrator.state === Narrator.Preparing
    readonly property string fontFamily: reader.serif ? "serif" : "sans-serif"

    // The flash on screen, in the coordinates of item, for the word card.
    function flashRect(item) {
        return line.mapToItem(item, Math.min(beforeText.x, pivotText.x), 0,
                              Math.max(8, line.contentWidth), line.height)
    }

    // A click on the flash starts or pauses it; the wheel steps through it
    // while paused.
    MouseArea {
        anchors.fill: parent
        onClicked: rsvp.toggle()
        onWheel: wheel => {
            if (stage.playing || wheel.angleDelta.y === 0)
                return
            rsvp.step(wheel.angleDelta.y < 0 ? 1 : -1)
        }
    }

    // The ticks above and below the recognition point.
    Repeater {
        model: [-1, 1]
        Rectangle {
            required property int modelData
            x: stage.pivotX - 1
            y: modelData < 0 ? stage.lineY - rsvp.fontSize * 0.95 : stage.lineY + rsvp.fontSize * 0.6
            width: 2
            height: rsvp.fontSize * 0.35
            radius: 1
            color: Colors.muted
            opacity: 0.6
        }
    }

    // The flash, in three pieces around its recognition point. A flash too
    // wide for the window shrinks about the point rather than wrapping.
    Item {
        id: line
        objectName: "rsvpFlash"
        x: 0
        y: stage.lineY - height / 2
        width: stage.width
        height: pivotText.implicitHeight
        readonly property real margin: 24
        // How far the flash reaches either side of the point.
        readonly property real leftReach: beforeText.implicitWidth + pivotText.implicitWidth / 2
        readonly property real rightReach: afterText.implicitWidth + pivotText.implicitWidth / 2
        readonly property real contentWidth: leftReach + rightReach
        readonly property real fit: Math.min(1,
                                             leftReach > 0 ? (stage.pivotX - margin) / leftReach : 1,
                                             rightReach > 0 ? (stage.width - stage.pivotX - margin) / rightReach : 1)
        transform: Scale {
            origin.x: stage.pivotX
            origin.y: line.height / 2
            xScale: line.fit
            yScale: line.fit
        }

        Text {
            id: beforeText
            x: pivotText.x - implicitWidth
            text: rsvp.before
            textFormat: Text.PlainText
            color: Colors.text
            font.family: stage.fontFamily
            font.pixelSize: rsvp.fontSize
        }
        Text {
            id: pivotText
            objectName: "rsvpPivot"
            x: stage.pivotX - implicitWidth / 2
            text: rsvp.pivot
            textFormat: Text.PlainText
            color: theme.accent
            font.family: stage.fontFamily
            font.pixelSize: rsvp.fontSize
        }
        Text {
            id: afterText
            x: pivotText.x + pivotText.implicitWidth
            text: rsvp.after
            textFormat: Text.PlainText
            color: Colors.text
            font.family: stage.fontFamily
            font.pixelSize: rsvp.fontSize
        }
    }

    // The sentence's width on one line, so a short one sits in the middle.
    TextMetrics {
        id: contextMetrics
        font.family: stage.fontFamily
        font.pixelSize: Math.max(14, reader.fontSize - 2)
        text: rsvp.context.map(flash => flash.text).join("")
    }

    // The sentence, while paused, its flashes each a click away.
    Flow {
        id: context
        objectName: "rsvpContext"
        visible: !stage.playing
        width: Math.min(reader.contentWidth, stage.width - 64,
                        Math.ceil(contextMetrics.advanceWidth) + spacing * Math.max(0, rsvp.context.length - 1) + 4)
        x: (stage.width - width) / 2
        y: stage.lineY + rsvp.fontSize * 1.3
        spacing: 6
        Repeater {
            model: rsvp.context
            Label {
                required property var modelData
                readonly property bool current: modelData.index === rsvp.index
                text: modelData.text
                textFormat: Text.PlainText
                color: current ? Colors.text : Colors.muted
                font.family: stage.fontFamily
                font.pixelSize: Math.max(14, reader.fontSize - 2)
                font.underline: current
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: rsvp.moveTo(modelData.index)
                }
            }
        }
    }

    // How far into the chapter the flash is.
    Rectangle {
        objectName: "rsvpProgress"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 28
        width: Math.min(320, stage.width - 64)
        height: 2
        radius: 1
        color: Colors.line
        Rectangle {
            width: parent.width * rsvp.chapterProgress
            height: parent.height
            radius: 1
            color: Colors.muted
        }
    }
}
