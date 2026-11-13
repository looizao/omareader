import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A word looked up from the page and what each installed dictionary says
// about it, in a card beside the word. Without a dictionary it says how to
// install one; the window itself never downloads anything for it.
Popup {
    id: card
    objectName: "wordCard"

    // Where the word sits, in the parent's coordinates.
    property rect wordRect: Qt.rect(0, 0, 0, 0)
    readonly property real gap: 8
    readonly property real parentWidth: parent ? parent.width : 0
    readonly property real parentHeight: parent ? parent.height : 0
    readonly property real roomBelow: parentHeight - wordRect.y - wordRect.height - gap * 2
    readonly property real roomAbove: wordRect.y - gap * 2
    // Below the word when it fits there, otherwise wherever there is more room.
    readonly property bool below: roomBelow >= implicitHeight || roomBelow >= roomAbove

    width: Math.min(440, parentWidth - 32)
    height: Math.min(implicitHeight, Math.max(140, below ? roomBelow : roomAbove))
    x: Math.max(16, Math.min(parentWidth - width - 16, wordRect.x + wordRect.width / 2 - width / 2))
    y: below ? wordRect.y + wordRect.height + gap : Math.max(gap, wordRect.y - height - gap)
    padding: 16
    // The reader's keys keep working while it is open; Esc and D close it.
    focus: false
    modal: false
    closePolicy: Popup.CloseOnPressOutside
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: Colors.raised
        radius: theme.radius
        border.color: Colors.line
    }

    readonly property bool empty: !lookup.busy && lookup.entries.length === 0

    contentItem: ColumnLayout {
        spacing: 8

        Label {
            objectName: "wordCardWord"
            Layout.fillWidth: true
            text: lookup.word
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Colors.text
            font.family: "serif"
            font.pixelSize: 22
        }

        BusyIndicator {
            visible: lookup.busy
            running: visible
            Layout.preferredWidth: 28
            Layout.preferredHeight: 28
        }

        Label {
            objectName: "wordCardMessage"
            Layout.fillWidth: true
            visible: card.empty
            text: lookup.dictionaryCount === 0
                  ? qsTr("No dictionaries yet. To look words up and translate them offline, install one in a terminal:")
                  : qsTr("None of your dictionaries has this word.")
            wrapMode: Text.Wrap
            color: Colors.muted
            font.pixelSize: 14
        }

        // Selectable, so it can be copied into a terminal.
        TextEdit {
            objectName: "wordCardCommand"
            Layout.fillWidth: true
            visible: card.empty && lookup.dictionaryCount === 0
            text: lookup.setupCommand(translator.language)
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
            color: theme.accent
            selectionColor: theme.accent
            selectedTextColor: theme.accentForeground
            font.family: "monospace"
            font.pixelSize: 13
        }

        Flickable {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: entries.implicitHeight
            visible: lookup.entries.length > 0
            contentHeight: entries.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            ScrollBar.vertical: ScrollBar {}

            Column {
                id: entries
                width: scroller.width
                spacing: 6

                Repeater {
                    model: lookup.entries
                    Column {
                        objectName: "wordCardEntry"
                        required property var modelData
                        required property int index
                        width: entries.width
                        spacing: 4
                        topPadding: index > 0 && modelData.firstOfDictionary ? 10 : 0

                        Label {
                            width: parent.width
                            visible: modelData.firstOfDictionary
                            text: modelData.dictionary
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: theme.accent
                            font.pixelSize: 12
                        }
                        // The form the dictionary had, when it isn't the word
                        // as written ("run" for "running").
                        Label {
                            width: parent.width
                            visible: modelData.headword.toLowerCase() !== lookup.word.toLowerCase()
                            text: modelData.headword
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            color: Colors.text
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                        }
                        Label {
                            objectName: "wordCardText"
                            width: parent.width
                            text: modelData.text
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            color: Colors.body
                            font.pixelSize: 15
                            lineHeight: 1.15
                        }
                    }
                }
            }
        }
    }
}
