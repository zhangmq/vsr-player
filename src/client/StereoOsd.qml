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

    // OSD 正常界面字体 +1（项目 UI 的常规字号是 13）。
    readonly property int osdFontSize: 14
    readonly property int parallax: 12

    Repeater {
        model: 2
        delegate: Item {
            x: index * root.width / 2
            width: root.width / 2
            height: root.height
            visible: stereo.osdVisible

            // 内容宽度以两列为准；Column/HotkeyRow 的 implicitWidth 首次布局前
            // 不可靠，给一个与两列内容匹配的下限，避免 panel 缩成一条。
            readonly property real contentWidth: Math.max(
                560,
                titleText.implicitWidth,
                leftColumn.implicitWidth + 40 + rightColumn.implicitWidth)

            Rectangle {
                id: panel
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: (index === 0 ? 1 : -1) * root.parallax
                width: Math.min(parent.width - 60, contentWidth + 48)
                height: Math.min(parent.height - 60, body.implicitHeight + 48)
                color: "#cc111111"
                radius: 14
                border.width: 1
                border.color: "#33ffffff"

                Column {
                    id: body
                    anchors.centerIn: parent
                    width: contentWidth
                    spacing: 8

                    Text {
                        id: titleText
                        text: qsTr("HotKeys")
                        color: "#ffcc00"
                        font.pixelSize: root.osdFontSize
                        font.bold: true
                    }
                    Rectangle {
                        width: contentWidth; height: 1
                        color: "#33ffffff"
                    }

                    Row {
                        spacing: 40
                        Column {
                            id: leftColumn
                            spacing: 6
                            HotkeyRow {
                                keyText: "Tab"
                                actionText: qsTr("Toggle HotKeys Help")
                                fontPixelSize: root.osdFontSize
                            }
                            HotkeyRow {
                                keyText: "J"
                                options: [qsTr("Side-by-Side"), qsTr("Top-and-Bottom")]
                                current: stereo.layout
                                fontPixelSize: root.osdFontSize
                            }
                            HotkeyRow {
                                keyText: "K"
                                options: [qsTr("Normal Eyes"), qsTr("Swapped Eyes")]
                                current: stereo.swap ? 1 : 0
                                fontPixelSize: root.osdFontSize
                            }
                            HotkeyRow {
                                keyText: "L"
                                options: [qsTr("Full"), qsTr("Half")]
                                current: stereo.half ? 1 : 0
                                fontPixelSize: root.osdFontSize
                            }
                        }
                        Column {
                            id: rightColumn
                            spacing: 6
                            HotkeyRow {
                                keyText: "Space"
                                actionText: qsTr("Play / Pause")
                                fontPixelSize: root.osdFontSize
                            }
                            HotkeyRow {
                                keyText: "← / →"
                                actionText: qsTr("Seek -5s / +5s")
                                fontPixelSize: root.osdFontSize
                            }
                            HotkeyRow {
                                keyText: "Shift+←/→"
                                keyWidth: 90
                                actionText: qsTr("Seek -10s / +10s")
                                fontPixelSize: root.osdFontSize
                            }
                            HotkeyRow {
                                keyText: "↑ / ↓"
                                actionText: qsTr("Volume")
                                fontPixelSize: root.osdFontSize
                            }
                            HotkeyRow {
                                keyText: "N"
                                actionText: qsTr("Next")
                                fontPixelSize: root.osdFontSize
                            }
                            HotkeyRow {
                                keyText: "B"
                                actionText: qsTr("Previous")
                                fontPixelSize: root.osdFontSize
                            }
                        }
                    }
                }
            }
        }
    }
}
