import QtQuick

Item {
    id: root
    property bool playing: false
    property bool overlaysVisible: true
    signal clicked()

    Rectangle {
        id: centerPlayBtn
        width: 72; height: 72; radius: 36
        x: (parent.width - width) / 2; y: (parent.height - height) / 2
        color: cpHover.hovered ? (cpMouse.pressed ? "#55000000" : "#44000000") : "#33000000"
        opacity: (!root.playing || root.overlaysVisible) ? 1.0 : 0.0
        Behavior on opacity { OpacityAnimator { duration: 200 } }

        Text {
            anchors.centerIn: parent
            font.family: iconFont; font.pixelSize: 36
            text: root.playing ? "󰏤" : "󰐊"; color: "#ffffff"   // 暂停→播放箭头；播放中→暂停条（hover 时可见）
            renderType: Text.NativeRendering
        }
        MouseArea {
            id: cpMouse; anchors.fill: parent
            cursorShape: Qt.PointingHandCursor; hoverEnabled: true
            onClicked: root.clicked()
        }
        HoverHandler { id: cpHover }
    }
}
