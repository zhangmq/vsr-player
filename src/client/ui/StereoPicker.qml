// StereoPicker.qml — 3D 模式入口对话框（右键菜单 → 3D → 3D片源/2D转3D）
//
// 只负责“提示切眼镜 + 选择眼镜显示器 + 开始 3D”。真正的 presentation 窗口由
// StereoController 在所选显示器上创建；本对话框始终留在桌面。
//
// 键盘（用户 2026-11-03 要求）：↑/↓ 选屏（循环）、Enter 开始、Esc 取消。
// 弹窗打开时键位表整体让位（KeyRouter 的 popup 门控），所以这里自己接 Keys
// ——与右键菜单同一套做法（contentItem 拿焦点 + Keys.onPressed）。
import QtQuick
import QtQuick.Controls

import "components"

PopupBase {
    id: root

    property int pendingMode: 1      // 1=3D片源, 2=2D转3D
    property int selectedIndex: 0

    width: 620
    height: 470
    padding: 20
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    x: parent ? Math.max(4, (parent.width - width) / 2) : 0
    y: parent ? Math.max(4, (parent.height - height) / 2) : 0

    function openForMode(m) {
        pendingMode = m
        // 屏幕列表由 StereoController 的 screenAdded/Removed/geometryChanged
        // 维护；这里不再同步跑 kscreen-doctor，避免点菜单后卡顿。
        // 屏幕记忆只做默认选中，不自动切换/启动；没有记忆选第一块。
        // 不按分辨率/宽高比猜“眼镜屏”。
        selectedIndex = stereo.preferredScreenIndex()
        if (selectedIndex < 0)
            selectedIndex = 0
        open()
    }

    /// 上下移动选中屏（循环；列表很短，到边界继续按不会卡住）
    function moveSelection(delta) {
        var n = stereo.screens.length
        if (n <= 0)
            return
        var cur = 0
        for (var i = 0; i < n; i++)
            if (stereo.screens[i].index === root.selectedIndex) cur = i
        cur = (cur + delta + n) % n
        root.selectedIndex = stereo.screens[cur].index
    }

    /// 先关选择器再启动：start() 会把主窗口移到眼镜屏，晚关的 popup 会跟过去。
    function startSelected() {
        var mode = root.pendingMode
        var idx = root.selectedIndex
        root.close()
        stereo.start(mode, idx)
    }

    contentItem: Item {
        id: content
        focus: true
        implicitWidth: col.implicitWidth
        implicitHeight: col.implicitHeight

        Keys.onPressed: function(ev) {
            if (ev.key === Qt.Key_Up) {
                root.moveSelection(-1); ev.accepted = true
            } else if (ev.key === Qt.Key_Down) {
                root.moveSelection(1); ev.accepted = true
            } else if (ev.key === Qt.Key_Return || ev.key === Qt.Key_Enter) {
                root.startSelected(); ev.accepted = true
            } else if (ev.key === Qt.Key_Escape) {
                root.close(); ev.accepted = true
            }
        }

        Column {
            id: col
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 12

            Text {
                width: parent.width
                text: root.pendingMode === 2 ? qsTr("2D to 3D (prototype)")
                                             : qsTr("3D playback (prototype)")
                color: "#e0e0e0"; font.pixelSize: 20; font.bold: true
            }
            Text {
                width: parent.width
                text: qsTr("Switch the target display to its 3D / SBS mode first, then select that display below.")
                color: "#b0b0b0"; font.pixelSize: 14; wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                text: qsTr("Select the display that belongs to the glasses:")
                color: "#b0b0b0"; font.pixelSize: 14
            }

            // 屏幕列表
            Column {
                width: parent.width
                spacing: 4
                Repeater {
                    model: stereo.screens
                    delegate: Rectangle {
                        width: parent.width
                        height: 46
                        radius: 5
                        color: root.selectedIndex === modelData.index ? "#33ffffff" : "transparent"
                        border.width: 1
                        border.color: root.selectedIndex === modelData.index ? "#55ffffff" : "transparent"
                        Text {
                            anchors.left: parent.left; anchors.leftMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            text: (modelData.model && modelData.model.length > 0
                                   ? modelData.model + (modelData.model !== modelData.name ? " (" + modelData.name + ")" : "")
                                   : modelData.name)
                                  + "   " + modelData.width + " × " + modelData.height
                            color: "#e0e0e0"; font.pixelSize: 16
                        }
                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectedIndex = modelData.index
                        }
                    }
                }
            }

            Text {
                width: parent.width
                text: qsTr("The main UI stays on this desktop; the selected display shows the 3D video and OSD.")
                color: "#808080"; font.pixelSize: 13; wrapMode: Text.WordWrap
            }

            Row {
                spacing: 10
                Rectangle {
                    width: 110; height: 36; radius: 5
                    color: startMa.containsMouse ? "#44ffffff" : "#33ffffff"
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Start")
                        color: "#ffffff"; font.pixelSize: 15
                    }
                    MouseArea {
                        id: startMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.startSelected()
                    }
                }
                Rectangle {
                    width: 90; height: 36; radius: 5
                    color: cancelMa.containsMouse ? "#33ffffff" : "transparent"
                    border.width: 1; border.color: "#33ffffff"
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: "#e0e0e0"; font.pixelSize: 15
                    }
                    MouseArea {
                        id: cancelMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.close()
                    }
                }
            }
        }
    }
}
