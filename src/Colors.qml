pragma Singleton
import QtQuick

// The reading colours: a dark page that sits with Omarchy's themes, with the
// theme's accent reserved for the highlight and the controls.
QtObject {
    readonly property color background: "#0e0e10"
    readonly property color surface: "#16161a"
    readonly property color raised: "#1f1f24"
    readonly property color line: "#2a2a30"
    readonly property color text: "#e4e4e7"
    readonly property color body: "#d6d6da"
    readonly property color muted: "#8b8b94"
}
