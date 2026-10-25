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

            // 内容宽度以最长行为准（ids 都在本 delegate 内，不能放到 root）。
            readonly property real contentWidth: Math.max(
                titleText.implicitWidth,
                tabRow.implicitWidth,
                layoutRow.implicitWidth,
                eyesRow.implicitWidth,
                halfRow.implicitWidth,
                playerLabel.implicitWidth,
                playRow.implicitWidth,
                seekRow.implicitWidth,
                seekShiftRow.implicitWidth,
                volumeRow.implicitWidth)

            Rectangle {
                id: panel
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: (index === 0 ? 1 : -1) * root.parallax
                width: Math.min(parent.width - 60, contentWidth + 56)
                height: Math.min(parent.height - 60, body.implicitHeight + 56)
                color: "#cc111111"
                radius: 14
                border.width: 1
                border.color: "#33ffffff"

                Column {
                    id: body
                    anchors.centerIn: parent
                    width: contentWidth
                    spacing: 10

                    Text {
                        id: titleText
                        text: qsTr("HotKeys")
                        color: "#ffcc00"
                        font.pixelSize: 32
                        font.bold: true
                    }
                    Rectangle {
                        width: contentWidth; height: 1
                        color: "#33ffffff"
                    }

                    HotkeyRow {
                        id: tabRow
                        width: contentWidth
                        keyText: "Tab"
                        actionText: qsTr("Toggle HotKeys Help")
                    }
                    HotkeyRow {
                        id: layoutRow
                        width: contentWidth
                        keyText: "J"
                        options: [qsTr("Side-by-Side"), qsTr("Top-and-Bottom")]
                        current: stereo.layout
                    }
                    HotkeyRow {
                        id: eyesRow
                        width: contentWidth
                        keyText: "K"
                        options: [qsTr("Normal Eyes"), qsTr("Swapped Eyes")]
                        current: stereo.swap ? 1 : 0
                    }
                    HotkeyRow {
                        id: halfRow
                        width: contentWidth
                        keyText: "L"
                        options: [qsTr("Full"), qsTr("Half")]
                        current: stereo.half ? 1 : 0
                    }
                    Rectangle {
                        width: contentWidth; height: 1
                        color: "#33ffffff"
                    }
                    Text {
                        id: playerLabel
                        text: qsTr("Player")
                        color: "#ffcc00"
                        font.pixelSize: 22
                        font.bold: true
                    }
                    HotkeyRow {
                        id: playRow
                        width: contentWidth
                        keyText: "Space"
                        actionText: qsTr("Play / Pause")
                    }
                    HotkeyRow {
                        id: seekRow
                        width: contentWidth
                        keyText: "← / →"
                        actionText: qsTr("Seek -5s / +5s")
                    }
                    HotkeyRow {
                        id: seekShiftRow
                        width: contentWidth
                        keyText: "Shift+←/→"
                        keyWidth: 120
                        actionText: qsTr("Seek -10s / +10s")
                    }
                    HotkeyRow {
                        id: volumeRow
                        width: contentWidth
                        keyText: "↑ / ↓"
                        actionText: qsTr("Volume")
                    }
                }
            }
        }
    }
}
