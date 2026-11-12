import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omareader

// Exports the open book as audio for a phone: one file with chapter markers
// or a folder with a file per chapter, in MP3 or M4B. The export carries on
// in the background once the dialog is closed.
Popup {
    id: dialog
    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(540, (parent ? parent.width : 540) - 48)
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: Colors.surface
        radius: theme.radius
        border.color: Colors.line
    }

    readonly property string voiceName: {
        const voices = narrator.voices
        for (let i = 0; i < voices.length; ++i)
            if (voices[i].id === narrator.voice)
                return voices[i].name
        return narrator.voice
    }

    function duration(minutes) {
        if (minutes < 60)
            return qsTr("%1 min").arg(Math.max(1, minutes))
        return qsTr("%1 h %2 min").arg(Math.floor(minutes / 60)).arg(minutes % 60)
    }

    onOpened: {
        narrator.refreshSetup()
        if (exporter.running)
            hide.forceActiveFocus()
        else
            start.forceActiveFocus()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            text: qsTr("Export as audio")
            font.pixelSize: 20
            font.family: "serif"
            color: Colors.text
        }
        Label {
            Layout.fillWidth: true
            text: exporter.running
                  ? exporter.bookTitle
                  : qsTr("%1 · read by %2 at %3×").arg(reader.title).arg(dialog.voiceName)
                        .arg(narrator.speed.toLocaleString(Qt.locale(), 'f', 2))
            color: Colors.muted
            wrapMode: Text.Wrap
            font.pixelSize: 13
        }

        // The choices, before an export starts.
        ColumnLayout {
            Layout.fillWidth: true
            visible: !exporter.running
            spacing: 2

            Label { Layout.topMargin: 8; text: qsTr("Format"); color: Colors.body; font.weight: Font.Medium }
            RadioButton {
                text: qsTr("MP3, plays everywhere")
                checked: exporter.format === Exporter.Mp3
                onToggled: if (checked) exporter.format = Exporter.Mp3
            }
            RadioButton {
                text: qsTr("M4B audiobook (AAC)")
                checked: exporter.format === Exporter.M4b
                onToggled: if (checked) exporter.format = Exporter.M4b
            }

            Label {
                Layout.topMargin: 8
                text: qsTr("Files")
                color: Colors.body
                font.weight: Font.Medium
            }
            RadioButton {
                text: qsTr("One file with chapter markers")
                checked: exporter.layout === Exporter.SingleFile
                onToggled: if (checked) exporter.layout = Exporter.SingleFile
            }
            RadioButton {
                text: qsTr("A folder with one file per chapter")
                checked: exporter.layout === Exporter.ChapterFiles
                onToggled: if (checked) exporter.layout = Exporter.ChapterFiles
            }

            Label { Layout.topMargin: 8; text: qsTr("Save to"); color: Colors.body; font.weight: Font.Medium }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    text: exporter.folder
                    elide: Text.ElideMiddle
                    font.family: "monospace"
                    font.pixelSize: 12
                    color: Colors.text
                }
                Button {
                    flat: true
                    text: qsTr("Change…")
                    onClicked: exporter.chooseFolder()
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.topMargin: 8
                text: narrator.available
                      ? qsTr("About %1 of audio, read by %2. The whole book is read aloud first, so a long book takes a while; you can keep reading meanwhile.")
                            .arg(dialog.duration(reader.audioMinutes(narrator.speed)))
                            .arg(narrator.engines.find(e => e.id === narrator.engine).name)
                      : narrator.problem
                wrapMode: Text.Wrap
                color: narrator.available ? Colors.muted : "#f2d4d6"
                font.pixelSize: 13
            }
        }

        // Progress, while an export runs.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: 8
            visible: exporter.running
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: exporter.chapter > exporter.chapterCount
                      ? qsTr("Putting the chapters together…")
                      : qsTr("Chapter %1 of %2: %3").arg(Math.max(1, exporter.chapter)).arg(exporter.chapterCount)
                            .arg(exporter.chapterTitle)
                elide: Text.ElideRight
                color: Colors.text
            }
            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: 1
                value: exporter.progress
            }
            Label {
                text: qsTr("%1% done. Closing this window keeps the export going.").arg(Math.floor(exporter.progress * 100))
                color: Colors.muted
                font.pixelSize: 13
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 12
            spacing: 8
            Item { Layout.fillWidth: true }
            Button {
                flat: true
                text: exporter.running ? qsTr("Cancel export") : qsTr("Close")
                onClicked: {
                    if (exporter.running)
                        exporter.cancel()
                    else
                        dialog.close()
                }
            }
            AccentButton {
                id: start
                visible: !exporter.running
                enabled: narrator.available && reader.hasBook
                text: qsTr("Export")
                onClicked: reader.exportAudio()
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed: clicked()
            }
            AccentButton {
                id: hide
                visible: exporter.running
                text: qsTr("Hide")
                onClicked: dialog.close()
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed: clicked()
            }
        }
    }

    Connections {
        target: exporter
        // A finished or failed export reports in the window, not here.
        function onFinished(ok) {
            if (ok || exporter.error !== "")
                dialog.close()
        }
    }
}
