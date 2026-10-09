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
    property bool aspectPopupOpen: false
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
    signal aspectClicked()
    signal fullscreenClicked()
    signal playlistClicked()
    signal loopClicked()
    signal seeked(real ms)

    /// 自动隐藏保持条件：整个 UI 区域（热区 40 + 进度条 14 + bottombar 48）
    /// 视作**一个连续区域**——单个 HoverHandler 挂在 root 上（implicitHeight
    /// 即该区域高度）。
    /// 反面教训（2026-10-09 用户实测）：此前用"热区 containsMouse || 进度条
    /// hovered || bottombar hovered"三段判定，进度条 band 高 14 而内部 Slider
    /// 只有 6/8，段间余下 6~8px 无任何 hover 源——鼠标自上而下穿过时 UI
    /// "可见→消失→可见"。
    /// progress.pressed 单独保留：拖动中指针可能移出本区域（滑块跟随），
    /// 但拖动状态必须保持 UI 可见。
    readonly property bool mouseInRegion: barHover.hovered || progress.pressed

    // Popup 定位锚点（PopupBase.anchorTarget）
    property alias volumeBtn: volBtn
    property alias qualityBtn: qualBtn
    property alias speedBtn: spdBtn
    property alias tracksBtn: trkBtn
    property alias aspectBtn: aspBtn

    // bottombar(48) + 进度条(14) + 热区(40) 一体
    implicitHeight: 48 + 14 + 40

    /// 覆盖整个 UI 区域（40+14+48）的 hover 源——见 mouseInRegion 注释。
    /// 用 HoverHandler 而非 MouseArea：只观察 hover，不吞鼠标事件
    ///（MouseArea 会吃掉右键，导致该区域内右键菜单失效）。
    HoverHandler { id: barHover }

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

            // 轨道选择 = 按文件记忆（其余按钮 = 全局设置）——置前并
            // 以纵向分割线与全局按钮区分（2026-08-06）
            IconButton { id: trkBtn; codepoint: "󰨖"; size: 22; tooltip: qsTr("Tracks")
                highlighted: root.tracksPopupOpen
                onClicked: root.tracksClicked() }
            Rectangle { width: 1; height: 20; color: "#0fffffff"
                anchors.verticalCenter: parent.verticalCenter }
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

            IconButton { id: aspBtn; codepoint: "󰨤"; size: 22; tooltip: qsTr("Aspect ratio")
                highlighted: root.aspectPopupOpen
                onClicked: root.aspectClicked() }
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
