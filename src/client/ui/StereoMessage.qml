// StereoMessage.qml — 瞬态提示（键位操作反馈：画面比例 / 每眼比例等）
//
// 与 StereoProgress / StereoVolume 同一套视觉：底部居中小面板、1.5s 自动隐藏；
// 3D 下左右眼各一份 + ±12px 交叉视差（与其它 OSD 一致）。2D 用同一组件
//（active=false 时只画一份、无视差）。
//
// 触发源 = viewModel.messageFlashed(value)；文案在这里组合（qsTr，便于 lupdate）。
import QtQuick

Item {
    id: root
    property bool active: false          // true = 3D（presentation 内，双眼各一份）
    property bool shown: false
    property string value: ""            // "no" | "16:9" | "4:3" | "2.35:1"
    property string perEyeLabel: qsTr("Per-eye aspect %1")
    property string label: qsTr("Aspect %1")

    visible: active
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 150 } }

    readonly property string text: value === "no"
        ? (active ? qsTr("Per-eye aspect: no override") : qsTr("Aspect: no override"))
        : (active ? perEyeLabel.arg(value) : label.arg(value))

    function flash(v) {
        if (!active)
            return
        root.value = v
        root.shown = true
        hideTimer.restart()
    }

    Timer {
        id: hideTimer
        interval: 1500
        onTriggered: root.shown = false
    }

    // 面板：与 StereoProgress 同尺寸/配色
    Repeater {
        model: root.active ? 2 : 1
        delegate: Item {
            width: panel.width
            height: panel.height
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 96
            x: root.active ? (index === 0 ? -12 : 12) : 0

            Rectangle {
                id: panel
                width: msg.implicitWidth + 32
                height: 56
                radius: 6
                color: "#dd202020"
                border.width: 1
                border.color: "#33ffffff"
                Text {
                    id: msg
                    anchors.centerIn: parent
                    text: root.text
                    color: "#e0e0e0"
                    font.pixelSize: 18
                }
            }
        }
    }
}
