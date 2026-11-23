import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// How much time goes into reading: the open book's time and what is left
// of it at the voice's speed, then the habits across every book, with the
// last seven days as bars. Everything here is counted on this computer and
// stays on it.
Popup {
    id: panel
    objectName: "statsPanel"
    modal: true
    focus: true
    closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape
    anchors.centerIn: Overlay.overlay
    width: Math.min(600, (parent ? parent.width : 600) - 48)
    height: Math.min(implicitHeight, (parent ? parent.height : 640) - 48)
    padding: 24
    Material.roundedScale: theme.radius
    background: Rectangle {
        color: Colors.surface
        radius: theme.radius
        border.color: Colors.line
    }

    readonly property var book: stats.current
    readonly property var overview: stats.overview
    // The tallest day sets the bars' scale; a quiet week still shows a little.
    readonly property real dayScale: {
        let most = 15 * 60
        for (const day of panel.overview.days)
            most = Math.max(most, day.reading + day.listening)
        return most
    }

    function toggle() {
        if (panel.opened)
            close()
        else
            open()
    }

    // The time up to now counts before the numbers show.
    onAboutToShow: stats.checkpoint()

    Shortcut { sequence: "I"; enabled: panel.opened; onActivated: panel.close() }

    // A number with what it counts beneath it.
    component Figure: ColumnLayout {
        property alias value: number.text
        property alias label: caption.text
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        spacing: 2
        Label {
            id: number
            Layout.fillWidth: true
            color: Colors.text
            font.pixelSize: 20
            elide: Text.ElideRight
        }
        Label {
            id: caption
            Layout.fillWidth: true
            color: Colors.muted
            font.pixelSize: 12
            elide: Text.ElideRight
        }
    }

    component Heading: Label {
        Layout.fillWidth: true
        color: Colors.muted
        font.pixelSize: 12
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 1
        elide: Text.ElideRight
    }

    contentItem: Flickable {
        implicitHeight: content.implicitHeight
        contentHeight: content.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: content
            width: parent.width
            spacing: 18

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Reading statistics")
                    font.family: "serif"
                    font.pixelSize: 24
                    color: Colors.text
                }
                ToolButton {
                    text: "✕"
                    onClicked: panel.close()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Close (Esc)")
                }
            }

            // The open book.
            ColumnLayout {
                objectName: "statsBook"
                visible: reader.hasBook
                Layout.fillWidth: true
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    text: reader.title
                    textFormat: Text.PlainText
                    color: Colors.text
                    font.family: "serif"
                    font.pixelSize: 17
                    elide: Text.ElideRight
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16
                    Figure {
                        objectName: "statsBookTotal"
                        value: stats.duration(panel.book.total || 0)
                        label: qsTr("With this book")
                    }
                    Figure {
                        value: stats.duration(panel.book.reading || 0)
                        label: qsTr("Reading")
                    }
                    Figure {
                        value: stats.duration(panel.book.listening || 0)
                        label: qsTr("Listening")
                    }
                    Figure {
                        objectName: "statsTimeLeft"
                        value: stats.duration(reader.minutesLeft * 60)
                        label: qsTr("Left at %1×").arg(narrator.speed.toLocaleString(Qt.locale(), 'f', 1))
                    }
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Colors.muted
                    font.pixelSize: 13
                    text: qsTr("%1% read · %2 left in this chapter")
                          .arg(Math.round(reader.progress * 100).toLocaleString(Qt.locale(), 'f', 0))
                          .arg(stats.duration(reader.minutesLeftInChapter * 60))
                }
                Label {
                    objectName: "statsBookDays"
                    visible: (panel.book.since || "") !== ""
                    Layout.fillWidth: true
                    Layout.topMargin: -8
                    wrapMode: Text.Wrap
                    color: Colors.muted
                    font.pixelSize: 13
                    text: qsTr("Read on %n day(s) since %1", "", panel.book.days || 0).arg(panel.book.since || "")
                }
            }

            Rectangle {
                visible: reader.hasBook
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Colors.line
            }

            // Habits across every book.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 12

                Heading { text: qsTr("Every book") }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16
                    Figure {
                        objectName: "statsToday"
                        value: stats.duration(panel.overview.today)
                        label: qsTr("Today")
                    }
                    Figure {
                        value: stats.duration(panel.overview.week)
                        label: qsTr("Last 7 days")
                    }
                    Figure {
                        objectName: "statsStreak"
                        value: qsTr("%n day(s)", "", panel.overview.streak)
                        label: qsTr("In a row")
                    }
                    Figure {
                        value: stats.duration(panel.overview.total)
                        label: qsTr("In all")
                    }
                }

                // The last seven days, reading and listening stacked.
                RowLayout {
                    objectName: "statsWeek"
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    spacing: 8
                    Repeater {
                        model: panel.overview.days
                        ColumnLayout {
                            id: day
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            spacing: 6
                            Item {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 72
                                Rectangle {
                                    anchors.fill: parent
                                    radius: Math.min(4, theme.radius)
                                    color: Colors.raised
                                }
                                Rectangle {
                                    objectName: "statsListeningBar"
                                    anchors.bottom: parent.bottom
                                    width: parent.width
                                    height: parent.height * day.modelData.listening / panel.dayScale
                                    radius: Math.min(4, theme.radius)
                                    color: theme.accent
                                }
                                Rectangle {
                                    objectName: "statsReadingBar"
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: parent.height * day.modelData.listening / panel.dayScale
                                    width: parent.width
                                    height: parent.height * day.modelData.reading / panel.dayScale
                                    radius: Math.min(4, theme.radius)
                                    color: Qt.alpha(theme.accent, 0.45)
                                }
                                ToolTip.visible: dayHover.hovered
                                ToolTip.text: qsTr("Reading %1 · listening %2")
                                              .arg(stats.duration(day.modelData.reading))
                                              .arg(stats.duration(day.modelData.listening))
                                HoverHandler { id: dayHover }
                            }
                            Label {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: day.index === 6 ? qsTr("Today") : day.modelData.label
                                color: day.index === 6 ? Colors.text : Colors.muted
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                Row {
                    spacing: 16
                    Repeater {
                        model: [[Qt.alpha(theme.accent, 0.45), qsTr("Reading")], [theme.accent, qsTr("Listening")]]
                        Row {
                            required property var modelData
                            spacing: 6
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 10
                                height: 10
                                radius: 2
                                color: parent.modelData[0]
                            }
                            Label {
                                text: parent.modelData[1]
                                color: Colors.muted
                                font.pixelSize: 12
                            }
                        }
                    }
                }
            }

            // Where the time went, in the library.
            ColumnLayout {
                visible: !reader.hasBook && panel.overview.books.length > 0
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    Layout.bottomMargin: 10
                    color: Colors.line
                }
                Heading { text: qsTr("Most time") }
                Repeater {
                    model: panel.overview.books
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 12
                        Label {
                            Layout.fillWidth: true
                            text: modelData.title
                            textFormat: Text.PlainText
                            color: Colors.text
                            elide: Text.ElideRight
                        }
                        Label {
                            text: stats.duration(modelData.total)
                            color: Colors.muted
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Colors.muted
                font.pixelSize: 12
                text: qsTr("Counted while the voice plays, or while the window is in front and you scroll or press keys. Kept on this computer only.")
            }
        }
    }
}
