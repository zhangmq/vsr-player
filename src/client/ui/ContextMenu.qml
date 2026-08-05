import QtQuick
import QtQuick.Controls
import "components"

/// 右键菜单（播放器惯例）。复刻 PopupBase 外观；定位 = 鼠标位置
///（anchorTarget 语义不适用），宿主席位 showAt() 后 open()。
PopupBase {
    id: root
    width: 230

    signal openFilesRequested()
    signal appendFilesRequested()
    signal loadSubsRequested()
    signal playPauseRequested()
    signal stopRequested()
    signal fullscreenRequested()
    signal playlistRequested()

    /// 在 item 坐标系 (x,y)（即鼠标位置）弹出，屏幕边缘钳制
    function showAt(item, x, y) {
        if (!item || !parent) return
        var p = item.mapToItem(parent, x, y)
        root.x = Math.min(Math.max(p.x, 4), parent.width - root.width - 4)
        root.y = Math.min(Math.max(p.y, 4), parent.height - root.height - 4)
        open()
    }

    Column {
        anchors { left: parent.left; right: parent.right }
        Repeater {
            // 菜单项（sig 须与上方 signal 声明同名；新增项同步改三处：
            // signal 声明 + 此数组 + main.qml handler）
            model: [
                {icon: "󰉋", text: qsTr("Open file"),          hint: "Ctrl+O", sig: "openFilesRequested"},
                {icon: "󰐍", text: qsTr("Add to playlist"),    hint: "",       sig: "appendFilesRequested"},
                {icon: "󰂎", text: qsTr("Load subtitles"),     hint: "",       sig: "loadSubsRequested"},
                {sep: true},
                {icon: "󰐊", text: qsTr("Play/Pause"),         hint: "Space",  sig: "playPauseRequested"},
                {icon: "󰓛", text: qsTr("Stop"),               hint: "",       sig: "stopRequested"},
                {sep: true},
                {icon: "󰊓", text: qsTr("Fullscreen"),         hint: "F",      sig: "fullscreenRequested"},
                {icon: "󰐑", text: qsTr("Playlist"),           hint: "P",      sig: "playlistRequested"}
            ]
            delegate: Item {
                property bool isSep: modelData.sep
                width: parent.width
                height: isSep ? 1 : 30
                Rectangle {
                    anchors.fill: parent
                    color: !isSep && miHover.containsMouse ? "#33ffffff" : "transparent"
                    visible: !isSep
                }
                // 分隔线（sep 行）
                Rectangle {
                    anchors { left: parent.left; leftMargin: 10; right: parent.right; rightMargin: 10 }
                    height: 1; color: "#22ffffff"; visible: isSep
                }
                Row {
                    anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                    spacing: 8
                    visible: !isSep
                    Text { font.family: iconFont; font.pixelSize: 16; text: modelData.icon
                        color: "#e0e0e0"; renderType: Text.NativeRendering }
                    Text { text: modelData.text; color: "#e0e0e0"; font.pixelSize: 13 }
                }
                Text { anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                    text: modelData.hint; color: "#808080"; font.pixelSize: 11; visible: !isSep && modelData.hint !== "" }
                MouseArea { id: miHover; anchors.fill: parent; hoverEnabled: true
                    visible: !isSep; cursorShape: Qt.PointingHandCursor
                    onClicked: { root.close(); root[modelData.sig]() } }
            }
        }
    }
}
