// Toast.qml — 瞬态提示（键位操作反馈：VSR / FRUC 档位、SDR→HDR10 开关）
//
// 与 StereoMessage **同一套外观**（底部居中小面板、1.5 s 自动隐藏），但文案
// 是调用方给的**完整文本**（不翻译，与 OSD 状态行同口径：`VSR ×2` / `FRUC off`
// / `SDR→HDR10 on`）。3D 下每一眼各一份 + ±12px 交叉视差，且必须画在各自那
// 一半里（同 StereoMessage 的教训：面板横跨中缝会被切成两半）。
//
// 触发源 = viewModel.toastFlashed(text)（KeyRouter 在档位快捷键里发）。
import QtQuick

Item {
    id: root
    property bool active: false          // 这一份是否值班（2D = 主那份，3D = 每眼那份）
    property bool perEye: false          // true = 每眼各画一份（只在 3D 打开）
    property bool shown: false
    property string text: ""

    visible: active
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 150 } }

    function flash(t) {
        if (!active || !t)
            return
        root.text = t
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
            visible: root.perEye || index === 0
            x: root.perEye ? index * root.width / 2 : 0
            width: root.perEye ? root.width / 2 : root.width
            height: root.height

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.horizontalCenterOffset: root.perEye ? (index === 0 ? 1 : -1) * 12 : 0
                anchors.bottom: parent.bottom
                // 比 StereoMessage（96）高一档：比例/音量提示可能同时出现
                anchors.bottomMargin: 150
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
