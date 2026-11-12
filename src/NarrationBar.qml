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

        ColumnLayout {
            Layout.preferredWidth: 220
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
                text: qsTr("%1% of the book · %n min left in chapter", "", reader.minutesLeftInChapter)
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
                enabled: narrator.active
                contentItem: MediaIcon { kind: "previous"; color: parent.enabled ? Colors.text : Colors.muted }
                onClicked: narrator.skip(-1)
                ToolTip.visible: hovered && bar.enabled
                ToolTip.text: qsTr("Previous sentence (,)")
            }

            // The play button: a filled accent circle, with a ring while the voice warms up.
            AbstractButton {
                id: play
                implicitWidth: 60
                implicitHeight: 60
                enabled: narrator.available
                readonly property bool running: narrator.state === Narrator.Playing || narrator.state === Narrator.Preparing
                onClicked: reader.togglePlay()
                ToolTip.visible: hovered && bar.enabled
                ToolTip.text: narrator.available ? qsTr("Read aloud (Space)") : narrator.problem
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
                enabled: narrator.active
                contentItem: MediaIcon { kind: "next"; color: parent.enabled ? Colors.text : Colors.muted }
                onClicked: narrator.skip(1)
                ToolTip.visible: hovered && bar.enabled
                ToolTip.text: qsTr("Next sentence (.)")
            }
        }

        RowLayout {
            Layout.preferredWidth: 220
            Layout.fillWidth: true
            spacing: 4
            Item { Layout.fillWidth: true }
            ToolButton {
                text: "−"
                onClicked: narrator.speed = narrator.speed - 0.1
                ToolTip.visible: hovered && bar.enabled
                ToolTip.text: qsTr("Slower ([)")
            }
            Label {
                text: qsTr("%1×").arg(narrator.speed.toLocaleString(Qt.locale(), 'f', 2))
                color: Colors.text
                font.pixelSize: 13
                font.features: { "tnum": 1 }
            }
            ToolButton {
                text: "+"
                onClicked: narrator.speed = narrator.speed + 0.1
                ToolTip.visible: hovered && bar.enabled
                ToolTip.text: qsTr("Faster (])")
            }
            Label {
                Layout.leftMargin: 8
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
