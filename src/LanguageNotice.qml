import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Says when the open book seems to be in a language the voice doesn't read,
// and offers an installed voice that does, so switching downloads nothing.
// Dismissing it is remembered for the book.
Rectangle {
    id: notice
    objectName: "languageNotice"

    readonly property var suggestion: narrator.languageSuggestion
    readonly property string kind: suggestion.kind || ""
    readonly property string book: inLanguage(suggestion.bookLanguage || "")

    visible: reader.hasBook && kind !== ""
    height: row.implicitHeight + 20
    radius: theme.radius
    color: Colors.raised
    border.color: theme.accent

    // A language as a sentence says it, "in Portuguese", from the English
    // name the narrator gives.
    function inLanguage(name) {
        switch (name) {
        case "English": return qsTr("in English")
        case "Portuguese": return qsTr("in Portuguese")
        case "Spanish": return qsTr("in Spanish")
        case "French": return qsTr("in French")
        case "Italian": return qsTr("in Italian")
        case "German": return qsTr("in German")
        case "Dutch": return qsTr("in Dutch")
        case "Russian": return qsTr("in Russian")
        case "Ukrainian": return qsTr("in Ukrainian")
        case "Polish": return qsTr("in Polish")
        case "Swedish": return qsTr("in Swedish")
        case "Turkish": return qsTr("in Turkish")
        case "Greek": return qsTr("in Greek")
        case "Arabic": return qsTr("in Arabic")
        case "Hebrew": return qsTr("in Hebrew")
        case "Hindi": return qsTr("in Hindi")
        case "Thai": return qsTr("in Thai")
        case "Chinese": return qsTr("in Chinese")
        case "Japanese": return qsTr("in Japanese")
        case "Korean": return qsTr("in Korean")
        }
        return name
    }

    readonly property string message: {
        const s = suggestion
        switch (kind) {
        case "voice":
            return qsTr("This book seems to be written %1, but %2 reads %3.")
                .arg(book).arg(s.voiceName).arg(inLanguage(s.voiceLanguage))
        case "language":
            return qsTr("This book seems to be written %1, but %2 is set to read %3.")
                .arg(book).arg(s.voiceName).arg(inLanguage(s.voiceLanguage))
        case "engine":
            return qsTr("This book seems to be written %1, which no %2 voice reads. %3 on %4 does.")
                .arg(book).arg(s.engineName).arg(s.targetVoiceName).arg(s.targetEngineName)
        case "none":
            return qsTr("This book seems to be written %1, and no installed voice reads it.").arg(book)
        }
        return ""
    }
    readonly property string hint: kind === "none" && suggestion.installCommand
                                    ? qsTr("%1 reads it once installed: %2")
                                        .arg(suggestion.targetEngineName).arg(suggestion.installCommand)
                                    : ""
    readonly property string action: {
        switch (kind) {
        case "voice": return qsTr("Read with %1").arg(suggestion.targetVoiceName)
        case "language": return qsTr("Read %1").arg(book)
        case "engine": return qsTr("Switch to %1").arg(suggestion.targetEngineName)
        }
        return ""
    }

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 8
        anchors.topMargin: 10
        anchors.bottomMargin: 10
        spacing: 10

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                objectName: "languageNoticeText"
                Layout.fillWidth: true
                text: notice.message
                wrapMode: Text.Wrap
                color: Colors.text
                font.pixelSize: 13
                textFormat: Text.PlainText
            }
            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                text: notice.hint
                wrapMode: Text.Wrap
                color: Colors.muted
                font.pixelSize: 12
                textFormat: Text.PlainText
            }
        }
        AccentButton {
            objectName: "languageNoticeAccept"
            visible: notice.action.length > 0
            implicitHeight: 34
            text: notice.action
            onClicked: narrator.acceptLanguageSuggestion()
        }
        ToolButton {
            objectName: "languageNoticeDismiss"
            text: "✕"
            onClicked: narrator.dismissLanguageSuggestion()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Don't suggest a voice for this book again")
        }
    }
}
