import QtQuick
import QtQuick.Dialogs
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
        // bottombar 一体）或顶部区域 hover（topBar.mouseInRegion：渐变条
        // + 下方 40px 热区——鼠标靠近顶缘即显示，否则顶部打开按钮
        // 不可达）或音量/画质/倍速弹窗打开。播放列表打开不阻止消失
        //（侧边栏独立，不依赖底部 UI）。状态翻转即写回 viewModel——
        // 移出热区立即隐藏，无计时器。启动即按当前状态应用（onCompleted
        // 写回初始值；onChanged 只在翻转时触发），鼠标不在热区则初始
        // 隐藏。弹窗注册表化：新增弹窗只需在 Component.onCompleted 的
        // _popups 数组中登记一行。
        property bool showUi: bottomBar.mouseInRegion || topBar.mouseInRegion || anyPopupOpen
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
            _popups = [volumePopup, qualityPopup, speedPopup, contextMenu, tracksPopup]
            // 启动即按当前状态应用 auto-hide（onShowUiChanged 只在翻转时触发）
            viewModel.overlaysVisible = showUi
        }

        // 右键菜单触发器（最底层：右击未被按钮/弹窗消费时到达此处）
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: {
                if (mouse.button === Qt.RightButton) {
                    closeOtherPopups(contextMenu)
                    contextMenu.showAt(uiLayer, mouse.x, mouse.y)
                }
            }
        }

        // ── Top Bar ────────────────────────────────────────────────
        TopBar {
            id: topBar
            anchors { left: parent.left; right: parent.right; top: parent.top }
            videoInfo: viewModel.videoInfo
            overlaysVisible: viewModel.overlaysVisible
            onOpenRequested: function() { fileDialog.mode = 0; fileDialog.open() }
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
            tracksPopupOpen: tracksPopup.visible
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
            onTracksClicked: tracksPopup.visible ? tracksPopup.close() : tracksPopup.open()
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

        TracksPopup {
            id: tracksPopup
            // modal 居中呈现（无需 anchorTarget）
            trackList: viewModel.trackList
            subVisible: viewModel.subVisible
            subDelay: viewModel.subDelay
            onTrackSelected: function(type, id) { viewModel.selectTrack(type, id) }
            onSubVisibilityToggled: viewModel.toggleSubtitles()
            onSubDelayAdjusted: function(d) { viewModel.adjustSubDelay(d) }
            onSubFileDialogRequested: function() { fileDialog.mode = 2; fileDialog.open() }
            onOpened: closeOtherPopups(tracksPopup)
        }

        // ── Playlist Panel ─────────────────────────────────────────
        PlaylistPanel {
            id: playlistPanel
        }

        // ── Context Menu（右键，showAt 定位 = 鼠标位置）──────────────
        ContextMenu {
            id: contextMenu
            onOpenFilesRequested: function() { fileDialog.mode = 0; fileDialog.open() }
            onAppendFilesRequested: function() { fileDialog.mode = 1; fileDialog.open() }
            onLoadSubsRequested: function() { fileDialog.mode = 2; fileDialog.open() }
            onPlayPauseRequested: viewModel.togglePlayPause()
            onStopRequested: viewModel.stop()
            onFullscreenRequested: viewModel.toggleFullscreen()
            onPlaylistRequested: root.togglePlaylist()
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
            case Qt.Key_O:
                if (event.modifiers === Qt.ControlModifier) {
                    fileDialog.mode = 0; fileDialog.open(); event.accepted = true
                }
                break
            case Qt.Key_V:
                viewModel.toggleSubtitles(); event.accepted = true; break
            case Qt.Key_P:
                root.togglePlaylist(); event.accepted = true; break
            case Qt.Key_F:
                viewModel.toggleFullscreen(); event.accepted = true; break
            }
        }
    }

    // ── 文件打开（FileDialog，TopBar 按钮 / Ctrl+O 触发）──────────
    FileDialog {
        id: fileDialog
        title: qsTr("Open media")
        fileMode: FileDialog.OpenFiles
        /// 过滤器按 mode 切换：加载字幕（2）时字幕优先，否则媒体优先
        nameFilters: fileDialog.mode === 2 ? [
            qsTr("Subtitle files (*.srt *.ass *.ssa *.vtt *.sub *.sbv)"),
            qsTr("All files (*)")
        ] : [
            qsTr("Media files (*.mp4 *.mkv *.webm *.avi *.mov *.ts *.flv *.wmv *.mp3 *.flac *.wav *.ogg *.m4a)"),
            qsTr("Video files (*.mp4 *.mkv *.webm *.avi *.mov *.ts *.flv *.wmv)"),
            qsTr("Audio files (*.mp3 *.flac *.wav *.ogg *.m4a)"),
            qsTr("Subtitle files (*.srt *.ass *.ssa *.vtt *.sub *.sbv)"),
            qsTr("All files (*)")
        ]
        /// 打开方式：0=replace+queue（打开/拖放）1=append（追加到列表）
        /// 2=加载字幕（sub-add，后续入口用）
        property int mode: 0
        onAccepted: {
            var paths = []
            for (var i = 0; i < selectedFiles.length; i++)
                paths.push(selectedFiles[i].toString())
            viewModel.openFiles(paths, mode)
        }
    }

    // ── 拖放（窗口级）：字幕文件 → sub-add；其余 → 播放（多文件排队）──
    DropArea {
        id: dropArea
        anchors.fill: parent
        // benchmark 模式无 UI——拖入不触发 loadfile
        enabled: !benchmarkMode
        onEntered: function(drag) { drag.accepted = true }
        onDropped: function(drop) {
            var paths = []
            for (var i = 0; i < drop.urls.length; i++)
                paths.push(drop.urls[i].toString())
            viewModel.openFiles(paths, 0)
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
