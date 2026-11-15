import QtQuick

/// 进度条悬停预览：缩略图 + "时间 · 章节名"。
///
/// 位置：悬浮在进度条上方、跟随指针水平移动、窗口左右边缘夹取。
/// 没有缩略图时（网络流/纯音频/首帧未到）退化成只显示时间的小气泡——
/// 用户要求"始终能看到指针在进度条上位置的时间"（2026-11-15）。
///
/// 数据全部来自 PlayerViewModel（缩略图经 image://vthumb/<serial> 提供，
/// 每次出新图 serial +1 → 换 URL，绕开 Image 的 URL 缓存）。
Item {
    id: root

    /// 指针在父坐标系里的 x（BottomBar 传 progress.x + hoverX）
    property real cursorX: 0
    property int hoverMs: 0

    readonly property bool hasImage: viewModel.previewReady && viewModel.previewSerial > 0
    readonly property int panelWidth: viewModel.previewThumbWidth
    readonly property real imgAspect: {
        var s = viewModel.previewImageSize
        if (!s || s.width <= 0 || s.height <= 0)
            return 9 / 16
        return s.height / s.width
    }
    readonly property real imgW: panelWidth
    readonly property real imgH: Math.round(panelWidth * imgAspect)
    readonly property string timeText: viewModel.formatTime(root.hoverMs)
    readonly property string chapter: viewModel.chapterTitleAt(root.hoverMs)
    readonly property string labelText: chapter.length > 0
        ? timeText + " · " + chapter : timeText

    readonly property int pad: 3

    implicitWidth: hasImage
        ? imgW + 2 * pad
        : Math.max(40, bubbleLabel.implicitWidth + 16)
    implicitHeight: hasImage
        ? (pad + imgH + 4 + label.implicitHeight + pad)
        : 22
    x: Math.min(Math.max(cursorX - width / 2, 0),
                parent ? parent.width - width : 0)

    // ── 缩略图面板 ──────────────────────────────────────────────
    Rectangle {
        visible: root.hasImage
        anchors.fill: parent
        color: "#d9111111"; radius: 6
        border { width: 1; color: "#22ffffff" }

        Image {
            id: thumb
            x: root.pad; y: root.pad
            width: root.imgW; height: root.imgH
            source: root.hasImage ? "image://vthumb/" + viewModel.previewSerial : ""
            cache: false
            asynchronous: false
            smooth: true
            fillMode: Image.PreserveAspectFit
        }

        Text {
            id: label
            x: root.pad
            y: thumb.y + thumb.height + 4
            width: root.imgW
            text: root.labelText
            color: "#e0e0e0"
            font.pixelSize: 13
            elide: Text.ElideRight
        }
    }

    // ── 只有时间的小气泡（无缩略图时）────────────────────────────
    Rectangle {
        visible: !root.hasImage
        anchors.fill: parent
        color: "#d9111111"; radius: 4
        border { width: 1; color: "#22ffffff" }

        Text {
            id: bubbleLabel
            anchors.centerIn: parent
            text: root.timeText
            color: "#e0e0e0"
            font.pixelSize: 13
        }
    }
}
