// StereoOsd.qml — 3D OSD 原型（不含 3D 视频播放）
//
// 全屏窗口由 StereoController 在所选显示器上创建；本文件只负责把同一份
// OSD 画在左右两半：眼镜的 SBS 3D 模式下，左半进左眼、右半进右眼，
// 两只眼看到完全相同的内容。OSD 不参与任何视频/3D 映射。
import QtQuick

Item {
    id: root

    // OSD 的双眼视差（logical px）：左半右移、右半左移 = crossed disparity，
    // 融合后 OSD 看起来浮在屏幕平面前方（在视频画面之上）。固定值、不受
    // 视频眼序（stereo.swap）影响——OSD 是 UI，不属于片源。
    readonly property int parallax: 12

    // 整块黑底：SBS 模式下左右两半都是黑的，OSD 才有内容。
    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    // 左右各一份（始终两份；即使当前屏幕不是 32:9，也能在副屏上预览
    // 左右眼布局，方便没有眼镜时测试）。
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
                // 左眼 +parallax、右眼 -parallax：交叉视差 → 浮出屏幕。
                anchors.horizontalCenterOffset: (index === 0 ? 1 : -1) * root.parallax
                width: Math.min(parent.width - 60, 1040)
                height: Math.min(parent.height - 60,
                                 body.implicitHeight + 64)
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
                        text: qsTr("3D OSD (prototype)")
                        color: "#ffcc00"
                        font.pixelSize: 30
                        font.bold: true
                    }
                    Text {
                        width: parent.width
                        text: qsTr("This is the 3D shortcut overlay only. 3D video output is not implemented yet.")
                        color: "#b8b8b8"
                        font.pixelSize: 17
                        font.weight: Font.Medium
                        wrapMode: Text.WordWrap
                    }

                    Rectangle {
                        width: parent.width; height: 1
                        color: "#33ffffff"
                    }

                    // 当前状态
                    Column {
                        width: parent.width
                        spacing: 6
                        Text {
                            width: parent.width
                            color: "#e0e0e0"; font.pixelSize: 21
                            font.weight: Font.DemiBold
                            text: qsTr("Mode: %1").arg(
                                stereo.mode === 1 ? qsTr("3D source")
                                                  : qsTr("2D to 3D (not implemented)"))
                        }
                        Text {
                            width: parent.width
                            color: "#e0e0e0"; font.pixelSize: 21
                            font.weight: Font.DemiBold
                            text: qsTr("Layout: %1  (J)").arg(
                                stereo.layout === 0 ? qsTr("SBS") : qsTr("Top-Bottom"))
                        }
                        Text {
                            width: parent.width
                            color: "#e0e0e0"; font.pixelSize: 21
                            font.weight: Font.DemiBold
                            text: qsTr("Eyes: %1  (K)").arg(
                                stereo.swap ? qsTr("Swapped") : qsTr("Normal"))
                        }
                        Text {
                            width: parent.width
                            color: "#e0e0e0"; font.pixelSize: 21
                            font.weight: Font.DemiBold
                            text: qsTr("Scale: %1  (L)").arg(
                                stereo.fit ? qsTr("Fit") : qsTr("Stretch"))
                        }
                    }

                    Rectangle {
                        width: parent.width; height: 1
                        color: "#33ffffff"
                    }

                    Text {
                        width: parent.width
                        text: qsTr("Shortcuts")
                        color: "#ffcc00"; font.pixelSize: 22; font.bold: true
                    }

                    Column {
                        width: parent.width
                        spacing: 7
                        Repeater {
                            model: [
                                { key: "J",   desc: qsTr("SBS / Top-Bottom") },
                                { key: "K",   desc: qsTr("Swap left / right eye") },
                                { key: "L",   desc: qsTr("Stretch / Fit") },
                                { key: "Tab", desc: qsTr("Show / hide this OSD") },
                                { key: "Space", desc: qsTr("Play / pause") },
                                { key: "← / →", desc: qsTr("Seek -5s / +5s") },
                                { key: "↑ / ↓", desc: qsTr("Volume") }
                            ]
                            Row {
                                width: parent.width
                                spacing: 16
                                Text {
                                    width: 96
                                    text: modelData.key
                                    color: "#ffcc00"; font.pixelSize: 20
                                    font.family: "monospace"
                                    font.bold: true
                                }
                                Text {
                                    width: parent.width - 112
                                    text: modelData.desc
                                    color: "#e0e0e0"; font.pixelSize: 20
                                    font.weight: Font.DemiBold
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
                    }

                    Text {
                        width: parent.width
                        text: qsTr("Exit: take off the glasses and close from the desktop, or switch the glasses back to 2D first.")
                        color: "#909090"; font.pixelSize: 16
                        font.weight: Font.Medium
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }

    // OSD 隐藏时（Tab）窗口保持纯黑；这里不画任何东西，避免误以为还在 3D。
}
