import QtQuick
import QtQuick.Controls

Item {
    id: root
    property real duration: 0
    property real currentTime: 0
    property bool overlaysVisible: true
    /// 供宿主（BottomBar）组成自动隐藏保持条件：进度条 hover/拖动
    /// 时 UI 不隐藏（拖动中鼠标在滑块上，不触发任何热区）。
    property alias hovered: psHover.hovered
    property alias pressed: progressSlider.pressed
    /// 指针在进度条上的位置（像素，进度条坐标）与换算出的比例/时间。
    /// SeekPreview 用它定位面板；hoverMs 同时驱动缩略图请求。
    property alias hoverX: progressSlider.hoverX
    property real hoverRatio: 0
    property int hoverMs: 0
    /// 测试钩子：RPC 注入的假悬停（Wayland 不能程序化移动指针）。<0 = 关。
    property int debugHoverMs: -1
    onDebugHoverMsChanged: {
        if (debugHoverMs < 0) return
        var r = root.duration > 0 ? debugHoverMs / root.duration : 0
        root.hoverRatio = r
        root.hoverMs = debugHoverMs
        progressSlider.hoverX = Math.round(r * progressSlider.availableWidth)
    }
    signal seeked(real ms)

    implicitHeight: 14

    Slider {
        id: progressSlider
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: psHover.hovered || pressed ? 8 : 6
        // ⚠ 四边 padding 必须自己钉成 0：Qt Quick Controls 的 Slider 默认 padding
        // 由**当前 style** 决定（Basic 是 6，org.kde.desktop 是 0/另一套），而我们的
        // handle/background 是按"整块都是轨道"画的 → 用 style 的 padding 会让旋钮整体
        // 偏移、两端到不了头（2026-11-28：钉死 Basic 后进度/音量滑块错位）。
        // 几何只依赖控件自身尺寸，换 style 不再影响。
        padding: 0

        opacity: root.overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
        from: 0; to: root.duration
        live: false
        Behavior on height { NumberAnimation { duration: 150 } }

        background: Rectangle {
            color: "#44ffffff"
            Rectangle {
                width: progressSlider.visualPosition * parent.width
                height: parent.height; color: "#e0e0e0"
            }
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
            onPointChanged: {
                if (!hovered) return
                var w = progressSlider.availableWidth
                if (w <= 0 || root.duration <= 0) return
                var x = Math.min(Math.max(point.position.x, 0), w)
                progressSlider.hoverX = x
                root.hoverRatio = x / w
                root.hoverMs = Math.round(root.hoverRatio * root.duration)
                // 节流/合并由 PreviewThumbnailer 负责：这里只报"最新目标"。
                viewModel.requestPreview(root.hoverMs)
            }
        }

        onPressedChanged: {
            if (!pressed)
                root.seeked(value)
        }
    }

    onHoveredChanged: {
        if (!hovered) {
            root.hoverRatio = 0
            viewModel.cancelPreview()
        }
    }

    onCurrentTimeChanged: {
        if (!progressSlider.pressed)
            progressSlider.value = root.currentTime
    }
}
