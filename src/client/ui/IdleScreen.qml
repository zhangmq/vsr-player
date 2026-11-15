import QtQuick

/// 无媒体时的空状态：中央 dropzone + 初始操作提示（用户 2026-11-15）。
///
/// 只负责**引导**：真正接住拖放的是 main.qml 里窗口级的 DropArea（这里只是
/// 视觉提示 + 快捷入口，拖放高亮由宿主把 DropArea 的 containsDrag 传进
/// `dragActive`）。样式沿用项目已有语言：`#d9111111` 面板、`#22ffffff` 边框、
/// 13 px 文本、菜单同款的 hover 高亮（`#33ffffff`），不引入新配色。
Item {
    id: root

    /// 拖放进行中（宿主 DropArea.containsDrag）→ 边框变强调色
    property bool dragActive: false
    /// 有可续播的播放列表时多一行"继续上次播放"
    property bool canContinue: false

    signal openFilesRequested()
    signal openFolderRequested()
    signal openUrlRequested()
    signal continueRequested()

    /// 「初始操作提示」的一行：图标 + 标签 + 右侧快捷键（与右键菜单同款行高）
    component ActionRow: Item {
        id: rowItem
        property string glyph: ""
        property string label: ""
        property string hint: ""
        signal activated()
        width: parent ? parent.width : 0
        height: 32

        Rectangle {
            anchors.fill: parent
            radius: 6
            color: rowHover.hovered ? "#33ffffff" : "transparent"
        }
        Row {
            anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
            spacing: 10
            Text {
                font.family: iconFont; font.pixelSize: 18
                text: rowItem.glyph; color: "#e0e0e0"
                renderType: Text.NativeRendering
            }
            Text { text: rowItem.label; color: "#e0e0e0"; font.pixelSize: 13 }
        }
        Text {
            anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
            text: rowItem.hint; color: "#808080"; font.pixelSize: 13
        }
        HoverHandler { id: rowHover }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: rowItem.activated()
        }
    }

    Rectangle {
        id: card
        width: 420
        anchors.centerIn: parent
        height: column.implicitHeight + 2 * 20
        radius: 10
        color: root.dragActive ? "#dd1c1c14" : "#d9111111"
        border.width: root.dragActive ? 2 : 1
        border.color: root.dragActive ? "#ffcc00" : "#22ffffff"
        Behavior on border.color { ColorAnimation { duration: 120 } }
        Behavior on color { ColorAnimation { duration: 120 } }

        Column {
            id: column
            anchors { left: parent.left; right: parent.right; top: parent.top }
            anchors.margins: 20
            spacing: 4

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                font.family: iconFont; font.pixelSize: 44
                text: "󰉋"; color: "#e0e0e0"
                renderType: Text.NativeRendering
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Drop files here")
                color: "#e0e0e0"; font.pixelSize: 14
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("or open with")
                color: "#808080"; font.pixelSize: 13
                bottomPadding: 6
            }
            Rectangle {
                width: parent.width; height: 1; color: "#1affffff"
                anchors.horizontalCenter: parent.horizontalCenter
            }
            ActionRow {
                glyph: "󰈙"; label: qsTr("Open file…"); hint: "Ctrl+O"
                onActivated: root.openFilesRequested()
            }
            ActionRow {
                glyph: "󰉖"; label: qsTr("Open folder…"); hint: "Ctrl+Shift+O"
                onActivated: root.openFolderRequested()
            }
            ActionRow {
                glyph: "󰌷"; label: qsTr("Open URL…"); hint: "Ctrl+L"
                onActivated: root.openUrlRequested()
            }
            ActionRow {
                visible: root.canContinue
                height: visible ? 32 : 0
                glyph: "󰐊"; label: qsTr("Continue last session"); hint: qsTr("Space")
                onActivated: root.continueRequested()
            }
        }
    }
}
