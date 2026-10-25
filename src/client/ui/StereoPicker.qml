// StereoPicker.qml — 3D 模式入口对话框（右键菜单 → 3D → 3D片源/2D转3D）
//
// 只负责“提示切眼镜 + 选择眼镜显示器 + 开始 3D OSD 原型”。真正的 3D OSD
// 窗口由 StereoController 在所选显示器上创建；本对话框始终留在桌面。
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
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    x: parent ? Math.max(4, (parent.width - width) / 2) : 0
    y: parent ? Math.max(4, (parent.height - height) / 2) : 0

    function openForMode(m) {
        pendingMode = m
        stereo.refreshScreens()
        // 屏幕记忆只做默认选中，不自动切换/启动；没有记忆再选 32:9
        // （眼镜 3D 模式），否则第一块。
        selectedIndex = stereo.preferredScreenIndex()
        if (selectedIndex < 0) {
            selectedIndex = 0
            for (var i = 0; i < stereo.screens.length; i++) {
                var s = stereo.screens[i]
                if (s.width >= s.height * 2.5) {
                    selectedIndex = s.index
                    break
                }
            }
        }
        open()
    }

    Column {
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
            text: qsTr("Switch the glasses to 3D / SBS mode first (VITURE: hold the Mode button). The glasses output should become 3840×1080. The desktop will not be readable on the glasses in that mode.")
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
                        text: modelData.name + "   " + modelData.width + " × " + modelData.height
                        color: "#e0e0e0"; font.pixelSize: 16
                    }
                    Text {
                        anchors.right: parent.right; anchors.rightMargin: 14
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.width >= modelData.height * 2.5 ? qsTr("SBS 3D")
                                                                        : qsTr("2D / other")
                        color: modelData.width >= modelData.height * 2.5 ? "#ffcc00" : "#808080"
                        font.pixelSize: 14
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
            text: qsTr("The main window stays on this desktop; the glasses will show the 3D OSD.")
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
                    onClicked: {
                        stereo.start(root.pendingMode, root.selectedIndex)
                        root.close()
                    }
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
