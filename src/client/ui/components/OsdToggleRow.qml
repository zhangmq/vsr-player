import QtQuick

/// OSD 两态切换行：标签 + 分段按钮（选中项即当前状态）。
/// 视觉对齐 QualityPopup 的档位按钮（圆角 4、选中 #33ffcc00 底 + #ffcc00 字），
/// OSD 是眼镜上的键盘 UI，按钮仅展示状态、不接收鼠标；快捷键切换后 current
/// 变化会触发一次淡黄闪动，作为即时反馈。
Item {
    id: root
    property string label: ""
    property string key: ""
    property var labels: []
    property int current: 0
    property int buttonWidth: 220
    implicitHeight: 46
    implicitWidth: row.implicitWidth

    // 闪一下：快捷键切完能立刻看到变化（在按钮底下，不影响文字/选中色）。
    Rectangle {
        id: flash
        anchors.fill: parent
        radius: 6
        color: "#ffcc00"
        opacity: 0
    }
    Row {
        id: row
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10
        Text {
            width: 126
            text: root.label
            color: "#e0e0e0"
            font.pixelSize: 21
            font.weight: Font.DemiBold
            anchors.verticalCenter: parent.verticalCenter
        }
        Repeater {
            model: root.labels
            delegate: Rectangle {
                width: root.buttonWidth
                height: 40
                radius: 4
                color: root.current === index ? "#33ffcc00" : "transparent"
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    anchors.centerIn: parent
                    text: modelData
                    color: root.current === index ? "#ffcc00" : "#e0e0e0"
                    font.pixelSize: 19
                    font.weight: Font.DemiBold
                }
            }
        }
        Text {
            width: 44
            text: root.key
            color: "#ffcc00"
            font.pixelSize: 20
            font.family: "monospace"
            font.bold: true
            anchors.verticalCenter: parent.verticalCenter
        }
    }
    SequentialAnimation {
        id: flashAnim
        NumberAnimation {
            target: flash; property: "opacity"
            from: 0.20; to: 0; duration: 550
            easing.type: Easing.OutQuad
        }
    }
    onCurrentChanged: flashAnim.restart()
}
