import QtQuick
import QtQuick.Controls

/// 两态分段开关（Off/On）——与 QualityPopup 里的档位行同款视觉
///（圆角矩形 + 选中高亮），用于 TrueHDR 的总开关 / 峰值覆盖 / 抗色带开关。
Item {
    id: root
    property bool value: false
    property string offLabel: qsTr("Off")
    property string onLabel: qsTr("On")
    signal toggled(bool v)

    implicitHeight: 32
    // ⚠ 宽度必须按**内容**算：内部是两个 56 宽按钮 + 6 间距。
    // 调用处不要再写 `width: 88` 之类的固定值——视觉内容会溢出到相邻控件上
    //（2026-10-18 实测：高级按钮与开关重叠）。
    implicitWidth: segRow.width

    Row {
        id: segRow
        spacing: 6
        Repeater {
            model: [ { label: root.offLabel, v: false },
                     { label: root.onLabel,  v: true } ]
            delegate: Rectangle {
                width: 56; height: 32; radius: 4
                color: tgHover.containsMouse ? "#33ffffff"
                     : (root.value === modelData.v ? "#33ffcc00" : "transparent")
                Text {
                    anchors.centerIn: parent
                    text: modelData.label
                    color: root.value === modelData.v ? "#ffcc00" : "#e0e0e0"
                    font.pixelSize: 13
                }
                MouseArea {
                    id: tgHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    enabled: root.enabled
                    onClicked: root.toggled(modelData.v)
                }
            }
        }
    }
}
