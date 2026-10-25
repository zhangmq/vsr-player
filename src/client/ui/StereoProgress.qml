// StereoProgress.qml — 3D 进度提示（快捷键快进/快退时短暂显示）
//
// 与 StereoVolume 同一套机制与视觉：底部居中小面板、1.5s 自动隐藏、
// 3D 左右眼各一份（±12px 交叉视差）。只在 **3D** 下显示——2D 桌面 UI 本身
// 有进度条，不需要这层提示。
//
// 触发源 = viewModel.seekNudged()（只有快捷键路径发；拖进度条走 seekAbsolute 不发）。
import QtQuick

Item {
    id: root
    property bool active: false
    property real position: 0     // 当前时间（ms）
    property real duration: 0     // 总时长（ms）
    property bool shown: false
    visible: active
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 150 } }

    function flash() {
        if (!active)
            return
        shown = true
        hideTimer.restart()
    }

    // 与 PlayerViewModel::fmtTime 同风格：>=1h 用 H:MM:SS，否则 M:SS
    function fmt(ms) {
        var s = Math.floor(Math.max(0, ms) / 1000)
        var h = Math.floor(s / 3600)
        var m = Math.floor((s % 3600) / 60)
        var sec = s % 60
        var p2 = function (n) { return (n < 10 ? "0" : "") + n }
        return h > 0 ? h + ":" + p2(m) + ":" + p2(sec) : m + ":" + p2(sec)
    }

    Timer {
        id: hideTimer
        interval: 1500
        onTriggered: root.shown = false
    }

    Connections {
        target: viewModel
        function onSeekNudged() { root.flash() }
    }

    Repeater {
        model: stereo.active ? 2 : 1
        delegate: Item {
            x: stereo.active ? index * root.width / 2 : 0
            width: stereo.active ? root.width / 2 : root.width
            height: root.height

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.horizontalCenterOffset: stereo.active ? (index === 0 ? 1 : -1) * 12 : 0
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 96
                width: 300
                height: 56
                radius: 10
                color: "#cc111111"
                border.width: 1
                border.color: "#33ffffff"

                Row {
                    anchors.centerIn: parent
                    spacing: 14

                    Rectangle {
                        width: 150
                        height: 6
                        radius: 3
                        color: "#44ffffff"
                        anchors.verticalCenter: parent.verticalCenter

                        Rectangle {
                            width: parent.width * (root.duration > 0
                                ? Math.max(0, Math.min(1, root.position / root.duration)) : 0)
                            height: 6
                            radius: 3
                            color: "#e0e0e0"
                        }
                    }
                    Text {
                        text: root.fmt(root.position) + " / " + root.fmt(root.duration)
                        color: "#e0e0e0"
                        font.pixelSize: 13
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }
    }
}
