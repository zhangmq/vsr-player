import QtQuick
import "components"

Item {
    id: root
    property string videoInfo: ""
    property bool overlaysVisible: true
    /// 打开菜单请求（右上角打开按钮触发，main.qml 弹出 OpenMenu：
    /// 文件 / 文件夹 / URL 三入口合并）
    signal openMenuRequested()
    /// 打开按钮引用（供 OpenMenu 的 anchorTarget 定位）
    property alias openBtn: openFileBtn
    /// 自动隐藏保持条件：整个 UI 区域（渐变条 48 + 热区 40）视作**一个连续
    /// 区域**——单个 HoverHandler 挂在 root 上（implicitHeight 即该区域）。
    /// 与 BottomBar 同一模型（那里三段判定曾在段间留下死带，鼠标穿越时 UI
    /// "可见→消失→可见"，见 BottomBar.mouseInRegion 注释）。
    /// HoverHandler 而非 MouseArea：只观察 hover，不吞鼠标事件。
    readonly property bool mouseInRegion: topHover.hovered
    implicitHeight: 48 + 40   // 渐变条 + 热区

    HoverHandler { id: topHover }

    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 48
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#cc000000" }
            GradientStop { position: 1.0; color: "transparent" }
        }
        opacity: root.overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

        Text {
            anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            font.pixelSize: 13; elide: Text.ElideRight
            color: "#e0e0e0"
            text: {
                if (root.videoInfo) return root.videoInfo
                return qsTr("VSR Player")
            }
        }

        // ── 打开按钮（右上角，随标题一起淡出；点击弹三入口菜单）──
        IconButton {
            id: openFileBtn
            codepoint: "󰉋"; size: 22; tooltip: qsTr("Open")
            // 淡出后仍命中测试——隐藏时禁点（含 tooltip）
            enabled: root.overlaysVisible
            anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
            onClicked: root.openMenuRequested()
        }
    }
}
