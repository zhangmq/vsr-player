// StereoMessage.qml — 瞬态提示（键位操作反馈：画面比例 / 每眼比例等）
//
// 与 StereoProgress / StereoVolume **同一套布局**：底部居中小面板、1.5s 自动隐藏；
// 3D 下每一眼各一份 + ±12px 交叉视差。2D 用同一组件（active=false 时只画一份、
// 无视差、文案用 "Aspect …"）。
//
// ⚠ 3D 的每一份必须画在**各自那一半**里（delegate 宽 = 窗口一半、面板在本半
// 居中）。曾经写成"两份都在窗口中心、只偏 ±12px"——面板横跨中缝，每只眼只看
// 到半个面板（2026-11-03 用户报"比例 OSD 没有左右眼分别显示"）。
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

    Repeater {
        model: 2
        delegate: Item {
            // 2D：只留一份、占满整宽、居中；3D：左右两半各一份
            visible: root.active || index === 0
            x: root.active ? index * root.width / 2 : 0
            width: root.active ? root.width / 2 : root.width
            height: root.height

            Rectangle {
                id: panel
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.horizontalCenterOffset: root.active ? (index === 0 ? 1 : -1) * 12 : 0
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 96
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
