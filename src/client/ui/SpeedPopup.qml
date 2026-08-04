import QtQuick
import QtQuick.Controls
import "components"

PopupBase {
    id: root
    property real speed: 1.0
    signal speedAdjusted(real v)

    width: 180; padding: 12

    Column { spacing: 4; anchors { left: parent.left; right: parent.right }
        Text { text: qsTr("Speed"); color: "#b0b0b0"; font.pixelSize: 13 }
        Repeater {
            model: [0.5, 0.75, 1.0, 2.0]
            Rectangle { width: parent.width; height: 36; radius: 4
                color: spm.containsMouse ? "#33ffffff" : "transparent"
                Text { anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                    text: qsTr("%1×").arg(Number(modelData))
                    color: Math.abs(root.speed - modelData) < 0.01 ? "#ffcc00" : "#e0e0e0"; font.pixelSize: 14 }
                MouseArea { id: spm; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: root.speedAdjusted(modelData) }
            }
        }
    }
}
