import QtQuick
import QtQuick.Controls

Popup {
    id: root
    property real speed: 1.0
    signal speedAdjusted(real v)

    width: 180; padding: 12
    modal: true; closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle { color: "#d9111111"; radius: 8; border { width: 1; color: "#22ffffff" } }

    Column { spacing: 4; anchors { left: parent.left; right: parent.right }
        Text { text: "速度"; color: "#b0b0b0"; font.pixelSize: 13 }
        Repeater {
            model: [0.5, 0.75, 1.0, 2.0]
            Rectangle { width: parent.width; height: 36; radius: 4
                color: spm.containsMouse ? "#33ffffff" : "transparent"
                Text { anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                    text: Number(modelData) + "倍"
                    color: Math.abs(root.speed - modelData) < 0.01 ? "#ffcc00" : "#e0e0e0"; font.pixelSize: 14 }
                MouseArea { id: spm; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: root.speedAdjusted(modelData) }
            }
        }
    }
}
