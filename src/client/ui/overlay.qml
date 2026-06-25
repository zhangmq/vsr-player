import QtQuick
import QtQuick.Controls

Item {
    id: root
    anchors.fill: parent

    // ── Font ─────────────────────────────────────────────────────────

    FontLoader {
        id: materialIcons
        source: "file:///usr/share/fonts/TTF/MaterialIcons-Regular.ttf"
    }
    readonly property string iconFont: materialIcons.name

    // ── IconButton helper component (inline, file-scoped) ──────────────
    component IconButton: Item {
        property string codepoint; property real size: 22
        property string tooltip: ""; property bool highlighted: false
        property string label: ""    // if set, shows text instead of icon
        signal clicked()
        implicitWidth: label ? label.length * 12 + 20 : size + 16
        implicitHeight: size + 16

        Rectangle {
            id: ibBg; anchors.fill: parent; radius: 4
            color: ibHover.hovered || highlighted ? "#33ffffff" : "transparent"
            Behavior on color { ColorAnimation { duration: 150 } }
        }
        Text {
            anchors.centerIn: parent
            font.family: label ? root.font.family : iconFont
            font.pixelSize: label ? size - 7 : size
            text: label ? label : codepoint
            color: ibHover.hovered || highlighted ? "#ffffff" : "#c8c8c8"
            Behavior on color { ColorAnimation { duration: 150 } }
            renderType: Text.NativeRendering
        }
        MouseArea {
            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
            hoverEnabled: true; onClicked: parent.clicked()
        }
        HoverHandler { id: ibHover }
        ToolTip {
            visible: ibHover.hovered && tooltip !== ""; text: tooltip
            delay: 600; font.pixelSize: 11
            background: Rectangle { color: "#d9111111"; radius: 4; border { width: 1; color: "#22ffffff" } }
            contentItem: Text { text: tooltip; color: "#e0e0e0"; font.pixelSize: 11 }
        }
    }

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
            if (!popupOpen && !barHover.hovered)
                viewModel.overlaysVisible = false
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // Top Bar
    // ══════════════════════════════════════════════════════════════════

    Rectangle {
        id: topBar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 48
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#cc000000" }
            GradientStop { position: 1.0; color: "transparent" }
        }
        opacity: viewModel.overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

        Text {
            anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            font.pixelSize: 14; elide: Text.ElideRight
            color: "#e0e0e0"
            text: {
                if (viewModel.videoInfo) return viewModel.videoInfo
                return "VSR Player"
            }
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // Center Play Button
    // ══════════════════════════════════════════════════════════════════

    Rectangle {
        id: centerPlayBtn
        width: 72; height: 72; radius: 36
        x: (parent.width - width) / 2; y: (parent.height - height) / 2
        color: cpHover.hovered ? (cpMouse.pressed ? "#55000000" : "#44000000") : "#33000000"
        opacity: (!viewModel.playing && viewModel.overlaysVisible) ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 200 } }

        Text {
            anchors.centerIn: parent
            font.family: iconFont; font.pixelSize: 36
            text: ""; color: "#ffffff"
            renderType: Text.NativeRendering
        }
        MouseArea {
            id: cpMouse; anchors.fill: parent
            cursorShape: Qt.PointingHandCursor; hoverEnabled: true
            onClicked: viewModel.togglePlayPause()
        }
        HoverHandler { id: cpHover }
    }

    // ══════════════════════════════════════════════════════════════════
    // Progress Slider
    // ══════════════════════════════════════════════════════════════════

    Slider {
        id: progressSlider
        anchors { left: parent.left; right: parent.right; bottom: bottomBar.top }
        height: psHover.hovered || pressed ? 6 : 4
        opacity: viewModel.overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
        from: 0; to: viewModel.duration
        live: false
        Behavior on height { NumberAnimation { duration: 150 } }

        background: Rectangle {
            color: "#44ffffff"
            // Played portion
            Rectangle {
                width: progressSlider.visualPosition * parent.width
                height: parent.height; color: "#e0e0e0"
            }
            // Hover preview — shows where click will land
            Rectangle {
                visible: psHover.hovered && !progressSlider.pressed
                width: 2; height: parent.height
                x: progressSlider.hoverX; color: "#aaffffff"
            }
        }

        handle: Rectangle {
            implicitWidth: 14; implicitHeight: 14; radius: 7; color: "#ffffff"
            visible: psHover.hovered || progressSlider.pressed
            x: progressSlider.leftPadding + progressSlider.visualPosition * (progressSlider.availableWidth - width)
            y: progressSlider.topPadding + progressSlider.availableHeight / 2 - height / 2
        }

        HoverHandler { id: psHover }
        property real hoverX: 0
        HoverHandler {
            onPointChanged: { if (hovered) progressSlider.hoverX = Math.min(Math.max(point.position.x, 0), progressSlider.availableWidth) }
        }

        // Seek on release — use onPressedChanged (synchronous) instead of
        // onMoved (queued signal). onMoved fires after the event loop
        // processes pending model updates, which can overwrite value
        // before the handler reads it. onPressedChanged fires during
        // the mouse-release event, before any queued signals.
        onPressedChanged: {
            if (!pressed)
                viewModel.seekAbsolute(value)
        }
    }

    // Sync slider value from model — but NOT while user is dragging/clicking.
    // Without this guard, the value binding races with user interaction:
    // model.currentTime updates can overwrite the slider before onMoved fires,
    // causing "seek to clicked position" to silently fail.
    Connections {
        target: viewModel
        function onCurrentTimeChanged() {
            if (!progressSlider.pressed)
                progressSlider.value = viewModel.currentTime
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // Bottom Bar
    // ══════════════════════════════════════════════════════════════════

    Rectangle {
        id: bottomBar
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 48
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 1.0; color: "#cc000000" }
        }
        opacity: viewModel.overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

        HoverHandler { id: barHover }
        // ── Left group: transport controls + time ──
        Row {
            anchors { left: parent.left; verticalCenter: parent.verticalCenter; leftMargin: 12 }
            spacing: 4

            IconButton { codepoint: ""; size: 22; tooltip: "上一个 (B)"
                onClicked: { var f = playlist.previous(); if (f) viewModel.loadFile(f) } }
            IconButton { codepoint: viewModel.playing ? "" : ""; size: 22
                tooltip: viewModel.playing ? "暂停 (Space)" : "播放 (Space)"
                onClicked: viewModel.togglePlayPause() }
            IconButton { codepoint: ""; size: 22; tooltip: "下一个 (N)"
                onClicked: { var f = playlist.next(); if (f) viewModel.loadFile(f) } }
            IconButton { codepoint: ""; size: 22; tooltip: "停止"
                onClicked: viewModel.stop() }

            Rectangle { width: 1; height: 20; color: "#0fffffff"; anchors.verticalCenter: parent.verticalCenter }

            Text {
                function fmt(ms) {
                    if (ms <= 0) return "0:00"
                    var s = Math.floor(ms/1000), m = Math.floor(s/60)
                    return m + ":" + (s%60 < 10 ? "0" : "") + s%60
                }
                text: fmt(viewModel.currentTime) + " / " + fmt(viewModel.duration)
                color: "#e0e0e0"; font.pixelSize: 13
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        // ── Right group: settings toggles (end-aligned) ──
        Row {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 12 }
            spacing: 4

            IconButton { id: volBtn; codepoint: ""; size: 22; tooltip: "音量"
                highlighted: volumePopup.visible
                onClicked: volumePopup.visible ? volumePopup.close() : volumePopup.open() }
            IconButton { id: qualBtn; codepoint: ""; size: 22; tooltip: "画质"
                highlighted: qualityPopup.visible
                onClicked: qualityPopup.visible ? qualityPopup.close() : qualityPopup.open() }
            IconButton { id: spdBtn; label: "倍速"; size: 22; tooltip: "播放速度"
                highlighted: speedPopup.visible
                onClicked: speedPopup.visible ? speedPopup.close() : speedPopup.open() }
            IconButton { label: viewModel.hwDecoding ? "硬解" : "软解"; size: 22
                tooltip: viewModel.hwDecoding ? "点击切换软解" : "点击切换硬解"
                onClicked: viewModel.toggleHwaccel() }
            IconButton { codepoint: viewModel.fullscreen ? "" : ""; size: 22
                tooltip: viewModel.fullscreen ? "退出全屏" : "全屏"
                onClicked: viewModel.toggleFullscreen() }
            IconButton { id: playlistBtn; codepoint: ""; size: 22; tooltip: "播放列表 (P)"
                highlighted: playlistPanel.visible
                onClicked: root.togglePlaylist() }
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // Volume Popup
    // ══════════════════════════════════════════════════════════════════

    Popup {
        id: volumePopup
        x: { var p = volBtn.mapToItem(root, 0, 0); return Math.min(Math.max(p.x + volBtn.width/2 - width/2, 8), root.width - width - 8) }
        y: bottomBar.y - height - 8
        width: 48; padding: 12
        modal: true; closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle { color: "#d9111111"; radius: 8; border { width: 1; color: "#22ffffff" } }

        Column {
            spacing: 8; anchors.horizontalCenter: parent.horizontalCenter

            Slider {
                id: volSlider
                anchors.horizontalCenter: parent.horizontalCenter
                orientation: Qt.Vertical; height: 120
                from: 0; to: 100; live: true
                onMoved: viewModel.setVolume(value / 100.0)
                handle: Rectangle { implicitWidth: 14; implicitHeight: 14; radius: 7; color: "#ffffff"
                    x: parent.leftPadding + parent.availableWidth / 2 - width / 2
                    y: parent.topPadding + parent.visualPosition * (parent.availableHeight - height) }
                background: Rectangle { implicitWidth: 4; color: "#44ffffff"; radius: 2
                    anchors.horizontalCenter: parent.horizontalCenter
                    Rectangle { width: 4; radius: 2; color: "#e0e0e0"
                        anchors.bottom: parent.bottom
                        height: parent.height * volSlider.position } }
            }

            Connections {
                target: volumePopup
                function onOpened() { volSlider.value = viewModel.volume * 100 }
            }

            // Mute toggle
            IconButton {
                anchors.horizontalCenter: parent.horizontalCenter
                codepoint: viewModel.muted ? "" : ""; size: 20
                tooltip: viewModel.muted ? "取消静音" : "静音"
                onClicked: { viewModel.toggleMute(); volSlider.value = viewModel.volume * 100 }
            }
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // Quality Popup
    // ══════════════════════════════════════════════════════════════════

    // ── Model data (JS arrays, valid for Repeater model in Qt 6) ──
    readonly property var qualityScaleModel: [
        {label: "关闭", value: -1},
        {label: "自动", value: 0},
        {label: "2×", value: 2},
        {label: "3×", value: 3},
        {label: "4×", value: 4}
    ]
    readonly property var qualityLevelModel: [
        {label: "Low", value: 1},
        {label: "Medium", value: 2},
        {label: "High", value: 3},
        {label: "Ultra", value: 4}
    ]
    readonly property var qualityDenoiseModel: [
        {label: "关闭", value: -1},
        {label: "Low", value: 8},
        {label: "Medium", value: 9},
        {label: "High", value: 10},
        {label: "Ultra", value: 11}
    ]

    Popup {
        id: qualityPopup
        x: { var p = qualBtn.mapToItem(root, 0, 0)
             return Math.min(p.x + qualBtn.width/2 - width/2, root.width - width - 8) }
        y: bottomBar.y - height - 8
        width: 360; padding: 16
        modal: true; closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle { color: "#d9111111"; radius: 8; border { width: 1; color: "#22ffffff" } }

        Column {
            spacing: 12; anchors { left: parent.left; right: parent.right }

            // ── Scale ──
            Text { text: "Scale"; color: "#b0b0b0"; font.pixelSize: 13 }
            Row { spacing: 6; anchors { left: parent.left; right: parent.right }
                Repeater {
                    model: qualityScaleModel
                    delegate: Rectangle {
                        width: 56; height: 32; radius: 4
                        color: sHover.containsMouse ? "#33ffffff"
                             : (viewModel.scale === modelData.value ? "#33ffcc00" : "transparent")
                        Text { anchors.centerIn: parent
                            text: modelData.label
                            color: viewModel.scale === modelData.value ? "#ffcc00" : "#e0e0e0"
                            font.pixelSize: 13 }
                        MouseArea { id: sHover; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { viewModel.setScale(modelData.value); qualityPopup.close() } }
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: "#0fffffff" }

            // ── 超分质量 ──
            Text { text: "超分质量"; color: "#b0b0b0"; font.pixelSize: 13 }
            Row { spacing: 6; anchors { left: parent.left; right: parent.right }
                Repeater {
                    model: qualityLevelModel
                    delegate: Rectangle {
                        width: 70; height: 32; radius: 4
                        color: qHover.containsMouse ? "#33ffffff"
                             : (viewModel.quality === modelData.value ? "#33ffcc00" : "transparent")
                        Text { anchors.centerIn: parent
                            text: modelData.label
                            color: viewModel.quality === modelData.value ? "#ffcc00" : "#e0e0e0"
                            font.pixelSize: 13 }
                        MouseArea { id: qHover; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { viewModel.setQuality(modelData.value); qualityPopup.close() } }
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: "#0fffffff" }

            // ── 强制降噪 ──
            Text { text: "强制降噪（scale=1时生效）"; color: "#b0b0b0"; font.pixelSize: 13 }
            Row { spacing: 6; anchors { left: parent.left; right: parent.right }
                Repeater {
                    model: qualityDenoiseModel
                    delegate: Rectangle {
                        width: 56; height: 32; radius: 4
                        color: dHover.containsMouse ? "#33ffffff"
                             : (viewModel.denoiseQuality === modelData.value ? "#33ffcc00" : "transparent")
                        Text { anchors.centerIn: parent
                            text: modelData.label
                            color: viewModel.denoiseQuality === modelData.value ? "#ffcc00" : "#e0e0e0"
                            font.pixelSize: 13 }
                        MouseArea { id: dHover; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { viewModel.setDenoiseQuality(modelData.value); qualityPopup.close() } }
                    }
                }
            }
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // Speed Popup
    // ══════════════════════════════════════════════════════════════════

    Popup {
        id: speedPopup
        x: { var p = spdBtn.mapToItem(root, 0, 0)
             return Math.min(p.x + spdBtn.width/2 - width/2, root.width - width - 8) }
        y: bottomBar.y - height - 8
        width: 180; padding: 12
        modal: true; closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle { color: "#d9111111"; radius: 8; border { width: 1; color: "#22ffffff" } }

        Column { spacing: 4; anchors { left: parent.left; right: parent.right }
            Text { text: "速度"; color: "#b0b0b0"; font.pixelSize: 13 }
            Repeater {
                model: [0.5, 0.75, 1.0, 2.0]
                Rectangle { width: parent.width; height: 36; radius: 4
                    color: spm.containsMouse ? "#33ffffff" : "transparent"
                    Text { anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                        text: Number(modelData) + "倍"
                        color: Math.abs(viewModel.speed - modelData) < 0.01 ? "#ffcc00" : "#e0e0e0"; font.pixelSize: 14 }
                    MouseArea { id: spm; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: { viewModel.setSpeed(modelData); speedPopup.close() } }
                }
            }
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // Playlist Panel
    // ══════════════════════════════════════════════════════════════════

    Drawer {
        id: playlistPanel
        edge: Qt.RightEdge
        width: 320; height: parent.height; z: 10
        dragMargin: 0
        padding: 0
        background: Rectangle { color: "#d9000000" }

        // Header
        Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 48; color: "#22ffffff"
            Text { anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
                text: "播放列表"; color: "#e0e0e0"; font.pixelSize: 15; font.bold: true }
            Text { anchors { right: closeBtn.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
                text: playlist ? playlist.count + " files" : ""; color: "#b0b0b0"; font.pixelSize: 12 }

            Item { id: closeBtn; width: 34; height: 34
                anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                Rectangle { anchors.fill: parent; radius: 4
                    color: clHover.hovered ? "#33ffffff" : "transparent"
                    Behavior on color { ColorAnimation { duration: 150 } } }
                Text { anchors.centerIn: parent; font.family: iconFont; font.pixelSize: 18
                    text: ""; color: clHover.hovered ? "#ffffff" : "#c8c8c8"
                    Behavior on color { ColorAnimation { duration: 150 } }
                    renderType: Text.NativeRendering }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; hoverEnabled: true
                    onClicked: playlistPanel.close() }
                HoverHandler { id: clHover }
            }
        }

        ListView {
            id: playlistView
            anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 48
                      bottom: parent.bottom }
            model: playlist ? playlist.files : []
            cacheBuffer: height * 2
            clip: true

            delegate: Rectangle {
                id: plDelegate
                width: ListView.view.width; height: 42; clip: true
                color: plHover.hovered ? "#22ffffff"
                     : (index === playlist.currentIndex ? "#11ffffff" : "transparent")

                Row {
                    anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                    spacing: 8

                    Text {
                        text: index + 1 + "."
                        width: 28; horizontalAlignment: Text.AlignRight
                        color: index === playlist.currentIndex ? "#e0e0e0" : "#b0b0b0"
                        font.pixelSize: 13
                        renderType: Text.NativeRendering
                    }
                    Text {
                        text: playlist && index < playlist.displayNames.length
                              ? playlist.displayNames[index] : ""
                        width: 250
                        color: index === playlist.currentIndex ? "#ffffff" : "#b0b0b0"
                        font.pixelSize: 13
                        renderType: Text.NativeRendering
                    }
                }

                MouseArea {
                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor; hoverEnabled: true
                    onClicked: { playlist.setCurrentFile(modelData); viewModel.loadFile(modelData) }
                }

                HoverHandler { id: plHover }

                ToolTip {
                    z: 11
                    visible: plHover.hovered
                    text: modelData
                    delay: 600
                    background: Rectangle {
                        color: "#d9111111"; radius: 4
                        border { width: 1; color: "#22ffffff" }
                    }
                    contentItem: Text {
                        text: modelData
                        color: "#e0e0e0"
                        font.pixelSize: 11
                    }
                }
            }
        }
    }

    // Dismiss playlist (click outside)
    MouseArea { anchors { left: parent.left; right: parent.right; top: parent.top; bottom: bottomBar.top }
        visible: playlistPanel.visible; z: 5; onClicked: root.togglePlaylist() }

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
