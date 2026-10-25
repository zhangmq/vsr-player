// StereoOsd.qml — 3D HotKeys 帮助覆盖层
//
// 全屏窗口由 StereoController 在所选显示器上创建；本文件只负责把同一份
// HotKeys 画在左右两半：SBS 下左半进左眼、右半进右眼，两只眼看到相同内容。
// OSD 不参与任何视频/3D 映射。
import QtQuick
import "ui/components"

Item {
    id: root
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0

    // 交叉视差（logical px）：融合后 HotKeys 看起来浮在视频画面前方。
    readonly property int parallax: 12

    Repeater {
        model: 2
        delegate: Item {
            x: index * root.width / 2
            width: root.width / 2
            height: root.height
            visible: stereo.osdVisible

            Rectangle {
                id: panel
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: (index === 0 ? 1 : -1) * root.parallax
                width: Math.min(parent.width - 60, 920)
                height: Math.min(parent.height - 60, body.implicitHeight + 56)
                color: "#cc111111"
                radius: 14
                border.width: 1
                border.color: "#33ffffff"

                Column {
                    id: body
                    anchors.centerIn: parent
                    width: parent.width - 56
                    spacing: 10

                    Text {
                        width: parent.width
                        text: qsTr("HotKeys")
                        color: "#ffcc00"
                        font.pixelSize: 32
                        font.bold: true
                    }
                    Rectangle {
                        width: parent.width; height: 1
                        color: "#33ffffff"
                    }

                    HotkeyRow {
                        width: parent.width
                        keyText: "Tab"
                        actionText: qsTr("Toggle HotKeys Help")
                    }
                    HotkeyRow {
                        width: parent.width
                        keyText: "J"
                        options: [qsTr("Side-by-Side"), qsTr("Top-and-Bottom")]
                        current: stereo.layout
                    }
                    HotkeyRow {
                        width: parent.width
                        keyText: "K"
                        options: [qsTr("Normal Eyes"), qsTr("Swapped Eyes")]
                        current: stereo.swap ? 1 : 0
                    }
                    HotkeyRow {
                        width: parent.width
                        keyText: "L"
                        options: [qsTr("Full"), qsTr("Half")]
                        current: stereo.half ? 1 : 0
                    }
                }
            }
        }
    }
}
