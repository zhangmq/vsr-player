import QtQuick
import QtQuick.Controls
import "components"

Popup {
    id: root
    property real volume: 0.0
    property bool muted: false
    signal volAdjusted(real v)
    signal muteToggled()

    width: 48; padding: 12
    modal: true; closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: { volSlider.value = root.volume * 100 }

    background: Rectangle { color: "#d9111111"; radius: 8; border { width: 1; color: "#22ffffff" } }

    Column {
        spacing: 8; anchors.horizontalCenter: parent.horizontalCenter

        Slider {
            id: volSlider
            anchors.horizontalCenter: parent.horizontalCenter
            orientation: Qt.Vertical; height: 120
            from: 0; to: 100; live: true
            onMoved: root.volAdjusted(value / 100.0)
            handle: Rectangle { implicitWidth: 14; implicitHeight: 14; radius: 7; color: "#ffffff"
                x: parent.leftPadding + parent.availableWidth / 2 - width / 2
                y: parent.topPadding + parent.visualPosition * (parent.availableHeight - height) }
            background: Rectangle { implicitWidth: 4; color: "#44ffffff"; radius: 2
                anchors.horizontalCenter: parent.horizontalCenter
                Rectangle { width: 4; radius: 2; color: "#e0e0e0"
                    anchors.bottom: parent.bottom
                    height: parent.height * volSlider.position } }
        }

        IconButton {
            anchors.horizontalCenter: parent.horizontalCenter
            codepoint: root.muted ? "" : ""; size: 20
            tooltip: root.muted ? "取消静音" : "静音"
            onClicked: { root.muteToggled(); volSlider.value = root.volume * 100 }
        }
    }
}
