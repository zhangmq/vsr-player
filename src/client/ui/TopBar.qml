import QtQuick

Item {
    id: root
    property string videoInfo: ""
    property bool overlaysVisible: true
    implicitHeight: 48

    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 48
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#cc000000" }
            GradientStop { position: 1.0; color: "transparent" }
        }
        opacity: root.overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

        Text {
            anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            font.pixelSize: 14; elide: Text.ElideRight
            color: "#e0e0e0"
            text: {
                if (root.videoInfo) return root.videoInfo
                return "VSR Player"
            }
        }
    }
}
