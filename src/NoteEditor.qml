import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A highlight and its note: the passage, the note to write or change, and
// ways to remove the highlight or read aloud from it. The note is kept as
// the dialog closes, however it closes.
Popup {
    id: dialog
    objectName: "noteEditor"
    modal: true
    focus: true
    closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape
    anchors.centerIn: Overlay.overlay
    width: Math.min(560, (parent ? parent.width : 560) - 48)
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: Colors.surface
        radius: theme.radius
        border.color: Colors.line
    }

    property string annotationId: ""
    property var entry: ({})

    function edit(id) {
        dialog.entry = annotations.item(id)
        if (dialog.entry.id === undefined)
            return
        dialog.annotationId = id
        note.text = dialog.entry.note || ""
        open()
    }

    function remove() {
        annotations.remove(dialog.annotationId)
        dialog.annotationId = ""
        close()
    }

    function readAloud() {
        const entry = dialog.entry
        close()
        if (entry.chapter !== reader.chapter)
            reader.goTo(entry.chapter, entry.paragraph, entry.start)
        reader.readFrom(entry.paragraph, entry.start)
    }

    onOpened: note.forceActiveFocus()
    // Saved as soon as it starts to close, not once it has faded away.
    onAboutToHide: {
        if (dialog.annotationId !== "")
            annotations.setNote(dialog.annotationId, note.text)
        dialog.annotationId = ""
    }
    onClosed: note.clear()

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Label {
            text: qsTr("Highlight")
            font.pixelSize: 20
            font.family: "serif"
            color: Colors.text
        }

        // The passage, in the book's own type, marked as the page marks it.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: passage.implicitHeight + 20
            radius: Math.min(8, theme.radius)
            color: Colors.background
            Rectangle {
                width: 3
                height: parent.height
                radius: 1.5
                color: theme.accent
            }
            Label {
                id: passage
                anchors.fill: parent
                anchors.margins: 10
                anchors.leftMargin: 16
                text: dialog.entry.text || ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                maximumLineCount: 6
                elide: Text.ElideRight
                font.family: reader.serif ? "serif" : "sans-serif"
                font.pixelSize: 15
                color: Colors.body
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 140
            radius: Math.min(8, theme.radius)
            color: Colors.background
            border.color: note.activeFocus ? theme.accent : Colors.line
            // Material floats a placeholder up into the border, which the
            // scroll view clips, so this one stays put.
            Label {
                x: 13
                y: 13
                visible: note.text === ""
                text: qsTr("Write a note…")
                color: Colors.muted
                font.pixelSize: 15
            }
            ScrollView {
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                TextArea {
                    id: note
                    objectName: "noteText"
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
                            dialog.close()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Escape) {
                            dialog.close()
                            event.accepted = true
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            Button {
                objectName: "removeHighlight"
                flat: true
                text: qsTr("Remove highlight")
                onClicked: dialog.remove()
            }
            Button {
                flat: true
                text: qsTr("Read aloud from here")
                onClicked: dialog.readAloud()
            }
            Item { Layout.fillWidth: true }
            AccentButton {
                text: qsTr("Done")
                onClicked: dialog.close()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Ctrl+Enter")
            }
        }
    }
}
