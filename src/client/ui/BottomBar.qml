import QtQuick
import QtQuick.Controls
import "components"

Item {
    id: root
    property bool playing: false
    property bool fullscreen: false
    property bool hwDecoding: false
    property bool muted: false
    property real currentTime: 0
    property real duration: 0
    property bool overlaysVisible: true
    property bool volumePopupOpen: false
    property bool qualityPopupOpen: false
    property bool speedPopupOpen: false
    property bool tracksPopupOpen: false
    property bool playlistOpen: false
    property int loopMode: 0    // 0=No loop, 1=Loop file, 2=Loop playlist

    signal playPauseClicked()
    signal prevClicked()
    signal nextClicked()
    signal stopClicked()
    signal volumeClicked()
    signal qualityClicked()
    signal hwaccelClicked()
    signal speedClicked()
    signal tracksClicked()
    signal fullscreenClicked()
    signal playlistClicked()
    signal loopClicked()
    signal seeked(real ms)

    /// 自动隐藏保持条件：热区/进度条/bottombar 任一 hover 或拖动中。
    /// 拖动进度条时鼠标在滑块上（不在热区内）——pressed 单独保护。
    /// 宿主（main.qml）用此状态驱动 overlaysVisible（showUi 绑定）。
    readonly property bool mouseInRegion: hotZone.containsMouse
                                          || progress.hovered || progress.pressed
                                          || barHover.hovered

    // Popup 定位锚点（PopupBase.anchorTarget）
    property alias volumeBtn: volBtn
    property alias qualityBtn: qualBtn
    property alias speedBtn: spdBtn
    property alias tracksBtn: trkBtn

    // bottombar(48) + 进度条(14) + 热区(40) 一体
    implicitHeight: 48 + 14 + 40

    // ── 热区（进度条上方——鼠标靠近进度条即显示/保持 UI）─────────
    MouseArea {
        id: hotZone
        anchors { left: parent.left; right: parent.right; bottom: progress.top }
        height: 40
        hoverEnabled: true
    }

    // ── 进度条（贴 bottombar 上沿，拖动中 hovered/pressed 保持 UI）─
    ProgressSlider {
        id: progress
        anchors { left: parent.left; right: parent.right; bottom: bottombar.top }
        duration: root.duration
        currentTime: root.currentTime
        overlaysVisible: root.overlaysVisible
        onSeeked: function(ms) { root.seeked(ms) }
    }

    // ── bottombar ────────────────────────────────────────────────
    Rectangle {
        id: bottombar
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

            IconButton { codepoint: "󰒮"; size: 22; tooltip: qsTr("Previous (B)")
                onClicked: root.prevClicked() }
            IconButton { codepoint: root.playing ? "󰏤" : "󰐊"; size: 22
                tooltip: root.playing ? qsTr("Pause (Space)") : qsTr("Play (Space)")
                onClicked: root.playPauseClicked() }
            IconButton { codepoint: "󰒭"; size: 22; tooltip: qsTr("Next (N)")
                onClicked: root.nextClicked() }
            IconButton { codepoint: "󰓛"; size: 22; tooltip: qsTr("Stop")
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

            IconButton { id: volBtn
                // 静音状态由图标表达（与 VolumePopup 同字形 󰖁/󰕾）
                codepoint: root.muted ? "󰖁" : "󰕾"; size: 22; tooltip: qsTr("Volume")
                highlighted: root.volumePopupOpen
                onClicked: root.volumeClicked() }
            IconButton { id: qualBtn; codepoint: "󰐵"; size: 22; tooltip: qsTr("Quality")
                highlighted: root.qualityPopupOpen
                onClicked: root.qualityClicked() }
            IconButton { label: root.hwDecoding ? qsTr("HW") : qsTr("SW"); size: 22
                tooltip: root.hwDecoding ? qsTr("Switch to SW decode") : qsTr("Switch to HW decode")
                onClicked: root.hwaccelClicked() }
            IconButton { id: spdBtn; label: qsTr("Speed"); size: 22; tooltip: qsTr("Playback speed")
                highlighted: root.speedPopupOpen
                onClicked: root.speedClicked() }
            IconButton { id: trkBtn; codepoint: "󰨖"; size: 22; tooltip: qsTr("Tracks")
                highlighted: root.tracksPopupOpen
                onClicked: root.tracksClicked() }
            IconButton { id: loopBtn
                // 三态三字形：No loop=loop(环形箭头)/单曲=repeat_one/
                // 列表=repeat（E028/E041/E040）。状态只由图标区分，
                // 不做背景持续高亮。
                codepoint: root.loopMode === 0 ? "󰑗" :
                           root.loopMode === 1 ? "󰑘" : "󰑖"
                size: 22
                tooltip: root.loopMode === 1 ? qsTr("Loop file") :
                          root.loopMode === 2 ? qsTr("Loop playlist") : qsTr("No loop")
                onClicked: root.loopClicked() }
            IconButton { codepoint: root.fullscreen ? "󰊔" : "󰊓"; size: 22
                tooltip: root.fullscreen ? qsTr("Exit fullscreen") : qsTr("Fullscreen")
                onClicked: root.fullscreenClicked() }
            IconButton { id: playlistBtn; codepoint: "󰐑"; size: 22; tooltip: qsTr("Playlist (P)")
                highlighted: root.playlistOpen
                onClicked: root.playlistClicked() }
        }
    }
}
