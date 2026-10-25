import QtQuick

/// HotKeys 列表的一行：键名 + 选项列表；当前选项用黄色/加粗标出。
/// OSD 是眼镜上的键盘 UI，不接收鼠标；current 变化时整行淡黄闪一下，
/// 作为快捷键切换后的即时反馈。
Item {
    id: root
    property string keyText: ""
    property var options: []
    property int current: 0
    property string actionText: ""
    implicitHeight: 42
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
        spacing: 14
        Text {
            width: 78
            text: root.keyText
            color: "#ffcc00"
            font.pixelSize: 24
            font.family: "monospace"
            font.bold: true
            anchors.verticalCenter: parent.verticalCenter
        }
        Repeater {
            model: root.actionText !== "" ? [root.actionText] : root.options
            delegate: Text {
                text: modelData
                color: root.actionText !== ""
                     ? "#e0e0e0"
                     : (index === root.current ? "#ffcc00" : "#e0e0e0")
                font.pixelSize: 24
                font.weight: (root.actionText === "" && index === root.current)
                             ? Font.Bold : Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
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
