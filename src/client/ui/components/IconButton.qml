import QtQuick
import QtQuick.Controls

Item {
    id: root
    property string codepoint
    property real size: 22
    property string tooltip: ""
    property bool highlighted: false
    property string label: ""
    signal clicked()
    implicitWidth: label ? label.length * 12 + 20 : size + 16
    implicitHeight: size + 16

    Rectangle {
        id: ibBg; anchors.fill: parent; radius: 4
        color: ibHover.hovered || root.highlighted ? "#33ffffff" : "transparent"
        Behavior on color { ColorAnimation { duration: 150 } }
    }
    Text {
        anchors.centerIn: parent
        font.family: label ? "" : iconFont
        font.pixelSize: label ? size - 7 : size
        text: label ? label : codepoint
        color: ibHover.hovered || root.highlighted ? "#ffffff" : "#c8c8c8"
        Behavior on color { ColorAnimation { duration: 150 } }
        renderType: Text.NativeRendering
    }
    MouseArea {
        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
        hoverEnabled: true; onClicked: root.clicked()
    }
    HoverHandler { id: ibHover }
    ToolTip {
        visible: ibHover.hovered && tooltip !== ""; text: tooltip
        delay: 600; font.pixelSize: 11
        background: Rectangle { color: "#d9111111"; radius: 4; border { width: 1; color: "#22ffffff" } }
        contentItem: Text { text: tooltip; color: "#e0e0e0"; font.pixelSize: 11 }
    }
}
