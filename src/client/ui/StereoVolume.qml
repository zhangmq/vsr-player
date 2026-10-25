// StereoVolume.qml — 3D 音量提示（独立于 3D 快捷键帮助面板）
//
// 参考系统音量 OSD：底部居中的小面板，扬声器图标 + 横向音量条 + 百分比。
// 2D 一份，3D 左右眼各一份（带和帮助面板相同的 ±12px 交叉视差）。
// 只在音量变化时短暂显示，与 helpVisible 完全无关。
import QtQuick

Item {
    id: root
    property bool active: false
    property real volume: 1.0
    property bool muted: false
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

    Timer {
        id: hideTimer
        interval: 1500
        onTriggered: root.shown = false
    }

    Connections {
        target: viewModel
        // 只认"快捷键改音量"：UI 滑杆拖动不弹提示。
        function onVolumeNudged() { root.flash() }
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

                    Text {
                        text: root.muted ? "󰖁" : "󰕾"
                        color: "#e0e0e0"
                        font.family: iconFont
                        font.pixelSize: 26
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Rectangle {
                        width: 170
                        height: 6
                        radius: 3
                        color: "#44ffffff"
                        anchors.verticalCenter: parent.verticalCenter

                        Rectangle {
                            width: parent.width * Math.max(0, Math.min(1, root.volume))
                            height: 6
                            radius: 3
                            color: "#e0e0e0"
                        }
                    }
                    Text {
                        text: Math.round(root.volume * 100) + "%"
                        color: "#e0e0e0"
                        font.pixelSize: 13
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }
    }
}
