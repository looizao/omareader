import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Adds pasted text to the library, for articles, notes and messages that
// have no book file. The title starts as the text's first words; Omarchy's
// coding agent can suggest a better one once the user allows it, since the
// agent sends the text to its provider.
Popup {
    id: dialog
    modal: true
    focus: true
    closePolicy: Popup.CloseOnPressOutside
    anchors.centerIn: Overlay.overlay
    width: Math.min(640, (parent ? parent.width : 640) - 48)
    height: Math.min(560, (parent ? parent.height : 560) - 48)
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: Colors.surface
        radius: theme.radius
        border.color: Colors.line
    }

    // Set once the user types a title, which then stays theirs.
    property bool titleEdited: false
    property string agentNote: ""
    readonly property bool hasText: textArea.text.trim() !== ""

    // Opens empty, or with the clipboard already pasted in.
    function start(fromClipboard) {
        textArea.clear()
        titleField.clear()
        dialog.titleEdited = false
        dialog.agentNote = ""
        open()
        if (fromClipboard)
            textArea.paste()
    }

    function add() {
        if (!dialog.hasText)
            return
        titleAgent.cancel()
        if (reader.addText(titleField.text, textArea.text))
            close()
    }

    function suggestTitle() {
        dialog.agentNote = ""
        if (titleAgent.enabled && titleAgent.available && !dialog.titleEdited && dialog.hasText)
            titleAgent.suggest(textArea.text)
    }

    onOpened: {
        titleAgent.refresh()
        textArea.forceActiveFocus()
    }
    // The text isn't kept around once the dialog is gone.
    onClosed: {
        titleAgent.cancel()
        suggestDelay.stop()
        textArea.clear()
        titleField.clear()
    }

    // Waits for the paste or the typing to settle before asking the agent.
    Timer {
        id: suggestDelay
        interval: 900
        onTriggered: dialog.suggestTitle()
    }

    Connections {
        target: titleAgent
        function onSuggested(title) {
            if (dialog.opened && !dialog.titleEdited)
                titleField.text = title
        }
        function onFailed(reason) {
            if (dialog.opened)
                dialog.agentNote = reason
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            text: qsTr("Add text")
            font.pixelSize: 20
            font.family: "serif"
            color: Colors.text
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Paste an article, a note or any text to read it aloud. It is saved in your library.")
            color: Colors.muted
            wrapMode: Text.Wrap
            font.pixelSize: 13
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: 4
            radius: Math.min(8, theme.radius)
            color: Colors.background
            border.color: textArea.activeFocus ? theme.accent : Colors.line
            // Material floats a placeholder up into the border, which the
            // scroll view clips, so this one stays put.
            Label {
                x: 13
                y: 13
                visible: textArea.text === ""
                text: qsTr("Paste or type the text here (Ctrl+V pastes)")
                color: Colors.muted
                font.pixelSize: 15
            }
            ScrollView {
                id: scroll
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                TextArea {
                    id: textArea
                    objectName: "pasteText"
                    wrapMode: TextEdit.Wrap
                    textFormat: TextEdit.PlainText
                    selectByMouse: true
                    color: Colors.text
                    font.pixelSize: 15
                    topInset: 0
                    bottomInset: 0
                    padding: 12
                    topPadding: 12
                    bottomPadding: 12
                    background: null
                    Keys.onPressed: event => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                && (event.modifiers & Qt.ControlModifier)) {
                            dialog.add()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Escape) {
                            dialog.close()
                            event.accepted = true
                        }
                    }
                    onTextChanged: {
                        if (!dialog.titleEdited)
                            titleField.text = titleAgent.fallbackTitle(text)
                        titleAgent.cancel()
                        dialog.agentNote = ""
                        if (titleAgent.enabled)
                            suggestDelay.restart()
                    }
                }
            }
        }

        TextField {
            id: titleField
            objectName: "pasteTitle"
            Layout.fillWidth: true
            Layout.topMargin: 6
            placeholderText: qsTr("Title")
            selectByMouse: true
            color: Colors.text
            onTextEdited: {
                dialog.titleEdited = text.trim() !== ""
                titleAgent.cancel()
            }
            Keys.onReturnPressed: dialog.add()
            Keys.onEnterPressed: dialog.add()
            Keys.onEscapePressed: dialog.close()
        }

        // Only offered when a supported agent is installed, and off until the
        // user turns it on.
        RowLayout {
            Layout.fillWidth: true
            visible: titleAgent.available
            spacing: 4
            CheckBox {
                id: useAgent
                objectName: "pasteUseAgent"
                checked: titleAgent.enabled
                text: qsTr("Suggest a title with %1").arg(titleAgent.name)
                onToggled: {
                    titleAgent.enabled = checked
                    if (checked)
                        dialog.suggestTitle()
                }
            }
            BusyIndicator {
                visible: titleAgent.busy
                running: visible
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
            }
            Item { Layout.fillWidth: true }
        }
        Label {
            Layout.fillWidth: true
            visible: titleAgent.available
            text: dialog.agentNote !== ""
                  ? qsTr("%1 The first words stay as the title.").arg(dialog.agentNote)
                  : qsTr("%1 sends the first part of the text to its provider. Everything else stays on this computer.")
                        .arg(titleAgent.name)
            color: Colors.muted
            wrapMode: Text.Wrap
            font.pixelSize: 12
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 8
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: qsTr("Ctrl+Enter adds · Esc cancels")
                color: Colors.muted
                font.pixelSize: 12
                elide: Text.ElideRight
            }
            Button {
                flat: true
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }
            AccentButton {
                objectName: "pasteAdd"
                enabled: dialog.hasText && !reader.loading
                text: qsTr("Add to library")
                onClicked: dialog.add()
            }
        }
    }
}
