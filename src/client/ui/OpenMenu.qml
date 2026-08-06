import QtQuick
import "components"

/// 右上角"打开"菜单（文件 / 文件夹 / URL 三入口合并，2026-08-06）。
/// 复用 PopupBase（anchorTarget 定位到 TopBar 打开按钮下方）；行样式
/// 与 ContextMenu 一致（深色 + hover 高亮 + 快捷键提示）。
PopupBase {
    id: root
    width: 220

    signal fileRequested()
    signal folderRequested()
    signal urlRequested()

    Column {
        anchors { left: parent.left; right: parent.right }
        Repeater {
            // sig 须与上方 signal 声明同名；modelData 字段缺失时 undefined
            //（=== true / || "" 默认值，勿直接赋值）
            model: [
                {icon: "󰉋", text: qsTr("Open file"),    hint: "Ctrl+O", sig: "fileRequested"},
                {icon: "󰉖", text: qsTr("Open folder"),  hint: "Ctrl+Shift+O", sig: "folderRequested"},
                {icon: "󰌷", text: qsTr("Open URL"), hint: "Ctrl+L", sig: "urlRequested"}
            ]
            delegate: Item {
                width: parent.width
                height: 30
                Rectangle {
                    anchors.fill: parent
                    color: miHover.containsMouse ? "#33ffffff" : "transparent"
                }
                Row {
                    anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                    spacing: 8
                    Text { font.family: iconFont; font.pixelSize: 16; text: modelData.icon || ""
                        color: "#e0e0e0"; renderType: Text.NativeRendering }
                    Text { text: modelData.text || ""; color: "#e0e0e0"; font.pixelSize: 13 }
                }
                Text { anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                    text: modelData.hint || ""; color: "#808080"; font.pixelSize: 13 }
                MouseArea { id: miHover; anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { root.close(); root[modelData.sig]() } }
            }
        }
    }
}
