import QtQuick
import VSR 1.0
import "ui"

/// UI 根：渲染层（Video）+ UI 层叠放。
/// benchmark 模式（benchmarkMode context property）下 UI 层整体隐藏。
Item {
    id: root
    anchors.fill: parent

    // ── 渲染层（z 最低）────────────────────────────────────────────
    Video {
        id: video
        anchors.fill: parent
    }

    // ── UI 层（benchmark 模式隐藏）────────────────────────────────
    Item {
        id: uiLayer
        anchors.fill: parent
        visible: !benchmarkMode
        enabled: !benchmarkMode

        // ── Auto-hide ───────────────────────────────────────────────
        // 显示 = 底部区域 hover（bottomBar.mouseInRegion：热区+进度条+
        // bottombar 一体）或音量/画质/倍速弹窗打开。播放列表打开
        // 不阻止消失（侧边栏独立，不依赖底部 UI）。状态翻转即写回
        // viewModel——移出热区立即隐藏，无计时器。启动即按当前状态
        // 应用（onCompleted 写回初始值；onChanged 只在翻转时触发），
        // 鼠标不在热区则初始隐藏。弹窗注册表化：新增弹窗只需在
        // Component.onCompleted 的 _popups 数组中登记一行。
        property bool showUi: bottomBar.mouseInRegion || anyPopupOpen
        onShowUiChanged: viewModel.overlaysVisible = showUi
        property var _popups: []
        readonly property bool anyPopupOpen: {
            for (var i = 0; i < _popups.length; i++)
                if (_popups[i].visible) return true
            return false
        }
        /// 互斥：打开一个 popup 时关闭其他（非 modal 下多开会叠放）。
        function closeOtherPopups(exclude) {
            for (var i = 0; i < _popups.length; i++)
                if (_popups[i] !== exclude) _popups[i].close()
        }
        Component.onCompleted: {
            _popups = [volumePopup, qualityPopup, speedPopup]
            // 启动即按当前状态应用 auto-hide（onShowUiChanged 只在翻转时触发）
            viewModel.overlaysVisible = showUi
        }

        // ── Top Bar ────────────────────────────────────────────────
        TopBar {
            anchors { left: parent.left; right: parent.right; top: parent.top }
            videoInfo: viewModel.videoInfo
            overlaysVisible: viewModel.overlaysVisible
        }

        // ── Center Play Button ─────────────────────────────────────
        CenterPlayBtn {
            anchors.fill: parent
            playing: viewModel.playing
            overlaysVisible: viewModel.overlaysVisible
            onClicked: viewModel.togglePlayPause()
        }

        // ── Bottom Bar（热区 + 进度条 + bottombar 一体）────────────
        BottomBar {
            id: bottomBar
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            playing: viewModel.playing
            fullscreen: viewModel.fullscreen
            hwDecoding: viewModel.hwDecoding
            muted: viewModel.muted
            currentTime: viewModel.currentTime
            duration: viewModel.duration
            overlaysVisible: viewModel.overlaysVisible
            loopMode: viewModel.loopMode
            volumePopupOpen: volumePopup.visible
            qualityPopupOpen: qualityPopup.visible
            speedPopupOpen: speedPopup.visible
            playlistOpen: playlistPanel.visible
            onSeeked: function(ms) { viewModel.seekAbsolute(ms) }
            onPlayPauseClicked: viewModel.togglePlayPause()
            onPrevClicked: viewModel.playlistPrev()
            onNextClicked: viewModel.playlistNext()
            onStopClicked: viewModel.stop()
            onVolumeClicked: volumePopup.visible ? volumePopup.close() : volumePopup.open()
            onQualityClicked: qualityPopup.visible ? qualityPopup.close() : qualityPopup.open()
            onHwaccelClicked: viewModel.toggleHwaccel()
            onSpeedClicked: speedPopup.visible ? speedPopup.close() : speedPopup.open()
            onFullscreenClicked: viewModel.toggleFullscreen()
            onPlaylistClicked: root.togglePlaylist()
            onLoopClicked: viewModel.toggleLoop()
        }

        // ── Popups（PopupBase.anchorTarget 定位）────────────────────
        VolumePopup {
            id: volumePopup
            anchorTarget: bottomBar.volumeBtn
            volume: viewModel.volume
            muted: viewModel.muted
            onVolAdjusted: function(v) { viewModel.setVolume(v) }
            onMuteToggled: viewModel.toggleMute()
            onOpened: closeOtherPopups(volumePopup)
        }

        QualityPopup {
            id: qualityPopup
            anchorTarget: bottomBar.qualityBtn
            scal: viewModel.scale
            quality: viewModel.quality
            denoiseQuality: viewModel.denoiseQuality
            onScalPicked: function(v) { viewModel.setScale(v) }
            onQualityPicked: function(v) { viewModel.setQuality(v) }
            onDenoiseQualityPicked: function(v) { viewModel.setDenoiseQuality(v) }
            onOpened: closeOtherPopups(qualityPopup)
        }

        SpeedPopup {
            id: speedPopup
            anchorTarget: bottomBar.speedBtn
            speed: viewModel.speed
            onSpeedAdjusted: function(v) { viewModel.setSpeed(v) }
            onOpened: closeOtherPopups(speedPopup)
        }

        // ── Playlist Panel ─────────────────────────────────────────
        PlaylistPanel {
            id: playlistPanel
        }

        // ── Fullscreen 双向同步 + 键盘（快捷键单点）─────────────────
        Connections { target: window
            function onVisibilityChanged() { viewModel.fullscreen = (window.visibility === Window.FullScreen) } }
        Connections { target: viewModel
            function onFullscreenChanged() {
                var wantFs = viewModel.fullscreen
                var isFs = (window.visibility === Window.FullScreen)
                if (wantFs !== isFs) window.visibility = wantFs ? Window.FullScreen : Window.Windowed
            } }

        focus: true
        Keys.onPressed: function(event) {
            switch (event.key) {
            case Qt.Key_Escape:
                // 全屏 → 退全屏；播放列表打开 → 关闭；其余不响应。
                // Esc 不控制播放停止。
                if (viewModel.fullscreen) viewModel.toggleFullscreen()
                else if (playlistPanel.visible) playlistPanel.close()
                event.accepted = true
                break
            case Qt.Key_Space:
                viewModel.togglePlayPause(); event.accepted = true; break
            case Qt.Key_Left:
                viewModel.seekRelative(event.modifiers & Qt.ShiftModifier ? -10000 : -5000)
                event.accepted = true; break
            case Qt.Key_Right:
                viewModel.seekRelative(event.modifiers & Qt.ShiftModifier ? 10000 : 5000)
                event.accepted = true; break
            case Qt.Key_Up:
                viewModel.setVolume(viewModel.volume + 0.05); event.accepted = true; break
            case Qt.Key_Down:
                viewModel.setVolume(viewModel.volume - 0.05); event.accepted = true; break
            case Qt.Key_S:
                viewModel.screenshot(); event.accepted = true; break
            case Qt.Key_Tab:
                viewModel.toggleOsd(); event.accepted = true; break
            case Qt.Key_N:
                viewModel.playlistNext(); event.accepted = true; break
            case Qt.Key_B:
                viewModel.playlistPrev(); event.accepted = true; break
            case Qt.Key_BracketLeft:
                viewModel.setSpeed(0.5); event.accepted = true; break
            case Qt.Key_BracketRight:
                viewModel.setSpeed(2.0); event.accepted = true; break
            case Qt.Key_Backslash:
                viewModel.setSpeed(1.0); event.accepted = true; break
            case Qt.Key_P:
                root.togglePlaylist(); event.accepted = true; break
            case Qt.Key_F:
                viewModel.toggleFullscreen(); event.accepted = true; break
            }
        }
    }

    // ── root 级函数（uiLayer 之外）─────────────────────────────────
    // togglePlaylist 必须定义在 root 上：调用方是 root.togglePlaylist()
    //（BottomBar onPlaylistClicked / P 键）。若定义在 uiLayer 内，
    // root 上找不到该函数 → 运行时 TypeError → 点击无反应。
    function togglePlaylist() {
        playlistPanel.visible ? playlistPanel.close() : playlistPanel.open()
    }
}
