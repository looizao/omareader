import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omareader

// How a name or term is said: the word as books write it and a respelling of
// how it sounds, which the voice then says wherever the word is written,
// reading aloud and in exports. Hear it tries the respelling with the
// current voice before it is saved.
Popup {
    id: dialog
    objectName: "pronunciationEditor"
    modal: true
    focus: true
    closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape
    anchors.centerIn: Overlay.overlay
    width: Math.min(520, (parent ? parent.width : 520) - 48)
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: Colors.surface
        radius: theme.radius
        border.color: Colors.line
    }

    // The entry being edited, empty for a new one.
    property string original: ""
    // Whether the voice was paused for this and goes on when it closes.
    property bool resumeOnClose: false
    readonly property bool canSave: pronunciations.valid(written.text, spoken.text)

    // Opens for a word: its saved respelling when it has one, otherwise a
    // new entry for it (or a blank one).
    function edit(word) {
        const text = (word || "").trim()
        const saved = text !== "" ? pronunciations.spokenFor(text) : ""
        dialog.original = saved !== "" ? text : ""
        written.text = text
        spoken.text = saved
        open()
    }

    function save() {
        if (!dialog.canSave)
            return
        if (pronunciations.set(written.text, spoken.text, dialog.original))
            close()
    }

    function remove() {
        pronunciations.remove(dialog.original)
        close()
    }

    onOpened: (written.text === "" ? written : spoken).forceActiveFocus()
    onClosed: {
        narrator.stopPreview()
        dialog.original = ""
        if (dialog.resumeOnClose && narrator.state === Narrator.Paused)
            narrator.resume()
        dialog.resumeOnClose = false
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            text: qsTr("Pronunciation")
            font.pixelSize: 20
            font.family: "serif"
            color: Colors.text
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Spell it the way it sounds, as in Her-my-oh-nee for Hermione, and the voice will say it that way wherever it is written, reading aloud and in exports.")
            wrapMode: Text.Wrap
            font.pixelSize: 13
            color: Colors.muted
        }

        Label { Layout.topMargin: 6; text: qsTr("Written"); color: Colors.body }
        TextField {
            id: written
            objectName: "pronunciationWritten"
            Layout.fillWidth: true
            selectByMouse: true
            onAccepted: spoken.forceActiveFocus()
        }

        Label { Layout.topMargin: 6; text: qsTr("Say it as"); color: Colors.body }
        TextField {
            id: spoken
            objectName: "pronunciationSpoken"
            Layout.fillWidth: true
            selectByMouse: true
            onAccepted: dialog.save()
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 8
            spacing: 4
            Button {
                objectName: "removePronunciation"
                visible: dialog.original !== ""
                flat: true
                text: qsTr("Remove")
                onClicked: dialog.remove()
            }
            Button {
                objectName: "hearPronunciation"
                flat: true
                text: narrator.previewing ? qsTr("Stop") : qsTr("Hear it")
                enabled: narrator.available && (narrator.previewing || spoken.text.trim() !== "")
                onClicked: narrator.previewing ? narrator.stopPreview() : narrator.previewText(spoken.text)
            }
            Item { Layout.fillWidth: true }
            Button {
                flat: true
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }
            AccentButton {
                objectName: "savePronunciation"
                text: qsTr("Save")
                enabled: dialog.canSave
                onClicked: dialog.save()
            }
        }
    }
}
