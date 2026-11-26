import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1100
    height: 780
    minimumWidth: 640
    minimumHeight: 480
    visible: true
    title: reader.hasBook ? qsTr("%1 · omareader").arg(reader.title) : "omareader"
    color: Colors.background

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.background: Colors.surface
    Material.roundedScale: theme.radius

    property bool helpVisible: false

    readonly property string message: reader.error !== "" ? reader.error
                                    : narrator.error !== "" ? narrator.error : exporter.error

    function dismissMessage() {
        reader.clearError()
        narrator.clearError()
        exporter.clearError()
    }

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequences: ["O", StandardKey.Open]; context: Qt.ApplicationShortcut; onActivated: reader.openDialog() }
    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (reader.loading)
                reader.cancelLoading()
            else if (statsPanel.opened)
                statsPanel.close()
            else if (pasteDialog.opened)
                pasteDialog.close()
            else if (urlDialog.opened)
                urlDialog.dismiss()
            else if (!reader.hasBook && libraryView.closeDialogs())
                return
            else if (win.helpVisible)
                win.helpVisible = false
            else if (win.message !== "")
                win.dismissMessage()
            else if (exporter.result !== "")
                exporter.clearResult()
            else if (!reader.hasBook)
                libraryView.clearFilter()
            else
                readerView.handleEscape()
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        LibraryView {
            id: libraryView
            anchors.fill: parent
            visible: !reader.hasBook
            focus: visible
            dialogOpen: pasteDialog.opened || urlDialog.opened || statsPanel.opened
            onAddTextRequested: fromClipboard => pasteDialog.start(fromClipboard)
            onAddPageRequested: address => urlDialog.start(address)
            onStatsRequested: statsPanel.toggle()
        }

        ReaderView {
            id: readerView
            anchors.fill: parent
            visible: reader.hasBook
            focus: visible
            onExportRequested: exportDialog.open()
            onStatsRequested: statsPanel.toggle()
        }

        DropArea {
            id: drop
            anchors.fill: parent
            keys: ["text/uri-list"]
            onDropped: drop => {
                if (drop.hasUrls && drop.urls.length > 0) {
                    reader.open(drop.urls[0].toString())
                    drop.acceptProposedAction()
                }
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: 16
                visible: drop.containsDrag
                color: Qt.alpha(theme.accent, 0.08)
                border.color: theme.accent
                border.width: 2
                radius: theme.radius
                Label {
                    anchors.centerIn: parent
                    text: qsTr("Drop to open")
                    font.pixelSize: 22
                    color: theme.accent
                }
            }
        }

        Rectangle {
            id: loadingOverlay
            objectName: "loadingOverlay"
            anchors.fill: parent
            visible: reader.loading
            color: Qt.alpha(Colors.background, loadingOverlay.recognising ? 0.85 : 0.7)
            // A scanned PDF has its text recognised page by page first,
            // which takes a while, so it shows how far it has got, on a card
            // of its own so the library behind doesn't show through.
            readonly property bool recognising: reader.pagesToRecognise > 0
            MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }
            Rectangle {
                visible: loadingOverlay.recognising
                // The flat Stop button carries padding of its own below.
                x: loadingColumn.x - 32
                y: loadingColumn.y - 28
                width: loadingColumn.width + 64
                height: loadingColumn.height + 40
                radius: theme.radius
                color: Colors.surface
                border.color: Colors.line
            }
            ColumnLayout {
                id: loadingColumn
                anchors.centerIn: parent
                width: Math.min(parent.width - 112, 420)
                spacing: 16
                BusyIndicator {
                    visible: !loadingOverlay.recognising
                    running: reader.loading && visible
                    Layout.alignment: Qt.AlignHCenter
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: loadingOverlay.recognising ? qsTr("Recognising the text of this scanned book…")
                                                     : qsTr("Opening book…")
                    color: loadingOverlay.recognising ? Colors.text : Colors.muted
                    font.pixelSize: loadingOverlay.recognising ? 16 : font.pixelSize
                }
                ProgressBar {
                    objectName: "recognitionProgress"
                    visible: loadingOverlay.recognising
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(1, reader.pagesToRecognise)
                    value: reader.pagesRecognised
                }
                Label {
                    visible: loadingOverlay.recognising
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: qsTr("Page %1 of %2. This happens once: the text is kept, and stopping keeps the pages done.")
                              .arg(reader.pagesRecognised).arg(reader.pagesToRecognise)
                    color: Colors.muted
                    font.pixelSize: 13
                }
                Button {
                    objectName: "stopRecognitionButton"
                    visible: loadingOverlay.recognising
                    flat: true
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Stop")
                    onClicked: reader.cancelLoading()
                }
            }
        }

        // What went wrong stays on screen until it's dismissed.
        Rectangle {
            id: banner
            visible: win.message !== ""
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: reader.hasBook ? 104 : 28
            width: Math.min(parent.width - 48, 720)
            height: bannerRow.implicitHeight + 24
            radius: theme.radius
            color: "#2a1618"
            border.color: "#a8444c"
            RowLayout {
                id: bannerRow
                anchors.fill: parent
                anchors.margins: 12
                spacing: 12
                Label {
                    Layout.fillWidth: true
                    text: win.message
                    wrapMode: Text.Wrap
                    color: "#f2d4d6"
                    textFormat: Text.PlainText
                }
                ToolButton {
                    text: "✕"
                    onClicked: win.dismissMessage()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Dismiss (Esc)")
                }
            }
        }

        // An export runs on after its dialog closes. In the library this shows
        // how far along it is (the reader's title bar shows that itself), and
        // anywhere it says where the audio went once it's done.
        Rectangle {
            id: exportNotice
            visible: exporter.running ? !reader.hasBook : exporter.result !== ""
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: (reader.hasBook ? 104 : 64) + (banner.visible ? banner.height + 8 : 0)
            width: Math.min(noticeRow.implicitWidth + 24, parent.width - 48)
            height: noticeRow.implicitHeight + 16
            radius: theme.radius
            color: Colors.raised
            border.color: exporter.running ? Colors.line : theme.accent
            RowLayout {
                id: noticeRow
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8
                BusyIndicator {
                    visible: exporter.running
                    running: visible
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22
                }
                Label {
                    Layout.maximumWidth: 420
                    text: exporter.running
                          ? qsTr("Exporting %1 · %2%").arg(exporter.bookTitle).arg(Math.floor(exporter.progress * 100))
                          : qsTr("Saved %1").arg(exporter.result)
                    elide: Text.ElideMiddle
                    color: Colors.text
                    font.pixelSize: 13
                }
                Button {
                    visible: !exporter.running
                    flat: true
                    text: qsTr("Show in folder")
                    onClicked: exporter.showResult()
                }
                ToolButton {
                    visible: !exporter.running
                    text: "✕"
                    onClicked: exporter.clearResult()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Dismiss (Esc)")
                }
            }
        }

        // The voice for the book's language, above the messages.
        LanguageNotice {
            id: languageNotice
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 104 + (banner.visible ? banner.height + 8 : 0)
                                  + (exportNotice.visible ? exportNotice.height + 8 : 0)
            width: Math.min(parent.width - 48, 680)
        }

        ExportDialog {
            id: exportDialog
            objectName: "exportDialog"
        }

        PasteDialog {
            id: pasteDialog
            objectName: "pasteDialog"
        }

        UrlDialog {
            id: urlDialog
            objectName: "urlDialog"
        }

        StatsPanel {
            id: statsPanel
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            onClosed: win.helpVisible = false
        }
    }
}
