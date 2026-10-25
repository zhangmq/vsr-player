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
    // controlMode = 桌面 UI 窗口：不创建 Video（视频只在 presentation 主窗口）。
    Loader {
        id: videoLoader
        anchors.fill: parent
        active: !controlMode
        sourceComponent: Component {
            Video {
                id: video
                anchors.fill: parent
            }
        }
    }

    // ── UI 层（benchmark 模式隐藏）────────────────────────────────
    Item {
        id: uiLayer   // 信号处理器作用域不含父对象——弹窗互斥须经 uiLayer 显式调用
        // presentation 主窗口在 3D 时只留 Video + OSD；桌面 control 窗口
        // 载入同一份 UI，主 UI 始终在桌面可用。
        anchors.fill: parent
        visible: !benchmarkMode && (controlMode || !stereo.active)
        enabled: !benchmarkMode

        // 2D→3D 占位提示（本期不启动 3D 输出）。
        Rectangle {
            id: stereoToast
            z: 1000
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 96
            width: toastText.implicitWidth + 32
            height: 38
            radius: 6
            color: "#dd202020"
            border.width: 1; border.color: "#33ffffff"
            opacity: 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 150 } }
            Text {
                id: toastText
                anchors.centerIn: parent
                color: "#e0e0e0"; font.pixelSize: 14
            }
            Timer {
                id: toastTimer
                interval: 2000
                onTriggered: stereoToast.opacity = 0
            }
            function show(msg) {
                toastText.text = msg
                opacity = 1
                toastTimer.restart()
            }
        }

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
            _popups = [volumePopup, qualityPopup, trueHdrAdvancedPopup, speedPopup,
                       aspectPopup, contextMenu, tracksPopup, openMenu, urlPopup]
            // 启动即按当前状态应用 auto-hide（onShowUiChanged 只在翻转时触发）
            viewModel.overlaysVisible = showUi
        }

        // 右键菜单触发器（最底层：右击未被按钮/弹窗消费时到达此处）
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            // 3D presentation（主窗口在眼镜屏）不响应右键；此时 UI 在 control 窗口。
            enabled: controlMode || !stereo.active
            onClicked: {
                if (mouse.button === Qt.RightButton) {
                    uiLayer.closeOtherPopups(contextMenu)
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
            onOpenMenuRequested: openMenu.open()
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
            hwDecodeAvailable: viewModel.hwDecodeAvailable
            hwDecodeReason: viewModel.hwDecodeReason
            aiAvailable: viewModel.aiAvailable
            muted: viewModel.muted
            currentTime: viewModel.currentTime
            duration: viewModel.duration
            overlaysVisible: viewModel.overlaysVisible
            loopMode: viewModel.loopMode
            volumePopupOpen: volumePopup.visible
            qualityPopupOpen: qualityPopup.visible
            speedPopupOpen: speedPopup.visible
            tracksPopupOpen: tracksPopup.visible
            aspectPopupOpen: aspectPopup.visible
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
            onAspectClicked: aspectPopup.visible ? aspectPopup.close() : aspectPopup.open()
            onFullscreenClicked: viewModel.toggleFullscreen()
            onPlaylistClicked: root.togglePlaylist()
            onLoopClicked: viewModel.toggleLoop()
        }

        // ── 右上角打开菜单（文件/文件夹/URL 三入口）──────────────────
        OpenMenu {
            id: openMenu
            anchorTarget: topBar.openBtn
            onFileRequested: function() { fileDialog.mode = 0; fileDialog.open() }
            onFolderRequested: function() { folderDialog.mode = 0; folderDialog.open() }
            onUrlRequested: urlPopup.open()
            onOpened: uiLayer.closeOtherPopups(openMenu)
        }

        // ── URL 输入弹窗（Ctrl+L 触发）──────────────────────────────
        UrlPopup {
            id: urlPopup
            onUrlEntered: function(url) { viewModel.openFiles([url], 4) }
            onOpened: uiLayer.closeOtherPopups(urlPopup)
        }

        // ── Popups（PopupBase.anchorTarget 定位）────────────────────
        VolumePopup {
            id: volumePopup
            anchorTarget: bottomBar.volumeBtn
            volume: viewModel.volume
            muted: viewModel.muted
            onVolAdjusted: function(v) { viewModel.setVolume(v) }
            onMuteToggled: viewModel.toggleMute()
            onOpened: uiLayer.closeOtherPopups(volumePopup)
        }

        QualityPopup {
            id: qualityPopup
            anchorTarget: bottomBar.qualityBtn
            aiAvailable: viewModel.aiAvailable
            scal: viewModel.scale
            quality: viewModel.quality
            denoiseQuality: viewModel.denoiseQuality
            frucFps: viewModel.frucFps
            frucModel: viewModel.frucModel   // 档位标签随编译后端分支（C++ 单一来源）
            frucQuality: viewModel.frucQuality
            frucQualityModel: viewModel.frucQualityModel  // 同上（仅 VFG 分支有档）
            frucQualityAvailable: viewModel.frucQualityAvailable  // RIFE 分支隐藏该排
            frucMaxFps: viewModel.frucMaxFps
            frucMaxFpsModel: viewModel.frucMaxFpsModel    // 48/60/120 fps
            frucMaxFpsAvailable: viewModel.frucMaxFpsAvailable  // RIFE 分支隐藏该排
            onScalPicked: function(v) { viewModel.setScale(v) }
            onQualityPicked: function(v) { viewModel.setQuality(v) }
            onDenoiseQualityPicked: function(v) { viewModel.setDenoiseQuality(v) }
            onFrucFpsPicked: function(v) { viewModel.setFrucFps(v) }
            onFrucQualityPicked: function(v) { viewModel.setFrucQuality(v) }
            onFrucMaxFpsPicked: function(v) { viewModel.setFrucMaxFps(v) }
            upHdrEnabled: viewModel.upHdrEnabled
            upHdrEffective: viewModel.upHdrEffective
            onUpHdrEnabledPicked: function(v) { viewModel.setUpHdrEnabled(v) }
            // 高级参数独立弹框（modal 居中）：打开时**关掉 QualityPopup**
            onAdvancedRequested: {
                uiLayer.closeOtherPopups(trueHdrAdvancedPopup)
                trueHdrAdvancedPopup.open()
            }
            onOpened: uiLayer.closeOtherPopups(qualityPopup)
        }

        // ── TrueHDR（SDR→HDR10）参数弹框 ─────────────────────────────
        TrueHdrAdvancedPopup {
            id: trueHdrAdvancedPopup
            upHdrOn: viewModel.upHdrEnabled
            upHdrEffective: viewModel.upHdrEffective
            lumOverride: viewModel.upHdrLuminanceOverride
            lum: viewModel.upHdrLuminance
            lumAuto: viewModel.upHdrAutoLuminance
            saturation: viewModel.upHdrSaturation
            contrast: viewModel.upHdrContrast
            middleGray: viewModel.upHdrMiddleGray
            debanding: viewModel.upHdrDebanding
            limits: viewModel.upHdrLimits
            onLumOverrideToggled: function(on) { viewModel.setUpHdrLuminanceOverride(on) }
            onLumMoved: function(v) { viewModel.setUpHdrLuminance(v) }
            onSaturationMoved: function(v) { viewModel.setUpHdrSaturation(v) }
            onContrastMoved: function(v) { viewModel.setUpHdrContrast(v) }
            onMiddleGrayMoved: function(v) { viewModel.setUpHdrMiddleGray(v) }
            onDebandingToggled: function(on) { viewModel.setUpHdrDebanding(on) }
        }

        SpeedPopup {
            id: speedPopup
            anchorTarget: bottomBar.speedBtn
            speed: viewModel.speed
            onSpeedAdjusted: function(v) { viewModel.setSpeed(v) }
            onOpened: uiLayer.closeOtherPopups(speedPopup)
        }

        AspectPopup {
            id: aspectPopup
            anchorTarget: bottomBar.aspectBtn
            aspect: viewModel.aspect
            onAspectPicked: function(v) { viewModel.setAspect(v) }
            onOpened: uiLayer.closeOtherPopups(aspectPopup)
        }

        TracksPopup {
            id: tracksPopup
            // modal 居中呈现（无需 anchorTarget）
            trackList: viewModel.trackList
            subVisible: viewModel.subVisible
            subDelay: viewModel.subDelay
            onTrackSelected: function(type, id) { viewModel.selectTrack(type, id) }
            onSubDelayAdjusted: function(d) { viewModel.adjustSubDelay(d) }
            onSubFileDialogRequested: function() { fileDialog.mode = 2; fileDialog.open() }
            onOpened: uiLayer.closeOtherPopups(tracksPopup)
        }

        // ── Playlist Panel ─────────────────────────────────────────
        PlaylistPanel {
            id: playlistPanel
        }

        // ── Context Menu（右键，showAt 定位 = 鼠标位置）──────────────
        ContextMenu {
            id: contextMenu
            // 仅主 UI 窗口可用；presentation 窗口不挂 context menu。
            enabled: controlMode || !stereo.active
            onOpenFilesRequested: function() { fileDialog.mode = 0; fileDialog.open() }
            onOpenFolderRequested: function() { folderDialog.mode = 0; folderDialog.open() }
            onOpenUrlRequested: urlPopup.open()
            onAppendFilesRequested: function() { fileDialog.mode = 1; fileDialog.open() }
            onAppendFolderRequested: function() { folderDialog.mode = 1; folderDialog.open() }
            onRemoveFromPlaylistRequested: viewModel.removeCurrentFromPlaylist()
            onDeleteFileRequested: viewModel.deleteCurrentFile()
            onDeinterlaceSelected: function(mode) { viewModel.setDeintMode(mode) }
            onRotateSelected: function(mode) { viewModel.setRotMode(mode) }
            onStereoSelected: function(mode) {
                // ContextMenu 是 Popup.Item，内容挂在窗口 overlay 上；打开另一个
                // Popup 不会自动关它（Qt 文档/源码无此语义），必须先显式关闭，
                // 否则它会留在选择器下面，随后跟着主窗口一起移到眼镜屏。
                contextMenu.close()
                if (mode === 0) {
                    stereo.stop()
                } else if (mode === 1) {
                    // 等当前 popup 的 close 处理完再开选择器，避免两个 popup
                    // 同帧叠放/抢 grab。
                    Qt.callLater(function() { stereoPicker.openForMode(1) })
                } else {
                    stereoToast.show(qsTr("2D to 3D is not implemented yet"))
                }
            }
            onQuitRequested: Qt.quit()   // 走标准退出路径（watch-later 落盘）
        }

        StereoPicker { id: stereoPicker }

        // ── Fullscreen 双向同步 + 键盘（快捷键单点）─────────────────
        Connections { target: window
            enabled: !controlMode
            function onVisibilityChanged() { viewModel.fullscreen = (window.visibility === Window.FullScreen) } }
        Connections { target: viewModel
            enabled: !controlMode
            function onFullscreenChanged() {
                var wantFs = viewModel.fullscreen
                var isFs = (window.visibility === Window.FullScreen)
                if (wantFs !== isFs) window.visibility = wantFs ? Window.FullScreen : Window.Windowed
            } }

        // 3D 启动时主窗口会移到眼镜屏；Popup.Item 没有独立窗口，会跟着窗口
        // 一起过去。这里在主窗口移动前把主窗口里的 popup 全部关掉（不是靠
        // uiLayer.visible，overlay 里的 popup 不受它影响）。
        Connections {
            target: stereo
            function onActiveChanged() {
                if (stereo.active) {
                    uiLayer.closeOtherPopups(null)
                    playlistPanel.close()
                    stereoPicker.close()
                }
            }
        }

        // 及时关闭：窗口失活/隐藏、UI 层隐藏（自动隐藏或进入 3D）时，
        // Popup.Item 不会收到“窗口外”的点击，必须显式关。
        Connections {
            target: uiLayer
            function onVisibleChanged() {
                if (!uiLayer.visible) {
                    contextMenu.close()
                    stereoPicker.close()
                }
            }
        }
        Connections {
            target: window
            function onActiveChanged() {
                if (!window.active) {
                    contextMenu.close()
                    stereoPicker.close()
                }
            }
            function onVisibleChanged() {
                if (!window.visible) {
                    contextMenu.close()
                    stereoPicker.close()
                }
            }
        }

        focus: true

        // ── 窗口级快捷键（Shortcut，与焦点无关）────────────────────
        // 不能用 Keys.onPressed 做全局快捷键：Keys 只投递给 activeFocusItem，
        // 任何可聚焦控件被点中就会把按键全部吃掉（进度条 Slider 在非 macOS 上
        // focusPolicy = StrongFocus，点一下它，方向键就进了 Slider 自己的
        // keyPressEvent——stepSize=0 时只改 0.1 ms，松手还会触发 seeked，
        // 现象是"画面原地跳、快进快退失效"）。
        // Shortcut 走 QShortcutMap：qguiapplication.cpp 的 processKeyEvent →
        // handleShortcutEvent 在**把按键投递给焦点项之前**匹配，因此换焦点
        // 不影响快捷键。
        // 弹窗打开时整体禁用：URL 输入框里要能正常打字，弹窗内控件保留自己的
        // 按键（Esc 由弹窗自己处理；播放列表 Drawer 不在 _popups 里，照常）。
        readonly property bool shortcutsEnabled: !uiLayer.anyPopupOpen

        Shortcut {
            // 3D presentation 期间不响应 Esc（主窗口可能仍被合成器聚焦，
            // Esc 退全屏会把窗口切回焦点屏）；退出用桌面 3D → Off。
            enabled: root.shortcutsEnabled && !stereo.active; sequence: "Escape"
            onActivated: {
                // 全屏 → 退全屏；播放列表打开 → 关闭；其余不响应。Esc 不控制播放停止。
                if (viewModel.fullscreen) viewModel.toggleFullscreen()
                else if (playlistPanel.visible) playlistPanel.close()
            }
        }
        Shortcut {
            enabled: root.shortcutsEnabled && !stereo.active; sequence: "Space"
            onActivated: viewModel.togglePlayPause()
        }
        // 当前条目操作：Delete = 仅从列表移除；Shift+Delete = 连同文件移入回收站
        //（破坏性操作不设确认弹窗，但回收站可恢复）。
        Shortcut {
            enabled: root.shortcutsEnabled; sequence: "Delete"
            onActivated: viewModel.removeCurrentFromPlaylist()
        }
        Shortcut {
            enabled: root.shortcutsEnabled; sequence: "Shift+Delete"
            onActivated: viewModel.deleteCurrentFile()
        }
        Shortcut {
            enabled: root.shortcutsEnabled && !stereo.active; sequence: "Left"
            onActivated: viewModel.seekRelative(-5000)
        }
        Shortcut {
            enabled: root.shortcutsEnabled && !stereo.active; sequence: "Shift+Left"
            onActivated: viewModel.seekRelative(-10000)
        }
        Shortcut {
            enabled: root.shortcutsEnabled && !stereo.active; sequence: "Right"
            onActivated: viewModel.seekRelative(5000)
        }
        Shortcut {
            enabled: root.shortcutsEnabled && !stereo.active; sequence: "Shift+Right"
            onActivated: viewModel.seekRelative(10000)
        }
        Shortcut {
            enabled: root.shortcutsEnabled && !stereo.active; sequence: "Up"
            onActivated: viewModel.setVolume(viewModel.volume + 0.05)
        }
        Shortcut {
            enabled: root.shortcutsEnabled && !stereo.active; sequence: "Down"
            onActivated: viewModel.setVolume(viewModel.volume - 0.05)
        }
        Shortcut {
            enabled: root.shortcutsEnabled; sequence: "S"
            onActivated: viewModel.screenshot()
        }
        Shortcut {
            // 单帧步进（调试插帧帧序）：暂停态前进 1 帧
            enabled: root.shortcutsEnabled; sequence: "."
            onActivated: viewModel.frameStep(1)
        }
        Shortcut {
            enabled: root.shortcutsEnabled; sequence: ","
            onActivated: viewModel.frameStep(-1)
        }
        Shortcut {
            enabled: root.shortcutsEnabled && !stereo.active; sequence: "Tab"
            onActivated: viewModel.toggleOsd()
        }
        // 3D J/K/L/Tab 不再用 QML Shortcut：Tab 会被焦点遍历先吃掉，且
        // Shortcut 的字母大小写等价 Shift；改由 main.cpp 的 StereoKeyFilter
        // 在 QML 之前按 event->key() 拦截（大小写无关）。
        Shortcut {
            // Ctrl+Q 退出（与右键菜单 Quit 一致；走标准退出路径）
            enabled: root.shortcutsEnabled; sequence: "Ctrl+Q"
            onActivated: Qt.quit()
        }
        Shortcut {
            enabled: root.shortcutsEnabled && !stereo.active; sequence: "n"
            onActivated: viewModel.playlistNext()
        }
        Shortcut {
            enabled: root.shortcutsEnabled && !stereo.active; sequence: "b"
            onActivated: viewModel.playlistPrev()
        }
        Shortcut {
            enabled: root.shortcutsEnabled; sequence: "["
            onActivated: viewModel.setSpeed(0.5)
        }
        Shortcut {
            enabled: root.shortcutsEnabled; sequence: "]"
            onActivated: viewModel.setSpeed(2.0)
        }
        Shortcut {
            enabled: root.shortcutsEnabled; sequence: "\\"
            onActivated: viewModel.setSpeed(1.0)
        }
        Shortcut {
            enabled: root.shortcutsEnabled; sequence: "Ctrl+O"
            onActivated: { fileDialog.mode = 0; fileDialog.open() }
        }
        Shortcut {
            enabled: root.shortcutsEnabled; sequence: "Ctrl+Shift+O"
            onActivated: { folderDialog.mode = 0; folderDialog.open() }
        }
        Shortcut {
            enabled: root.shortcutsEnabled; sequence: "Ctrl+L"
            onActivated: urlPopup.open()
        }
        Shortcut {
            enabled: root.shortcutsEnabled; sequence: "V"
            onActivated: viewModel.toggleSubtitles()
        }
        Shortcut {
            enabled: root.shortcutsEnabled; sequence: "P"
            onActivated: root.togglePlaylist()
        }
        Shortcut {
            enabled: root.shortcutsEnabled; sequence: "F"
            onActivated: viewModel.toggleFullscreen()
        }
    }

    // ── 文件夹对话框（同一实例服务"打开文件夹"与"追加文件夹到播放列表"：
    //     mode 0 = replace（打开，与拖入窗口同通路）；mode 1 = append（追加，
    //     目录作为条目留在列表、点击才由 mpv 展开）；目录路径都不扫描）──────
    FolderDialog {
        id: folderDialog
        property int mode: 0
        title: folderDialog.mode === 1 ? qsTr("Add folder to playlist")
                                       : qsTr("Open folder")
        onAccepted: viewModel.openFiles([folderDialog.selectedFolder.toString()],
                                        folderDialog.mode)
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

    // ── 拖放区 A（窗口级）= 完全重置播放列表：首条 replace 载入并播放，
    //    其余 append 排队（mode 0）。字幕文件仍按扩展名走 sub-add。
    //    拖放区 B（追加）= PlaylistPanel 的 DropArea（mode 1）——面板打开时
    //    它在 popup overlay 里、叠在本区之上，Qt 只把 drop 交给最上层接受者，
    //    所以拖到面板=追加、拖到面板外=重置。两个区的差别只在 mode。
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
    // 测试/调试入口：RPC 打开 3D 选择器（正常从右键菜单进入）。
    property int stereoPickerMode: 1
    function openStereoPicker() {
        stereoPicker.openForMode(stereoPickerMode)
    }


    // 3D OSD：presentation 内覆盖层（不是独立窗口）；Tab 只切显示。
    Loader {
        id: stereoOsdOverlay
        anchors.fill: parent
        z: 100
        active: !controlMode && stereo.active
        visible: active
        source: "StereoOsd.qml"
    }
}
