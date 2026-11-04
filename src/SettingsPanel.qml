import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The voice, its speed, and how the page looks.
Drawer {
    id: panel
    edge: Qt.RightEdge
    width: Math.min(400, parent ? parent.width * 0.85 : 400)
    Material.roundedScale: theme.radius
    background: Rectangle { color: Colors.surface }

    onOpened: voice.forceActiveFocus()

    function languageName(code) {
        switch (code) {
        case "en-us": return qsTr("American English")
        case "en-gb": return qsTr("British English")
        case "es": return qsTr("Spanish")
        case "fr-fr": return qsTr("French")
        case "hi": return qsTr("Hindi")
        case "it": return qsTr("Italian")
        case "ja": return qsTr("Japanese")
        case "pt-br": return qsTr("Brazilian Portuguese")
        case "cmn": return qsTr("Mandarin Chinese")
        }
        return code
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            width: parent.width
            spacing: 6

            Label {
                Layout.margins: 16
                Layout.bottomMargin: 4
                text: qsTr("Voice")
                font.pixelSize: 20
                font.family: "serif"
                color: Colors.text
            }

            ComboBox {
                id: voice
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                model: narrator.voices
                textRole: "label"
                valueRole: "id"
                // Labels carry the language, since a voice reads its own language best.
                Component.onCompleted: {
                    const voices = narrator.voices
                    const labelled = []
                    for (let i = 0; i < voices.length; ++i)
                        labelled.push({ id: voices[i].id, label: qsTr("%1 · %2").arg(voices[i].name).arg(panel.languageName(voices[i].language)) })
                    model = labelled
                    currentIndex = indexOfValue(narrator.voice)
                }
                onActivated: narrator.voice = currentValue
            }

            Label {
                Layout.leftMargin: 16
                Layout.topMargin: 10
                text: qsTr("Speed %1×").arg(narrator.speed.toLocaleString(Qt.locale(), 'f', 2))
                color: Colors.body
            }
            Slider {
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                from: 0.5
                to: 2.0
                stepSize: 0.05
                value: narrator.speed
                onMoved: narrator.speed = value
            }

            // Where Kokoros is, or what's missing.
            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                implicitHeight: status.implicitHeight + 24
                radius: theme.radius
                color: Colors.raised
                ColumnLayout {
                    id: status
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 4
                    Label {
                        Layout.fillWidth: true
                        text: narrator.available ? qsTr("Read-aloud is ready, fully offline.") : narrator.problem
                        wrapMode: Text.Wrap
                        color: narrator.available ? Colors.text : "#f2d4d6"
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: narrator.available
                        text: narrator.setup.koko + "\n" + narrator.setup.model
                        wrapMode: Text.WrapAnywhere
                        font.family: "monospace"
                        font.pixelSize: 11
                        color: Colors.muted
                    }
                    Button {
                        flat: true
                        text: qsTr("Check again")
                        onClicked: narrator.refreshSetup()
                    }
                }
            }

            Label {
                Layout.margins: 16
                Layout.topMargin: 24
                Layout.bottomMargin: 4
                text: qsTr("Page")
                font.pixelSize: 20
                font.family: "serif"
                color: Colors.text
            }

            Label { Layout.leftMargin: 16; text: qsTr("Text size %1").arg(reader.fontSize); color: Colors.body }
            Slider {
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                from: 12; to: 48; stepSize: 1
                value: reader.fontSize
                onMoved: reader.fontSize = value
            }

            Label {
                Layout.leftMargin: 16
                text: qsTr("Line spacing %1").arg(reader.lineHeight.toLocaleString(Qt.locale(), 'f', 1))
                color: Colors.body
            }
            Slider {
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                from: 1.0; to: 2.4; stepSize: 0.1
                value: reader.lineHeight
                onMoved: reader.lineHeight = value
            }

            Label { Layout.leftMargin: 16; text: qsTr("Column width"); color: Colors.body }
            Slider {
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                from: 420; to: 1400; stepSize: 20
                value: reader.contentWidth
                onMoved: reader.contentWidth = value
            }

            Switch {
                Layout.leftMargin: 8
                text: qsTr("Serif typeface")
                checked: reader.serif
                onToggled: reader.serif = checked
            }

            Item { Layout.preferredHeight: 24 }
        }
    }
}
