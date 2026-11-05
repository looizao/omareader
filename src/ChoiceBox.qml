import QtQuick
import QtQuick.Controls

// A combo box that always shows `selected`: when its list arrives or changes
// (another engine's voices) and after a pick, which would otherwise break a
// plain currentIndex binding. A pick is only reported through chosen().
ComboBox {
    id: box
    property var selected
    signal chosen(var value)

    textRole: "label"
    valueRole: "id"

    function sync() {
        currentIndex = count > 0 ? indexOfValue(selected) : -1
    }
    onSelectedChanged: sync()
    onCountChanged: sync()
    onModelChanged: sync()
    Component.onCompleted: sync()
    onActivated: {
        chosen(currentValue)
        sync()
    }
}
