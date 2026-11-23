import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Starts, renames or deletes a collection. Deleting one keeps its books in
// the library.
Popup {
    id: dialog
    objectName: "collectionDialog"
    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(440, (parent ? parent.width : 440) - 48)
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: Colors.surface
        radius: theme.radius
        border.color: Colors.line
    }

    // "new", "rename" or "delete".
    property string mode: "new"
    // The collection renamed or deleted.
    property string name: ""
    property string problem: ""

    function startNew() {
        dialog.mode = "new"
        dialog.name = ""
        nameField.text = ""
        dialog.problem = ""
        open()
    }

    function startRename(name) {
        dialog.mode = "rename"
        dialog.name = name
        nameField.text = name
        dialog.problem = ""
        open()
    }

    function startDelete(name) {
        dialog.mode = "delete"
        dialog.name = name
        dialog.problem = ""
        open()
    }

    function accept() {
        if (dialog.mode === "delete") {
            library.removeCollection(dialog.name)
            close()
        } else if (nameField.text.trim() === "") {
            return
        } else if (dialog.mode === "rename") {
            if (library.renameCollection(dialog.name, nameField.text))
                close()
            else
                dialog.problem = qsTr("Another collection already has that name.")
        } else if (library.addCollection(nameField.text) !== "") {
            close()
        }
    }

    onOpened: {
        if (dialog.mode === "delete") {
            buttons.forceActiveFocus()
        } else {
            nameField.forceActiveFocus()
            nameField.selectAll()
        }
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: dialog.mode === "rename" ? qsTr("Rename the collection")
                  : dialog.mode === "delete" ? qsTr("Delete “%1”?").arg(dialog.name)
                  : qsTr("New collection")
            font.pixelSize: 20
            font.family: "serif"
            color: Colors.text
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.mode === "delete"
            text: qsTr("The collection goes; its books stay in the library.")
            color: Colors.muted
            wrapMode: Text.Wrap
            font.pixelSize: 14
        }
        TextField {
            id: nameField
            objectName: "collectionNameField"
            Layout.fillWidth: true
            visible: dialog.mode !== "delete"
            placeholderText: qsTr("Name")
            onTextChanged: dialog.problem = ""
            onAccepted: dialog.accept()
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.problem !== ""
            text: dialog.problem
            color: theme.accent
            wrapMode: Text.Wrap
            font.pixelSize: 13
        }
        RowLayout {
            id: buttons
            Layout.fillWidth: true
            Layout.topMargin: 8
            spacing: 8
            Keys.onReturnPressed: dialog.accept()
            Keys.onEnterPressed: dialog.accept()
            Item { Layout.fillWidth: true }
            Button {
                flat: true
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }
            AccentButton {
                objectName: "collectionDialogAccept"
                text: dialog.mode === "rename" ? qsTr("Rename")
                      : dialog.mode === "delete" ? qsTr("Delete")
                      : qsTr("Create")
                enabled: dialog.mode === "delete" || nameField.text.trim() !== ""
                onClicked: dialog.accept()
            }
        }
    }
}
