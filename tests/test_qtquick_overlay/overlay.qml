import QtQuick

Rectangle {
    id: root
    width: 800
    height: 600
    color: "transparent"

    // Semi-transparent circular button (center)
    Rectangle {
        id: playBtn
        width: 80
        height: 80
        radius: 40
        color: "#66000000"  // 40% opacity black
        anchors.centerIn: parent

        property bool playing: false

        Text {
            text: playBtn.playing ? "⏸" : "▶"
            color: "#eeffffff"  // slightly transparent white
            font.pixelSize: 28
            anchors.centerIn: parent
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            hoverEnabled: true
            onClicked: {
                playBtn.playing = !playBtn.playing
                console.log("clicked! playing =", playBtn.playing)
            }
            onEntered: playBtn.color = "#aa333333"  // 67% grey on hover
            onExited:  playBtn.color = "#66000000"  // back to 40% black
        }
    }

    // Semi-transparent bottom overlay bar
    Rectangle {
        id: bottomBar
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        height: 48
        opacity: 0.0   // hidden until mouse moves
        color: "#55000000"  // 33% opacity black — Vulkan pattern shows through

        Behavior on opacity {
            NumberAnimation { duration: 300 }
        }

        Text {
            anchors.centerIn: parent
            text: "QML Overlay  |  00:00 / 00:00"
            color: "#ddffffff"
            font.pixelSize: 14
        }
    }

    // Top semi-transparent label
    Rectangle {
        anchors {
            top: parent.top
            left: parent.left
            margins: 12
        }
        width: labelText.implicitWidth + 24
        height: 32
        radius: 6
        color: "#44000000"  // 27% opacity

        Text {
            id: labelText
            anchors.centerIn: parent
            text: "Vulkan pattern below + QML overlay above"
            color: "#ccffffff"
            font.pixelSize: 13
        }
    }

    // Mouse move shows/hides bottom bar
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onPositionChanged: {
            bottomBar.opacity = 1.0
            hideTimer.restart()
        }
    }

    Timer {
        id: hideTimer
        interval: 800
        onTriggered: bottomBar.opacity = 0.0
    }

}
