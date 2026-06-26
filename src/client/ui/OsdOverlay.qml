import QtQuick

Item {
    id: root
    property bool osdVisible: false
    property string osdText: ""

    Rectangle {
        anchors { left: parent.left; top: parent.top; leftMargin: 16; topMargin: 64 }
        color: "#99000000"; radius: 6
        opacity: root.osdVisible ? 1.0 : 0.0
        Behavior on opacity { OpacityAnimator { duration: 150 } }

        Text {
            text: root.osdText
            color: "#e0e0e0"
            font.family: "monospace"; font.pixelSize: 12
            lineHeight: 1.4
            leftPadding: 10; rightPadding: 10; topPadding: 8; bottomPadding: 8
            renderType: Text.NativeRendering
        }
    }
}
