import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omareader

// Play, pause and skip the narration; the voice, its speed, and how far along
// the book is.
Rectangle {
    id: bar
    implicitHeight: 88
    color: Colors.surface

    // RSVP paced by its own clock rather than the voice: the bar then
    // shows its rate in place of the voice's speed, and no sleep timer.
    readonly property bool silent: rsvp.active && !rsvp.aloud

    function openSleepMenu() {
        if (!bar.silent)
            sleepMenu.open()
    }

    // Minutes of listening left as m:ss.
    function clock(seconds) {
        const s = Math.max(0, seconds)
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }

    // One of the sleep timer's choices, in the accent while it is the one
    // set: a pick of one, so no checkboxes.
    component SleepChoice: MenuItem {
        property bool current: false
        Material.foreground: current ? theme.accent : Colors.text
        font.weight: current ? Font.DemiBold : Font.Normal
    }

    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: Colors.line
    }

    // Book progress along the top edge.
    Rectangle {
        anchors.top: parent.top
        height: 2
        width: parent.width * reader.progress
        color: theme.accent
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 20
        anchors.rightMargin: 20
        spacing: 16

        // Both sides ask for the same width, so the play button stays in the
        // middle of the bar whenever there is room for it to.
        ColumnLayout {
            Layout.preferredWidth: Math.max(220, controls.implicitWidth)
            Layout.fillWidth: true
            spacing: 2
            Label {
                Layout.fillWidth: true
                text: qsTr("Chapter %1 of %2").arg(reader.chapter + 1).arg(reader.chapterCount)
                color: Colors.text
                font.pixelSize: 13
                elide: Text.ElideRight
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("%1% of the book · %n min left in chapter", "",
                           rsvp.active ? rsvp.minutesLeftInChapter : reader.minutesLeftInChapter)
                          .arg(Math.round(reader.progress * 100).toLocaleString(Qt.locale(), 'f', 0))
                color: Colors.muted
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 8

            ToolButton {
                implicitWidth: 44
                implicitHeight: 44
                enabled: narrator.active || rsvp.active
                contentItem: MediaIcon { kind: "previous"; color: parent.enabled ? Colors.text : Colors.muted }
                onClicked: rsvp.active ? rsvp.skip(-1) : narrator.skip(-1)
                ToolTip.visible: hovered && bar.enabled
                ToolTip.text: qsTr("Previous sentence (,)")
            }

            // The play button: a filled accent circle, with a ring while the voice warms up.
            AbstractButton {
                id: play
                implicitWidth: 60
                implicitHeight: 60
                objectName: "playButton"
                // The silent clock needs no voice.
                enabled: bar.silent || narrator.available
                readonly property bool running: narrator.state === Narrator.Playing || narrator.state === Narrator.Preparing
                                                || rsvp.running
                onClicked: rsvp.active ? rsvp.toggle() : reader.togglePlay()
                ToolTip.visible: hovered && bar.enabled
                ToolTip.text: bar.silent ? qsTr("Start or pause (Space)")
                            : narrator.available ? qsTr("Read aloud (Space)") : narrator.problem
                background: Rectangle {
                    radius: width / 2
                    color: !play.enabled ? Colors.line
                         : play.down ? Qt.darker(theme.accent, 1.15)
                         : play.hovered ? Qt.lighter(theme.accent, 1.08) : theme.accent
                }
                contentItem: Item {
                    MediaIcon {
                        anchors.centerIn: parent
                        anchors.horizontalCenterOffset: play.running ? 0 : 2
                        width: 22
                        height: 22
                        kind: play.running ? "pause" : "play"
                        color: play.enabled ? theme.accentForeground : Colors.muted
                    }
                }
                BusyIndicator {
                    anchors.centerIn: parent
                    width: 76
                    height: 76
                    running: narrator.state === Narrator.Preparing
                    visible: running
                }
            }

            ToolButton {
                implicitWidth: 44
                implicitHeight: 44
                enabled: narrator.active || rsvp.active
                contentItem: MediaIcon { kind: "next"; color: parent.enabled ? Colors.text : Colors.muted }
                onClicked: rsvp.active ? rsvp.skip(1) : narrator.skip(1)
                ToolTip.visible: hovered && bar.enabled
                ToolTip.text: qsTr("Next sentence (.)")
            }
        }

        RowLayout {
            id: controls
            Layout.preferredWidth: Math.max(220, implicitWidth)
            // In a narrow window the chapter on the left gives way, never these.
            Layout.minimumWidth: implicitWidth
            Layout.fillWidth: true
            spacing: 4
            Item { Layout.fillWidth: true }
            // In RSVP: whether the voice reads and paces the flashes.
            Switch {
                objectName: "rsvpAloudSwitch"
                visible: rsvp.active
                Layout.rightMargin: 4
                text: qsTr("Read aloud")
                checked: rsvp.aloud
                onToggled: rsvp.aloud = checked
                font.pixelSize: 13
            }
            // The sleep timer: a moon, lit while it runs, beside what is
            // left of it.
            ToolButton {
                id: sleepButton
                objectName: "sleepButton"
                // It counts only the voice.
                visible: !bar.silent
                implicitWidth: 40
                implicitHeight: 40
                onClicked: sleepMenu.opened ? sleepMenu.close() : sleepMenu.open()
                ToolTip.visible: hovered && bar.enabled && !sleepMenu.opened
                ToolTip.text: qsTr("Sleep timer (C)")
                SleepIcon {
                    objectName: "sleepIcon"
                    anchors.centerIn: parent
                    width: 18
                    height: 18
                    color: narrator.sleepTimerOn ? theme.accent : Colors.text
                }

                Menu {
                    id: sleepMenu
                    objectName: "sleepMenu"
                    // Above the bar, not over it.
                    onAboutToShow: y = -implicitHeight - sleepButton.mapToItem(bar, 0, 0).y - 8
                    // Drawn here: the offscreen renderer drops Material's
                    // elevated popup background.
                    background: Rectangle {
                        implicitWidth: 200
                        color: Colors.raised
                        border.color: Colors.line
                        radius: theme.radius
                    }
                    Repeater {
                        model: [10, 15, 30, 45, 60, 90]
                        SleepChoice {
                            required property int modelData
                            text: qsTr("%n minute(s)", "", modelData)
                            current: narrator.sleepMinutes === modelData
                            onTriggered: narrator.sleepAfter(modelData)
                        }
                    }
                    SleepChoice {
                        text: qsTr("End of chapter")
                        current: narrator.sleepAtChapterEnd
                        onTriggered: narrator.sleepAtEndOfChapter()
                    }
                    MenuSeparator {}
                    SleepChoice {
                        text: qsTr("Off")
                        current: !narrator.sleepTimerOn
                        onTriggered: narrator.cancelSleep()
                    }
                }
            }
            Label {
                objectName: "sleepLabel"
                visible: narrator.sleepTimerOn && !bar.silent
                Layout.rightMargin: 8
                text: narrator.sleepAtChapterEnd ? qsTr("End of chapter") : bar.clock(narrator.sleepSecondsLeft)
                color: theme.accent
                font.pixelSize: 13
                font.features: { "tnum": 1 }
            }
            ToolButton {
                text: "−"
                onClicked: {
                    if (bar.silent)
                        rsvp.wpm = rsvp.wpm - 25
                    else
                        narrator.speed = narrator.speed - 0.1
                }
                ToolTip.visible: hovered && bar.enabled
                ToolTip.text: qsTr("Slower ([)")
            }
            Label {
                objectName: "speedLabel"
                text: bar.silent ? qsTr("%1 wpm").arg(rsvp.wpm)
                                 : qsTr("%1×").arg(narrator.speed.toLocaleString(Qt.locale(), 'f', 2))
                color: Colors.text
                font.pixelSize: 13
                font.features: { "tnum": 1 }
            }
            ToolButton {
                objectName: "fasterButton"
                text: "+"
                onClicked: {
                    if (bar.silent)
                        rsvp.wpm = rsvp.wpm + 25
                    else
                        narrator.speed = narrator.speed + 0.1
                }
                ToolTip.visible: hovered && bar.enabled
                ToolTip.text: qsTr("Faster (])")
            }
            Label {
                objectName: "voiceName"
                Layout.leftMargin: 8
                // A narrow bar showing the sleep timer has no room for it,
                // and the silent clock reads with no voice.
                visible: !bar.silent && (bar.width >= 820 || !narrator.sleepTimerOn)
                text: {
                    const voices = narrator.voices
                    for (let i = 0; i < voices.length; ++i)
                        if (voices[i].id === narrator.voice)
                            return voices[i].name
                    return narrator.voice
                }
                color: Colors.muted
                font.pixelSize: 13
            }
        }
    }
}
