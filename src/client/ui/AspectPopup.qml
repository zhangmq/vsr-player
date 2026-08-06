import QtQuick
import QtQuick.Controls
import "components"

/// 画面比例（mpv video-aspect-override，选项对齐 mpv 惯例 + 常用比例）。
/// 样式同 SpeedPopup：纵向单列、选中仅文字变色（#ffcc00）。
/// 值语义（mpv m_option_type_aspect）："no"=不覆盖（默认，容器比例，
/// mpv 0.41 官方推荐；"-1" 自动已弃用且与其等价——不提供）、其他任意
/// 比例字符串（parse_numeric 支持 "16:9"）。
/// 改变走 UPDATE_VIDEO →
/// VOCTRL_UPDATE_RENDER_OPTS（VO 渲染参数更新，不重建滤镜链——与
/// vf_vsr auto 不冲突，RT 尺寸不变决策稳定）。
PopupBase {
    id: root
    property string aspect: "no"
    signal aspectPicked(string v)

    width: 180; padding: 12

    readonly property var aspectModel: [
        {label: "16:9", value: "16:9"},
        {label: "16:10", value: "16:10"},
        {label: "4:3", value: "4:3"},
        {label: "5:4", value: "5:4"},
        {label: "2.35:1", value: "2.35:1"},
        {label: "2.39:1", value: "2.39:1"},
        {label: "1.85:1", value: "1.85:1"},
        {label: "1:1", value: "1:1"},
        {label: qsTr("No override"), value: "no"}
    ]

    Column { spacing: 4; anchors { left: parent.left; right: parent.right }
        Text { text: qsTr("Aspect ratio"); color: "#b0b0b0"; font.pixelSize: 13 }
        Repeater {
            model: root.aspectModel
            delegate: Rectangle { width: parent.width; height: 36; radius: 4
                color: aHover.containsMouse ? "#33ffffff" : "transparent"
                Text { anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                    text: modelData.label
                    color: root.aspect === modelData.value ? "#ffcc00" : "#e0e0e0"; font.pixelSize: 13 }
                MouseArea { id: aHover; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: root.aspectPicked(modelData.value) }
            }
        }
    }
}
