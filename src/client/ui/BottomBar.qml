import QtQuick
import QtQuick.Controls
import "components"

Item {
    id: root
    property bool playing: false
    property bool fullscreen: false
    property bool hwDecoding: false
    property real currentTime: 0
    property real duration: 0
    property bool overlaysVisible: true
    property bool volumePopupOpen: false
    property bool qualityPopupOpen: false
    property bool speedPopupOpen: false
    property bool playlistOpen: false

    signal playPauseClicked()
    signal prevClicked()
    signal nextClicked()
    signal stopClicked()
    signal volumeClicked()
    signal qualityClicked()
    signal hwaccelClicked()
    signal speedClicked()
    signal fullscreenClicked()
    signal playlistClicked()

    property alias barHovered: barHover.hovered

    implicitHeight: 48

    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 48
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 1.0; color: "#cc000000" }
        }
        opacity: root.overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

        HoverHandler { id: barHover }

        Row {
            anchors { left: parent.left; verticalCenter: parent.verticalCenter; leftMargin: 12 }
            spacing: 4

            IconButton { codepoint: ""; size: 22; tooltip: "上一个 (B)"
                onClicked: root.prevClicked() }
            IconButton { codepoint: root.playing ? "" : ""; size: 22
                tooltip: root.playing ? "暂停 (Space)" : "播放 (Space)"
                onClicked: root.playPauseClicked() }
            IconButton { codepoint: ""; size: 22; tooltip: "下一个 (N)"
                onClicked: root.nextClicked() }
            IconButton { codepoint: ""; size: 22; tooltip: "停止"
                onClicked: root.stopClicked() }

            Rectangle { width: 1; height: 20; color: "#0fffffff"; anchors.verticalCenter: parent.verticalCenter }

            Text {
                function fmt(ms) {
                    if (ms <= 0) return "0:00"
                    var s = Math.floor(ms/1000), m = Math.floor(s/60)
                    return m + ":" + (s%60 < 10 ? "0" : "") + s%60
                }
                text: fmt(root.currentTime) + " / " + fmt(root.duration)
                color: "#e0e0e0"; font.pixelSize: 13
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Row {
            id: rightRow
            anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 12 }
            spacing: 4

            IconButton { id: volBtn; codepoint: ""; size: 22; tooltip: "音量"
                highlighted: root.volumePopupOpen
                onClicked: root.volumeClicked() }
            IconButton { id: qualBtn; codepoint: ""; size: 22; tooltip: "画质"
                highlighted: root.qualityPopupOpen
                onClicked: root.qualityClicked() }
            IconButton { label: root.hwDecoding ? "硬解" : "软解"; size: 22
                tooltip: root.hwDecoding ? "点击切换软解" : "点击切换硬解"
                onClicked: root.hwaccelClicked() }
            IconButton { id: spdBtn; label: "倍速"; size: 22; tooltip: "播放速度"
                highlighted: root.speedPopupOpen
                onClicked: root.speedClicked() }
            IconButton { codepoint: root.fullscreen ? "" : ""; size: 22
                tooltip: root.fullscreen ? "退出全屏" : "全屏"
                onClicked: root.fullscreenClicked() }
            IconButton { id: playlistBtn; codepoint: ""; size: 22; tooltip: "播放列表 (P)"
                highlighted: root.playlistOpen
                onClicked: root.playlistClicked() }
        }
    }

    property real volumeBtnCenterX: rightRow.x + volBtn.x + volBtn.width / 2
    property real qualityBtnCenterX: rightRow.x + qualBtn.x + qualBtn.width / 2
    property real speedBtnCenterX: rightRow.x + spdBtn.x + spdBtn.width / 2
}
