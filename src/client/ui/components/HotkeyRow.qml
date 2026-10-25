import QtQuick

/// HotKeys 列表的一行：键名 + 选项按钮（画质选项按钮同款样式）。
/// OSD 是眼镜上的键盘 UI，按钮不接收鼠标；current 变化时整行淡黄闪一下。
Item {
    id: root
    property string keyText: ""
    property var options: []
    property int current: 0
    property string actionText: ""
    property int keyWidth: 50
    property int fontPixelSize: 14
    implicitHeight: 30
    implicitWidth: row.implicitWidth

    Rectangle {
        id: flash
        anchors.fill: parent
        radius: 5
        color: "#ffcc00"
        opacity: 0
    }
    Row {
        id: row
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        Text {
            width: root.keyWidth
            text: root.keyText
            color: "#ffcc00"
            font.pixelSize: root.fontPixelSize
            font.family: "monospace"
            font.bold: true
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            visible: root.actionText !== ""
            text: root.actionText
            color: "#e0e0e0"
            font.pixelSize: root.fontPixelSize
            anchors.verticalCenter: parent.verticalCenter
        }
        Repeater {
            model: root.actionText === "" ? root.options : []
            delegate: Rectangle {
                width: Math.max(48, label.implicitWidth + 16)
                height: 26
                radius: 4
                color: index === root.current ? "#33ffcc00" : "transparent"
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    id: label
                    anchors.centerIn: parent
                    text: modelData
                    color: index === root.current ? "#ffcc00" : "#e0e0e0"
                    font.pixelSize: root.fontPixelSize
                }
            }
        }
    }
    SequentialAnimation {
        id: flashAnim
        NumberAnimation {
            target: flash; property: "opacity"
            from: 0.18; to: 0; duration: 500
            easing.type: Easing.OutQuad
        }
    }
    onCurrentChanged: flashAnim.restart()
}
