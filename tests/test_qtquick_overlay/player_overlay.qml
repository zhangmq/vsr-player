import QtQuick

// Full overlay UI for VSR Player — Qt Quick version
// Matches the previous Qt Widgets experiment:
//   - Semi-transparent circular play/pause button (center)
//   - Semi-transparent bottom bar with info text
//   - Mouse move shows overlays, auto-hide after 800ms
//   - Click to toggle play/pause
//   - Keyboard handled via C++ bridge (RootContext property)

Rectangle {
    id: root
    color: "transparent"

    // ── State from C++ ────────────────────────────────────────────────

    property bool playing: false
    // playerState: "playing" | "paused" | "stopped"
    property string playerState: "stopped"
    property string videoInfo: ""
    property string timeText: "00:00 / 00:00"

    // ── Mouse tracking ────────────────────────────────────────────────

    MouseArea {
        id: mouseTracker
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        propagateComposedEvents: true

        onPositionChanged: showOverlays()
        onEntered: showOverlays()
    }

    function showOverlays() {
        controlBar.opacity = 1.0
        if (playerState != "stopped")
            playBtn.opacity = 1.0
        hideTimer.restart()
    }

    Timer {
        id: hideTimer
        interval: 800
        onTriggered: {
            controlBar.opacity = 0.0
            playBtn.opacity = 0.0
        }
    }

    // ── Center play/pause button ──────────────────────────────────────

    Rectangle {
        id: playBtn
        width: 72
        height: 72
        radius: 36
        color: hoverArea.containsMouse
               ? (hoverArea.pressed ? "#99222222" : "#88222222")
               : "#66000000"
        anchors.centerIn: parent
        opacity: playerState == "stopped" ? 1.0 : 0.0  // always show when stopped

        Behavior on opacity { NumberAnimation { duration: 200 } }
        Behavior on color { ColorAnimation { duration: 100 } }

        // Play/pause icon
        Canvas {
            id: iconCanvas
            anchors.fill: parent
            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)

                ctx.fillStyle = hoverArea.containsMouse ? "#ffffff" : "#e0e0e0"

                var cx = width / 2, cy = height / 2, sz = 14

                if (root.playing) {
                    // Pause bars
                    var barW = sz * 0.55, barH = sz * 2
                    ctx.beginPath()
                    ctx.roundRect(cx - sz + 2, cy - sz, barW, barH, 2)
                    ctx.fill()
                    ctx.beginPath()
                    ctx.roundRect(cx + sz - barW - 2, cy - sz, barW, barH, 2)
                    ctx.fill()
                } else {
                    // Play triangle
                    ctx.beginPath()
                    ctx.moveTo(cx - sz * 0.7, cy - sz)
                    ctx.lineTo(cx - sz * 0.7, cy + sz)
                    ctx.lineTo(cx + sz, cy)
                    ctx.closePath()
                    ctx.fill()
                }
            }
        }

        MouseArea {
            id: hoverArea
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            hoverEnabled: true
            onClicked: {
                controller.togglePlayPause()
            }
        }

        // Redraw icon when state changes
        Connections {
            target: root
            function onPlayingChanged() { iconCanvas.requestPaint() }
        }
    }

    // ── Bottom control bar ────────────────────────────────────────────

    Rectangle {
        id: controlBar
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            margins: 12
        }
        height: 48
        radius: 6
        color: "#8c000000"  // ~55% opacity (matching old rgba(0,0,0,0.55))
        opacity: 0.0

        Behavior on opacity { NumberAnimation { duration: 250 } }

        Row {
            anchors.centerIn: parent
            spacing: 12

            Text {
                text: root.timeText
                color: "white"
                font.pixelSize: 14
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                text: root.videoInfo
                color: "#cccccc"
                font.pixelSize: 12
                anchors.verticalCenter: parent.verticalCenter
                visible: root.videoInfo != ""
            }
        }
    }

    // ── Initial show on startup ───────────────────────────────────────

    Component.onCompleted: {
        controlBar.opacity = 1.0
        hideTimer.start()
    }
}
