import QtQuick
import QtQuick.Controls
import "components"

Item {
    id: root
    anchors.fill: parent

    // ── Font ─────────────────────────────────────────────────────────

    FontLoader {
        id: materialIcons
        source: "file:///usr/share/fonts/TTF/MaterialIcons-Regular.ttf"
    }
    readonly property string iconFont: materialIcons.name

    // ── Auto-hide ────────────────────────────────────────────────────
    // Mouse movement resets the 3s timer. After 3s of no movement and
    // no open popup, overlays hide. Standard video player behavior.

    MouseArea {
        anchors.fill: parent; hoverEnabled: true
        onPositionChanged: { viewModel.overlaysVisible = true; hideTimer.restart() }
    }
    Timer {
        id: hideTimer; interval: 3000
        onTriggered: {
            var popupOpen = volumePopup.visible || qualityPopup.visible || speedPopup.visible
            if (!popupOpen && !bottomBar.barHovered && !progressHotZone.containsMouse)
                viewModel.overlaysVisible = false
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // Top Bar
    // ══════════════════════════════════════════════════════════════════

    TopBar {
        id: topBar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        videoInfo: viewModel.videoInfo
        overlaysVisible: viewModel.overlaysVisible
    }

    // ══════════════════════════════════════════════════════════════════
    // Center Play Button
    // ══════════════════════════════════════════════════════════════════

    CenterPlayBtn {
        anchors.fill: parent
        playing: viewModel.playing
        overlaysVisible: viewModel.overlaysVisible
        onClicked: viewModel.togglePlayPause()
    }

    // ══════════════════════════════════════════════════════════════════
    // OSD Overlay (Tab toggle)
    // ══════════════════════════════════════════════════════════════════

    OsdOverlay {
        osdVisible: viewModel.osdVisible
        osdText: viewModel.osdText
    }

    // ══════════════════════════════════════════════════════════════════
    // Progress Slider
    // ══════════════════════════════════════════════════════════════════

    // Invisible hot zone above bottom bar — prevents auto-hide when
    // mouse is near the progress slider (visual height ~6px is too narrow).
    MouseArea {
        id: progressHotZone
        anchors { left: parent.left; right: parent.right; bottom: bottomBar.top }
        height: 20
        hoverEnabled: true
    }

    ProgressSlider {
        id: progressSlider
        anchors { left: parent.left; right: parent.right; bottom: bottomBar.top }
        duration: viewModel.duration
        currentTime: viewModel.currentTime
        overlaysVisible: viewModel.overlaysVisible
        onSeeked: function(ms) { viewModel.seekAbsolute(ms) }
    }

    // ══════════════════════════════════════════════════════════════════
    // Bottom Bar
    // ══════════════════════════════════════════════════════════════════

    BottomBar {
        id: bottomBar
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        playing: viewModel.playing
        fullscreen: viewModel.fullscreen
        hwDecoding: viewModel.hwDecoding
        currentTime: viewModel.currentTime
        duration: viewModel.duration
        overlaysVisible: viewModel.overlaysVisible
        volumePopupOpen: volumePopup.visible
        qualityPopupOpen: qualityPopup.visible
        speedPopupOpen: speedPopup.visible
        playlistOpen: playlistPanel.visible
        onPlayPauseClicked: viewModel.togglePlayPause()
        onPrevClicked: { var f = playlist.previous(); if (f) viewModel.loadFile(f) }
        onNextClicked: { var f = playlist.next(); if (f) viewModel.loadFile(f) }
        onStopClicked: viewModel.stop()
        onVolumeClicked: volumePopup.visible ? volumePopup.close() : volumePopup.open()
        onQualityClicked: qualityPopup.visible ? qualityPopup.close() : qualityPopup.open()
        onHwaccelClicked: viewModel.toggleHwaccel()
        onSpeedClicked: speedPopup.visible ? speedPopup.close() : speedPopup.open()
        onFullscreenClicked: viewModel.toggleFullscreen()
        onPlaylistClicked: root.togglePlaylist()
    }

    // ══════════════════════════════════════════════════════════════════
    // Volume Popup
    // ══════════════════════════════════════════════════════════════════

    VolumePopup {
        id: volumePopup
        x: Math.min(Math.max(bottomBar.volumeBtnCenterX - width/2, 8), root.width - width - 8)
        y: bottomBar.y - height - 8
        volume: viewModel.volume
        muted: viewModel.muted
        onVolAdjusted: function(v) { viewModel.setVolume(v) }
        onMuteToggled: viewModel.toggleMute()
    }

    // ══════════════════════════════════════════════════════════════════
    // Quality Popup
    // ══════════════════════════════════════════════════════════════════

    QualityPopup {
        id: qualityPopup
        x: Math.min(Math.max(bottomBar.qualityBtnCenterX - width/2, 8), root.width - width - 8)
        y: bottomBar.y - height - 8
        scal: viewModel.scale
        qualit: viewModel.quality
        denoiseQualit: viewModel.denoiseQuality
        onScalPicked: function(v) { viewModel.setScale(v) }
        onQualitPicked: function(v) { viewModel.setQuality(v) }
        onDenoiseQualitPicked: function(v) { viewModel.setDenoiseQuality(v) }
    }

    // ══════════════════════════════════════════════════════════════════
    // Speed Popup
    // ══════════════════════════════════════════════════════════════════

    SpeedPopup {
        id: speedPopup
        x: Math.min(Math.max(bottomBar.speedBtnCenterX - width/2, 8), root.width - width - 8)
        y: bottomBar.y - height - 8
        speed: viewModel.speed
        onSpeedAdjusted: function(v) { viewModel.setSpeed(v) }
    }

    // ══════════════════════════════════════════════════════════════════
    // Playlist Panel
    // ══════════════════════════════════════════════════════════════════

    PlaylistPanel {
        id: playlistPanel
        onFileSelected: function(path) { viewModel.loadFile(path) }
    }

    // Dismiss playlist (click outside)
    MouseArea { anchors { left: parent.left; right: parent.right; top: parent.top; bottom: bottomBar.top }
        visible: playlistPanel.visible; z: 5; onClicked: playlistPanel.close() }

    // ══════════════════════════════════════════════════════════════════
    // Fullscreen sync + Keyboard
    // ══════════════════════════════════════════════════════════════════

    // Window ↔ ViewModel fullscreen sync (bidirectional, guards prevent loops)
    Connections { target: window
        function onVisibilityChanged() { viewModel.fullscreen = (window.visibility === Window.FullScreen) } }
    Connections { target: viewModel
        function onFullscreenChanged() {
            var wantFs = viewModel.fullscreen
            var isFs = (window.visibility === Window.FullScreen)
            if (wantFs !== isFs) window.visibility = wantFs ? Window.FullScreen : Window.Windowed
        } }

    focus: true
    function togglePlaylist() { playlistPanel.visible ? playlistPanel.close() : playlistPanel.open() }
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape) {
            if (playlistPanel.visible) playlistPanel.close(); else viewModel.stop()
        }
        if (event.key === Qt.Key_Space) { viewModel.togglePlayPause(); event.accepted = true }
        if (event.key === Qt.Key_P) root.togglePlaylist()
        if (event.key === Qt.Key_F) viewModel.toggleFullscreen()
    }

    Component.onCompleted: console.log("[ui] overlay loaded")
}
