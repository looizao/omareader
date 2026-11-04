import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The table of contents.
Drawer {
    id: panel
    edge: Qt.LeftEdge
    width: Math.min(420, parent ? parent.width * 0.8 : 420)
    Material.roundedScale: theme.radius
    background: Rectangle { color: Colors.surface }

    onOpened: {
        chapters.currentIndex = reader.chapter
        chapters.positionViewAtIndex(reader.chapter, ListView.Center)
        chapters.forceActiveFocus()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            text: qsTr("Contents")
            font.pixelSize: 20
            font.family: "serif"
            color: Colors.text
        }
        Label {
            Layout.fillWidth: true
            text: reader.author !== "" ? qsTr("%1 by %2").arg(reader.title).arg(reader.author) : reader.title
            color: Colors.muted
            wrapMode: Text.Wrap
            font.pixelSize: 12
        }

        ListView {
            id: chapters
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: reader.chapterTitles
            keyNavigationEnabled: true
            highlightMoveDuration: 0
            ScrollBar.vertical: ScrollBar {}

            Keys.onReturnPressed: { reader.goToChapter(currentIndex); panel.close() }
            Keys.onEnterPressed: { reader.goToChapter(currentIndex); panel.close() }

            delegate: ItemDelegate {
                id: item
                required property string modelData
                required property int index
                width: ListView.view.width
                highlighted: ListView.isCurrentItem
                contentItem: RowLayout {
                    spacing: 10
                    Rectangle {
                        Layout.preferredWidth: 3
                        Layout.fillHeight: true
                        color: item.index === reader.chapter ? theme.accent : "transparent"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: item.modelData
                        elide: Text.ElideRight
                        color: item.index === reader.chapter ? Colors.text : Colors.body
                    }
                }
                onClicked: {
                    reader.goToChapter(index)
                    panel.close()
                }
            }
        }
    }
}
