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
    /// 播放阶段（PlayerViewModel.mediaPhase）：0=idle 1=opening 3=failed。
    /// （2=playing 时本组件整体隐藏，由宿主控制 visible。）
    property int phase: 0
    /// failed = 失败原因（idle 不用）
    property string detail: ""
    /// failed 的条目是目录 → 标题显示"空文件夹"而不是"无法播放"
    property bool failedIsFolder: false
    /// failed 时是否给"重试"（只有网络源给；本地源没有可重试的意义）
    property bool canRetry: false

    readonly property bool failed: phase === 3
    readonly property bool idleState: phase === 0

    signal openFilesRequested()
    signal openFolderRequested()
    signal openUrlRequested()
    signal continueRequested()
    signal retryRequested()

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

            Rectangle {
                visible: root.idleState
                width: parent.width; height: visible ? 44 : 0; color: "transparent"
                Text {
                    anchors.centerIn: parent
                    font.family: iconFont; font.pixelSize: 44
                    text: "󰉋"; color: "#e0e0e0"
                    renderType: Text.NativeRendering
                }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.failed
                      ? (root.failedIsFolder ? qsTr("Empty folder") : qsTr("Cannot play"))
                      : qsTr("Drop files here")
                color: "#e0e0e0"; font.pixelSize: 14
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideMiddle
                text: root.idleState ? qsTr("or open with") : root.detail
                color: "#808080"; font.pixelSize: 13
                bottomPadding: 6
            }
            Rectangle {
                visible: root.idleState
                width: parent.width; height: visible ? 1 : 0; color: "#1affffff"
                anchors.horizontalCenter: parent.horizontalCenter
            }
            ActionRow {
                visible: root.idleState || root.failed
                height: visible ? 32 : 0
                glyph: "󰈙"; label: qsTr("Open file…"); hint: "Ctrl+O"
                onActivated: root.openFilesRequested()
            }
            ActionRow {
                visible: root.idleState || root.failed
                height: visible ? 32 : 0
                glyph: "󰉖"; label: qsTr("Open folder…"); hint: "Ctrl+Shift+O"
                onActivated: root.openFolderRequested()
            }
            ActionRow {
                visible: root.idleState || root.failed
                height: visible ? 32 : 0
                glyph: "󰌷"; label: qsTr("Open URL…"); hint: "Ctrl+L"
                onActivated: root.openUrlRequested()
            }
            ActionRow {
                // 失败态：网络源给"重试"（重新打开同一 URL），本地源不给
                visible: root.failed && root.canRetry
                height: visible ? 32 : 0
                glyph: "󰐊"; label: qsTr("Retry"); hint: ""
                onActivated: root.retryRequested()
            }
            ActionRow {
                // "继续上次播放"只在 idle；失败态显示它会再失败一次（用户 2026-11-15）
                visible: root.idleState && root.canContinue
                height: visible ? 32 : 0
                glyph: "󰐊"; label: qsTr("Continue last session"); hint: qsTr("Space")
                onActivated: root.continueRequested()
            }
        }
    }
}
