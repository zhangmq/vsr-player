import QtQuick
import QtQuick.Controls
import QtQuick.Window
import "components"

/// 「关于」弹窗（右键菜单 → About）：图标 / 名称 / 版本 / 许可协议 / 仓库地址。
///
/// 数据来源（都在代码里，不新造第二份）：
/// - 图标 = `qrc:/vsr-player.png`，即 `data/vsr-player.svg` 的 512 PNG 渲染
///   （与桌面图标同一份文件；换图标只改 SVG 后跑 scripts/render-icon.sh）。
/// - 版本 = context property `appVersion` ← `VSR_VERSION`（meson 由根
///   meson.build 的 project(version:) 生成；发版只改那一处）。
/// - 名称 / 许可 / 仓库 = 与 `data/vsr-player.desktop.in`、README「License」、
///   LICENSE 对齐；仓库地址可点（Qt.openUrlExternally）。
///
/// 版式：左侧图标 + 右侧名称/版本，下面一条分隔线 + 两列信息表（label / value）。
PopupBase {
    id: root
    padding: 18
    modal: true
    width: 400
    // modal 居中（Popup 默认 x/y=0 靠左）
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0

    /// 仓库地址（与 README 的 clone 地址一致）：显示完整 URL，点击用同一串。
    readonly property string repoUrl: "https://github.com/zhangmq/vsr-player"
    /// SPDX 标识（README 写 "GPLv2+"，对应 GPL-2.0-or-later）。
    readonly property string licenseId: "GPL-2.0-or-later"

    Column {
        spacing: 14
        anchors { left: parent.left; right: parent.right }

        Row {
            spacing: 16
            Image {
                width: 96; height: 96
                source: "qrc:/vsr-player.png"
                fillMode: Image.PreserveAspectFit
                smooth: true
                // 96 逻辑 px × DPR 2 = 192 物理 px：按这个尺寸解码，1:1 上屏不糊
                sourceSize: Qt.size(96 * Math.max(1, Screen.devicePixelRatio), 96 * Math.max(1, Screen.devicePixelRatio))
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6
                Text {
                    text: qsTr("VSR Player")
                    color: "#e0e0e0"; font.pixelSize: 18
                }
                Text {
                    text: qsTr("Version %1").arg(appVersion)
                    color: "#b0b0b0"; font.pixelSize: 13
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: "#0fffffff" }

        /// 信息表：两列（label / value），列宽按内容自动对齐。
        Grid {
            columns: 2
            columnSpacing: 16
            rowSpacing: 8

            Text { text: qsTr("License"); color: "#b0b0b0"; font.pixelSize: 13 }
            Text { text: root.licenseId; color: "#e0e0e0"; font.pixelSize: 13 }

            Text { text: qsTr("Repository"); color: "#b0b0b0"; font.pixelSize: 13 }
            // 点击区包一层 Item：MouseArea 不能直接 `anchors.fill` 填满 Row
            //（Grid/Row 里的 fill 会与 Row 的隐式尺寸互相依赖 → 整格算成 0 宽，
            //  实测这一行完全不渲染），所以先给外层 Item 明确的隐式尺寸。
            Item {
                implicitWidth: repoRow.implicitWidth
                implicitHeight: repoRow.implicitHeight
                width: implicitWidth
                height: implicitHeight
                Row {
                    id: repoRow
                    spacing: 6
                    Text {
                        text: root.repoUrl
                        color: repoHover.containsMouse ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13
                    }
                    Text {
                        font.family: iconFont; font.pixelSize: 13
                        text: "󰏌"          // mdi-open-in-new U+F03CC
                        color: repoHover.containsMouse ? "#ffcc00" : "#808080"
                        renderType: Text.NativeRendering
                    }
                }
                MouseArea {
                    id: repoHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Qt.openUrlExternally(root.repoUrl)
                }
            }
        }
    }
}
