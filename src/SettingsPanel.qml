import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The engine, its voice, speed and tuning, and how the page looks.
Drawer {
    id: panel
    edge: Qt.RightEdge
    width: Math.min(400, parent ? parent.width * 0.85 : 400)
    Material.roundedScale: theme.radius
    background: Rectangle { color: Colors.surface }

    onOpened: engine.forceActiveFocus()

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
        // Qwen3-TTS names its languages in English.
        case "Auto": return qsTr("Detect from the text")
        case "English": return qsTr("English")
        case "Portuguese": return qsTr("Portuguese")
        case "Spanish": return qsTr("Spanish")
        case "French": return qsTr("French")
        case "German": return qsTr("German")
        case "Italian": return qsTr("Italian")
        case "Russian": return qsTr("Russian")
        case "Chinese": return qsTr("Chinese")
        case "Japanese": return qsTr("Japanese")
        case "Korean": return qsTr("Korean")
        }
        return code
    }

    // A tuning value as the panel shows it: "110%", "0.45 s", "0.90".
    function formatValue(item, value) {
        const decimals = item.step >= 1 ? 0 : item.step >= 0.1 ? 1 : 2
        const number = Number(value).toLocaleString(Qt.locale(), 'f', decimals)
        if (item.unit === "%")
            return qsTr("%1%").arg(number)
        if (item.unit === "s")
            return qsTr("%1 s").arg(number)
        return number
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

            Label { Layout.leftMargin: 16; text: qsTr("Engine"); color: Colors.body }
            ChoiceBox {
                id: engine
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                model: narrator.engines.map(e => ({
                    id: e.id,
                    label: e.available ? e.name : qsTr("%1 (not installed)").arg(e.name)
                }))
                selected: narrator.engine
                onChosen: value => narrator.engine = value
            }

            Label { Layout.leftMargin: 16; Layout.topMargin: 6; text: qsTr("Voice"); color: Colors.body }
            ChoiceBox {
                id: voice
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                // Labels carry the language, since a voice reads its own language best.
                model: narrator.voices.map(v => ({
                    id: v.id,
                    label: qsTr("%1 · %2").arg(v.name).arg(panel.languageName(v.language))
                }))
                selected: narrator.voice
                onChosen: value => narrator.voice = value
            }
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                readonly property var current: narrator.voices.find(v => v.id === narrator.voice)
                visible: text.length > 0
                text: current && current.description ? qsTr(current.description) : ""
                wrapMode: Text.Wrap
                font.pixelSize: 12
                color: Colors.muted
            }

            Label {
                Layout.leftMargin: 16
                Layout.topMargin: 6
                visible: narrator.languages.length > 0
                text: qsTr("Language")
                color: Colors.body
            }
            ChoiceBox {
                id: language
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                visible: narrator.languages.length > 0
                model: narrator.languages.map(l => ({ id: l.id, label: panel.languageName(l.id) }))
                selected: narrator.language
                onChosen: value => narrator.language = value
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

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                spacing: 8
                AccentButton {
                    text: narrator.previewing ? qsTr("Stop preview") : qsTr("Preview")
                    enabled: narrator.available
                    onClicked: narrator.previewing ? narrator.stopPreview() : narrator.preview()
                }
                Button {
                    flat: true
                    text: qsTr("Reset to recommended")
                    enabled: narrator.tuned
                    onClicked: narrator.resetTuning()
                }
                Item { Layout.fillWidth: true }
            }
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                visible: narrator.error.length > 0 && panel.opened
                text: narrator.error
                wrapMode: Text.Wrap
                font.pixelSize: 12
                color: "#f2d4d6"
            }

            // Tuning lives behind a disclosure so the everyday choices stay simple.
            ItemDelegate {
                id: advancedToggle
                property bool expanded: false
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                Layout.topMargin: 4
                text: (expanded ? "▾  " : "▸  ") + qsTr("Advanced")
                onClicked: expanded = !expanded
            }

            Repeater {
                model: advancedToggle.expanded ? narrator.tuning : []
                delegate: ColumnLayout {
                    id: setting
                    required property var modelData
                    readonly property var item: modelData
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.bottomMargin: 6
                    visible: item.visible
                    spacing: 2

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr(setting.item.label)
                            color: Colors.body
                            elide: Text.ElideRight
                        }
                        Label {
                            visible: setting.item.kind === "number"
                            text: panel.formatValue(setting.item, setting.item.value)
                            color: Colors.text
                            font.features: { "tnum": 1 }
                        }
                    }
                    Slider {
                        visible: setting.item.kind === "number"
                        Layout.fillWidth: true
                        Layout.leftMargin: -8
                        Layout.rightMargin: -8
                        from: setting.item.minimum
                        to: setting.item.maximum
                        stepSize: setting.item.step
                        value: setting.item.value
                        onMoved: narrator.setTuning(setting.item.key, value)
                    }
                    ChoiceBox {
                        visible: setting.item.kind === "choice"
                        Layout.fillWidth: true
                        model: setting.item.choices.map(c => ({ id: c.value, label: qsTr(c.label) }))
                        selected: setting.item.value
                        onChosen: value => narrator.setTuning(setting.item.key, value)
                    }
                    TextField {
                        visible: setting.item.kind === "text"
                        Layout.fillWidth: true
                        text: setting.item.value
                        placeholderText: qsTr("calm, warm audiobook narration")
                        onEditingFinished: narrator.setTuning(setting.item.key, text)
                    }
                    Label {
                        Layout.fillWidth: true
                        text: {
                            const description = qsTr(setting.item.description)
                            if (setting.item.kind !== "number" || setting.item.value === setting.item.recommended)
                                return description
                            return qsTr("%1 Recommended: %2.").arg(description)
                                .arg(panel.formatValue(setting.item, setting.item.recommended))
                        }
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: Colors.muted
                    }
                }
            }

            // Where the engine is, or what's missing, and what the hardware allows.
            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 6
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
                        visible: narrator.hardwareAdvice.length > 0
                        text: narrator.hardwareAdvice
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: Colors.body
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: narrator.available
                        text: narrator.setup.paths || ""
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
