import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Adds a web page to the library from its address. The page is fetched only
// when the user asks, and its article shown here to check, and to fix the
// title, before it is saved; from then on it reads offline like pasted text.
Popup {
    id: dialog
    modal: true
    focus: true
    closePolicy: Popup.CloseOnPressOutside
    anchors.centerIn: Overlay.overlay
    width: Math.min(640, (parent ? parent.width : 640) - 48)
    height: Math.min(600, (parent ? parent.height : 600) - 48)
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: Colors.surface
        radius: theme.radius
        border.color: Colors.line
    }

    readonly property bool reviewing: webImport.hasArticle
    readonly property bool hasText: textArea.text.trim() !== ""
    readonly property bool canAdd: dialog.reviewing && dialog.hasText && !webImport.busy && !reader.loading

    // Opens empty, or with a copied link already in the address.
    function start(address) {
        webImport.cancel()
        webImport.clear()
        addressField.text = address || ""
        textArea.clear()
        titleField.clear()
        open()
    }

    function fetch() {
        if (addressField.text.trim() === "" || webImport.busy)
            return
        textArea.clear()
        titleField.clear()
        webImport.fetch(addressField.text)
    }

    function add() {
        if (!dialog.canAdd)
            return
        if (reader.addText(titleField.text, textArea.text, webImport.source, webImport.author))
            close()
    }

    // Esc stops a fetch first, and closes the dialog after.
    function dismiss() {
        if (webImport.busy)
            webImport.cancel()
        else
            close()
    }

    onOpened: {
        addressField.forceActiveFocus()
        addressField.selectAll()
    }
    // Nothing of the page stays around once the dialog is gone.
    onClosed: {
        webImport.cancel()
        webImport.clear()
        textArea.clear()
        titleField.clear()
    }

    Connections {
        target: webImport
        function onFetched() {
            if (!dialog.opened)
                return
            textArea.text = webImport.text
            textArea.cursorPosition = 0
            titleField.text = webImport.title
            titleField.forceActiveFocus()
            titleField.selectAll()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            text: qsTr("Add a web page")
            font.pixelSize: 20
            font.family: "serif"
            color: Colors.text
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Paste the address of an article. omareader fetches the page once, keeps its text in your library and reads it aloud offline from then on.")
            color: Colors.muted
            wrapMode: Text.Wrap
            font.pixelSize: 13
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 8
            TextField {
                id: addressField
                objectName: "urlAddress"
                Layout.fillWidth: true
                placeholderText: "https://"
                selectByMouse: true
                color: Colors.text
                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                Keys.onReturnPressed: event => {
                    if ((event.modifiers & Qt.ControlModifier) && dialog.canAdd)
                        dialog.add()
                    else
                        dialog.fetch()
                }
                Keys.onEnterPressed: dialog.fetch()
                Keys.onEscapePressed: dialog.dismiss()
            }
            Button {
                objectName: "urlFetch"
                flat: true
                text: qsTr("Fetch")
                enabled: addressField.text.trim() !== "" && !webImport.busy
                onClicked: dialog.fetch()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Fetch the page (Enter)")
            }
        }

        // What the fetch is doing, or why it failed.
        RowLayout {
            Layout.fillWidth: true
            visible: webImport.busy
            spacing: 8
            BusyIndicator {
                running: webImport.busy
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Fetching %1…").arg(webImport.fetching)
                color: Colors.muted
                font.pixelSize: 13
                elide: Text.ElideRight
            }
        }
        Rectangle {
            objectName: "urlError"
            Layout.fillWidth: true
            visible: webImport.error !== "" && !webImport.busy
            implicitHeight: errorLabel.implicitHeight + 20
            radius: Math.min(8, theme.radius)
            color: "#2a1618"
            border.color: "#a8444c"
            Label {
                id: errorLabel
                anchors.fill: parent
                anchors.margins: 10
                text: webImport.error
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: "#f2d4d6"
                font.pixelSize: 13
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Math.min(8, theme.radius)
            color: Colors.background
            border.color: textArea.activeFocus ? theme.accent : Colors.line
            Label {
                x: 13
                y: 13
                width: parent.width - 26
                visible: textArea.text === ""
                text: qsTr("The article's text shows here, to check before it is added.")
                color: Colors.muted
                wrapMode: Text.Wrap
                font.pixelSize: 15
            }
            ScrollView {
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                TextArea {
                    id: textArea
                    objectName: "urlText"
                    enabled: dialog.reviewing
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
                            dialog.dismiss()
                            event.accepted = true
                        }
                    }
                }
            }
        }

        TextField {
            id: titleField
            objectName: "urlTitle"
            Layout.fillWidth: true
            Layout.topMargin: 6
            enabled: dialog.reviewing
            placeholderText: qsTr("Title")
            selectByMouse: true
            color: Colors.text
            Keys.onReturnPressed: dialog.add()
            Keys.onEnterPressed: dialog.add()
            Keys.onEscapePressed: dialog.dismiss()
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.reviewing
            text: webImport.author !== ""
                  ? qsTr("From %1, by %2").arg(webImport.site).arg(webImport.author)
                  : qsTr("From %1").arg(webImport.site)
            color: Colors.muted
            font.pixelSize: 12
            elide: Text.ElideRight
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 8
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: dialog.reviewing ? qsTr("Enter or Ctrl+Enter adds · Esc cancels")
                                       : qsTr("Enter fetches · Esc cancels")
                color: Colors.muted
                font.pixelSize: 12
                elide: Text.ElideRight
            }
            Button {
                objectName: "urlCancel"
                flat: true
                text: webImport.busy ? qsTr("Stop") : qsTr("Cancel")
                onClicked: dialog.dismiss()
            }
            AccentButton {
                objectName: "urlAdd"
                enabled: dialog.canAdd
                text: qsTr("Add to library")
                onClicked: dialog.add()
            }
        }
    }
}
