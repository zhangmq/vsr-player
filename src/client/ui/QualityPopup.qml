import QtQuick
import QtQuick.Controls
import "components"

PopupBase {
    id: root
    property int scal: 0
    property int quality: 3
    property int denoiseQuality: -1
    signal scalPicked(int v)
    signal qualityPicked(int v)
    signal denoiseQualityPicked(int v)

    width: 360

    readonly property var scaleModel: [
        {label: qsTr("Off"), value: -1},
        {label: qsTr("Auto"), value: 0},
        {label: qsTr("2×"), value: 2},
        {label: qsTr("3×"), value: 3},
        {label: qsTr("4×"), value: 4}
    ]
    readonly property var levelModel: [
        {label: qsTr("Low"), value: 1},
        {label: qsTr("Medium"), value: 2},
        {label: qsTr("High"), value: 3},
        {label: qsTr("Ultra"), value: 4}
    ]
    readonly property var denoiseModel: [
        {label: qsTr("Off"), value: -1},
        {label: qsTr("Low"), value: 8},
        {label: qsTr("Medium"), value: 9},
        {label: qsTr("High"), value: 10},
        {label: qsTr("Ultra"), value: 11}
    ]

    Column {
        spacing: 12; anchors { left: parent.left; right: parent.right }

        Text { text: qsTr("Scale"); color: "#b0b0b0"; font.pixelSize: 13 }
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

        Text { text: qsTr("Upscale quality"); color: "#b0b0b0"; font.pixelSize: 13 }
        Row { spacing: 6; anchors { left: parent.left; right: parent.right }
            Repeater {
                model: root.levelModel
                delegate: Rectangle {
                    width: 70; height: 32; radius: 4
                    color: qHover.containsMouse ? "#33ffffff"
                         : (root.quality === modelData.value ? "#33ffcc00" : "transparent")
                    Text { anchors.centerIn: parent
                        text: modelData.label
                        color: root.quality === modelData.value ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13 }
                    MouseArea { id: qHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { root.qualityPicked(modelData.value); } }
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: "#0fffffff" }

        Text { text: qsTr("Force denoise (active when scale=1)"); color: "#b0b0b0"; font.pixelSize: 13 }
        Row { spacing: 6; anchors { left: parent.left; right: parent.right }
            Repeater {
                model: root.denoiseModel
                delegate: Rectangle {
                    width: 56; height: 32; radius: 4
                    color: dHover.containsMouse ? "#33ffffff"
                         : (root.denoiseQuality === modelData.value ? "#33ffcc00" : "transparent")
                    Text { anchors.centerIn: parent
                        text: modelData.label
                        color: root.denoiseQuality === modelData.value ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13 }
                    MouseArea { id: dHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { root.denoiseQualityPicked(modelData.value); } }
                }
            }
        }
    }
}
