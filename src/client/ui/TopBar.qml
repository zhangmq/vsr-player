import QtQuick
import "components"

Item {
    id: root
    property string videoInfo: ""
    property bool overlaysVisible: true
    /// 打开文件请求（TopBar 打开按钮触发，main.qml 接线到 FileDialog）
    signal openRequested()
    /// 打开文件夹请求（TopBar 文件夹按钮 → FolderDialog）
    signal openFolderRequested()
    /// 自动隐藏保持条件：TopBar 本体 hover 或下方热区（与 BottomBar
    /// hotZone 对称——鼠标靠近顶缘即激活 UI 显示，否则顶部按钮不可达）
    readonly property bool mouseInRegion: topHover.hovered || topHot.containsMouse
    implicitHeight: 48 + 40   // 渐变条 + 热区

    // ── 热区（渐变条下方 40px——鼠标靠近顶缘即显示 UI）────────
    MouseArea {
        id: topHot
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 40
        hoverEnabled: true
    }

    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 48
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#cc000000" }
            GradientStop { position: 1.0; color: "transparent" }
        }
        opacity: root.overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

        HoverHandler { id: topHover }

        Text {
            anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            font.pixelSize: 14; elide: Text.ElideRight
            color: "#e0e0e0"
            text: {
                if (root.videoInfo) return root.videoInfo
                return qsTr("VSR Player")
            }
        }

        // ── 打开文件按钮（右上角，随标题一起淡出）────────────────
        IconButton {
            id: openFileBtn
            codepoint: "󰉋"; size: 22; tooltip: qsTr("Open file (Ctrl+O)")
            // 淡出后仍命中测试——隐藏时禁点（含 tooltip）
            enabled: root.overlaysVisible
            anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
            onClicked: root.openRequested()
        }
        // ── 打开文件夹按钮（并列，folder-outline U+F0256）────────
        IconButton {
            codepoint: "󰉖"; size: 22; tooltip: qsTr("Open folder (Ctrl+Shift+O)")
            enabled: root.overlaysVisible
            anchors { right: openFileBtn.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
            onClicked: root.openFolderRequested()
        }
    }
}
