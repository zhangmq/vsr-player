// StereoHotkeyHelp.qml — 3D 快捷键帮助面板
//
// 全屏窗口由 StereoController 在所选显示器上创建；本文件只负责把同一份
// HotKeys 画在左右两半：SBS 下左半进左眼、右半进右眼，两只眼看到相同内容。
// OSD 不参与任何视频/3D 映射。
// 键名（keyText）一律来自 C++ 键位表（keys.helpKey(id)），不在这里重复写死：
// 改键位只改 src/client/Keymap.cpp 一处（2026-11-03）。
import QtQuick
import "ui/components"

Item {
    id: root
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0

    // OSD 整体字号 = 正常 UI（13）+ 2。
    readonly property int helpFontSize: 18
    readonly property int parallax: 12

    Repeater {
        model: 2
        delegate: Item {
            x: index * root.width / 2
            width: root.width / 2
            height: root.height
            visible: stereo.helpVisible

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
                        font.pixelSize: root.helpFontSize
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
                                keyText: keys.helpKey("toggle-help")
                                actionText: qsTr("Toggle HotKeys Help")
                                fontPixelSize: root.helpFontSize
                            }
                            HotkeyRow {
                                keyText: keys.helpKey("stereo-layout")
                                options: [qsTr("Side-by-Side"), qsTr("Top-and-Bottom")]
                                current: stereo.layout
                                fontPixelSize: root.helpFontSize
                            }
                            HotkeyRow {
                                keyText: keys.helpKey("stereo-swap")
                                options: stereo.active
                                         ? [qsTr("Normal Eyes"), qsTr("Swapped Eyes")]
                                         : [qsTr("Left Eye"), qsTr("Right Eye")]
                                current: stereo.active ? (stereo.swap ? 1 : 0)
                                                       : (viewModel.eyeMode === 2 ? 1 : 0)
                                fontPixelSize: root.helpFontSize
                            }
                            HotkeyRow {
                                keyText: keys.helpKey("stereo-half")
                                options: [qsTr("Full"), qsTr("Half")]
                                current: stereo.half ? 1 : 0
                                fontPixelSize: root.helpFontSize
                            }
                        }
                        Column {
                            id: rightColumn
                            spacing: 6
                            HotkeyRow {
                                keyText: keys.helpKey("play-pause")
                                actionText: qsTr("Play / Pause")
                                fontPixelSize: root.helpFontSize
                            }
                            HotkeyRow {
                                keyText: keys.helpKey("seek-back")
                                actionText: qsTr("Seek -5s")
                                fontPixelSize: root.helpFontSize
                            }
                            HotkeyRow {
                                keyText: keys.helpKey("seek-fwd")
                                actionText: qsTr("Seek +5s")
                                fontPixelSize: root.helpFontSize
                            }
                            HotkeyRow {
                                keyText: keys.helpKey("seek-back-10")
                                actionText: qsTr("Seek -10s")
                                fontPixelSize: root.helpFontSize
                            }
                            HotkeyRow {
                                keyText: keys.helpKey("seek-fwd-10")
                                actionText: qsTr("Seek +10s")
                                fontPixelSize: root.helpFontSize
                            }
                            HotkeyRow {
                                keyText: keys.helpKey("volume-up")
                                actionText: qsTr("Volume up")
                                fontPixelSize: root.helpFontSize
                            }
                            HotkeyRow {
                                keyText: keys.helpKey("volume-down")
                                actionText: qsTr("Volume down")
                                fontPixelSize: root.helpFontSize
                            }
                            HotkeyRow {
                                keyText: keys.helpKey("aspect-next")
                                options: [ "16:9", "4:3", "2.35:1", qsTr("No override") ]
                                current: viewModel.aspect === "16:9" ? 0
                                       : viewModel.aspect === "4:3" ? 1
                                       : viewModel.aspect === "2.35:1" ? 2 : 3
                                fontPixelSize: root.helpFontSize
                            }
                            HotkeyRow {
                                keyText: keys.helpKey("playlist-prev")
                                actionText: qsTr("Previous")
                                fontPixelSize: root.helpFontSize
                            }
                            HotkeyRow {
                                keyText: keys.helpKey("playlist-next")
                                actionText: qsTr("Next")
                                fontPixelSize: root.helpFontSize
                            }
                        }
                    }
                }
            }
        }
    }
}
