import QtQuick

/// 进度条悬停预览：缩略图 + 时间（底部）+ 章节名（顶部）。
///
/// 形态（用户 2026-11-15）：缩略图**无边框、无圆角、铺满整个组件**
/// （不再有外圈深色面板与内边距）；文字一律 overlay 在图片上、不额外占高度：
///   - **时间**：纯黑底（`#000000`）文字块，**横向居中、贴图片底边**；
///   - **章节名**：**半透明黑底**（`#cc000000`）、**占满图片宽度**、**贴图片顶部、
///     左对齐**（用户 2026-11-15 第三次调整：章节名不再和时间挤在同一行）；
///     没有章节时整条不显示。
/// 两处文本超出可用宽度都右侧省略（`elide: ElideRight`）。
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
    /// 面板/组件几何一律按 **Qt 逻辑像素**（用户 2026-11-15）；实际渲染宽由
    /// PlayerViewModel 按 "逻辑宽 × DPR" 算好，QML 这边的 Image 用逻辑宽显示。
    readonly property int panelWidth: viewModel.previewThumbLogicalWidth
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
    /// 取不到图时在气泡里补一句"无法获取"（用户 2026-11-15：失败也要给气泡，
    /// 最多写"无法获取"；面板不隐藏、尺寸不变）。章节名照常显示。
    readonly property string unavailableText: qsTr("Thumbnail unavailable")
    /// 取不到图时气泡补一句"无法获取缩略图"（用户 2026-11-15：失败也要给
    /// 气泡，最多写"无法获取缩略图"；面板不隐藏、尺寸随文字走）。
    readonly property string bubbleText: viewModel.previewUnavailable
        ? timeText + " · " + unavailableText : timeText
    /// 底部黑底文字块的内边距（横向居中、贴图片底边）
    readonly property int capPadX: 6
    readonly property int capPadY: 2

    implicitWidth: hasImage
        ? imgW
        : Math.max(40, bubbleLabel.implicitWidth + 16)
    implicitHeight: hasImage ? imgH : 22
    x: Math.min(Math.max(cursorX - width / 2, 0),
                parent ? parent.width - width : 0)

    // ── 缩略图：无边框、无圆角、铺满 ──────────────────────────────
    // crop 而非 fit：输出图宽高比就是按 imgW/imgH 算出来的（同一套取整），
    // 两者一致，crop 只吃掉可能的 1px 取整差 → 永远不会出现 letterbox 缝。
    Image {
        id: thumb
        visible: root.hasImage
        anchors.fill: parent
        source: root.hasImage ? "image://vthumb/" + viewModel.previewSerial : ""
        cache: false
        asynchronous: false
        smooth: true
        fillMode: Image.PreserveAspectCrop
        clip: true
    }

    // ── 章节名：半透明黑底、占满图片宽度、贴图片顶部、左对齐 ──────
    // 用户 2026-11-15：章节名**不再**和时间挤在同一行（时间留在底部）。
    // 没有章节时整条不显示（不占高度，也不留空条）。
    Rectangle {
        id: chapterBar
        visible: root.hasImage && root.chapter.length > 0
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
        }
        height: chapterLabel.implicitHeight + 2 * root.capPadY
        color: "#cc000000"     // 半透明黑（80% 不透明），压在画面上仍能看到内容

        Text {
            id: chapterLabel
            anchors.fill: parent
            anchors {
                leftMargin: root.capPadX
                rightMargin: root.capPadX
                topMargin: root.capPadY
                bottomMargin: root.capPadY
            }
            text: root.chapter
            color: "#e0e0e0"
            font.pixelSize: 13
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.NoWrap
            elide: Text.ElideRight
        }
    }

    // ── 时间：纯黑底、横向居中、overlay 在图片底部 ────────────────
    Rectangle {
        id: captionBar
        visible: root.hasImage
        color: "#000000"
        // 宽度按文字自然宽度取，但不超过图片宽（超过就交给 elide）
        width: Math.min(root.width, caption.implicitWidth + 2 * root.capPadX)
        height: caption.implicitHeight + 2 * root.capPadY
        x: Math.round((root.width - width) / 2)
        y: root.height - height

        Text {
            id: caption
            anchors.fill: parent
            anchors {
                leftMargin: root.capPadX
                rightMargin: root.capPadX
                topMargin: root.capPadY
                bottomMargin: root.capPadY
            }
            text: root.timeText
            color: "#e0e0e0"
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.NoWrap
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
            text: root.bubbleText
            color: "#e0e0e0"
            font.pixelSize: 13
        }
    }
}
