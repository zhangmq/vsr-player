import QtQuick

/// HotKeys 列表的一行：固定宽度的键名列 + 纯文本状态/说明。
/// 状态只用颜色区分（选中 #ffcc00，未选 #e0e0e0），不用按钮，避免破坏
/// 快捷键列与状态列的对齐。current 变化时整行淡黄闪一下。
Item {
    id: root
    property string keyText: ""
    property var options: []
    property int current: 0
    property string actionText: ""
    property int keyWidth: 100
    property int fontPixelSize: 15
    implicitHeight: 32
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
            delegate: Text {
                text: modelData
                color: index === root.current ? "#ffcc00" : "#e0e0e0"
                font.pixelSize: root.fontPixelSize
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
