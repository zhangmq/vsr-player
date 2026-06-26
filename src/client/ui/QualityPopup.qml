import QtQuick
import QtQuick.Controls

Popup {
    id: root
    property int scal: 0
    property int qualit: 3
    property int denoiseQualit: -1
    signal scalPicked(int v)
    signal qualitPicked(int v)
    signal denoiseQualitPicked(int v)

    width: 360; padding: 16
    modal: true; closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property var scaleModel: [
        {label: "关闭", value: -1},
        {label: "自动", value: 0},
        {label: "2×", value: 2},
        {label: "3×", value: 3},
        {label: "4×", value: 4}
    ]
    readonly property var levelModel: [
        {label: "Low", value: 1},
        {label: "Medium", value: 2},
        {label: "High", value: 3},
        {label: "Ultra", value: 4}
    ]
    readonly property var denoiseModel: [
        {label: "关闭", value: -1},
        {label: "Low", value: 8},
        {label: "Medium", value: 9},
        {label: "High", value: 10},
        {label: "Ultra", value: 11}
    ]

    background: Rectangle { color: "#d9111111"; radius: 8; border { width: 1; color: "#22ffffff" } }

    Column {
        spacing: 12; anchors { left: parent.left; right: parent.right }

        Text { text: "Scale"; color: "#b0b0b0"; font.pixelSize: 13 }
        Row { spacing: 6; anchors { left: parent.left; right: parent.right }
            Repeater {
                model: root.scaleModel
                delegate: Rectangle {
                    width: 56; height: 32; radius: 4
                    color: sHover.containsMouse ? "#33ffffff"
                         : (root.scal === modelData.value ? "#33ffcc00" : "transparent")
                    Text { anchors.centerIn: parent
                        text: modelData.label
                        color: root.scal === modelData.value ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13 }
                    MouseArea { id: sHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { root.scalPicked(modelData.value); } }
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: "#0fffffff" }

        Text { text: "超分质量"; color: "#b0b0b0"; font.pixelSize: 13 }
        Row { spacing: 6; anchors { left: parent.left; right: parent.right }
            Repeater {
                model: root.levelModel
                delegate: Rectangle {
                    width: 70; height: 32; radius: 4
                    color: qHover.containsMouse ? "#33ffffff"
                         : (root.qualit === modelData.value ? "#33ffcc00" : "transparent")
                    Text { anchors.centerIn: parent
                        text: modelData.label
                        color: root.qualit === modelData.value ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13 }
                    MouseArea { id: qHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { root.qualitPicked(modelData.value); } }
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: "#0fffffff" }

        Text { text: "强制降噪（scale=1时生效）"; color: "#b0b0b0"; font.pixelSize: 13 }
        Row { spacing: 6; anchors { left: parent.left; right: parent.right }
            Repeater {
                model: root.denoiseModel
                delegate: Rectangle {
                    width: 56; height: 32; radius: 4
                    color: dHover.containsMouse ? "#33ffffff"
                         : (root.denoiseQualit === modelData.value ? "#33ffcc00" : "transparent")
                    Text { anchors.centerIn: parent
                        text: modelData.label
                        color: root.denoiseQualit === modelData.value ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13 }
                    MouseArea { id: dHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { root.denoiseQualitPicked(modelData.value); } }
                }
            }
        }
    }
}
