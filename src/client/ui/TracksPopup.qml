import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import "components"

/// 轨道选择 + 字幕控制（字幕/音频/视频三页签，标准 TabBar + StackLayout 分页）。
/// 数据源：viewModel.trackList（track-list 属性观察器）+ viewModel.subtitleFiles
///（目录扫描，优先级：精确同名 > 语言后缀 > 其余）。modal 居中、尺寸
/// 随窗口自适应（宽 55% / 高 80%，下限 480×420 防局促，窗口过小钳制
/// 防溢出）。内容区高度固定锚定（定高 − 页签），整区不滚动；滚动只在
/// 各页列表区（字幕页控制行/加载按钮固定，2026-08-06）。
PopupBase {
    id: root
    padding: 0   // 移除最外层边距（PopupBase 默认 16——页签/内容贴边，实测效果）
    // 自适应 + 最小尺寸（用户实测反馈，2026-08-06）
    width: {
        var a = parent ? parent.width : 560
        return Math.min(Math.max(a * 0.55, 480), a - 40)
    }
    height: {
        var a = parent ? parent.height : 520
        return Math.min(Math.max(a * 0.8, 420), a - 40)
    }
    modal: true
    // modal 不自动居中（Popup 默认 x/y=0 靠左）——显式居中（parent=overlay）
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0

    property var trackList: []
    property bool subVisible: true
    property real subDelay: 0.0
    property int currentTab: 0   // 0=字幕 1=音频 2=视频

    signal trackSelected(string type, var id)
    signal subVisibilityToggled()
    signal subDelayAdjusted(real delta)
    signal subFileDialogRequested()   // "加载字幕文件…" → main.qml FileDialog

    /// 点击外部字幕文件：已加载 → 选择现有轨（不重复 sub-add）；未加载 →
    /// 加载（sub-add select）。external 轨的 filename = 完整路径。
    function onSubFileClicked(path) {
        for (var i = 0; i < root.trackList.length; i++) {
            var t = root.trackList[i]
            if (t.type === "sub" && t.external && t.filename === path) {
                root.trackSelected("sub", t.id)
                return
            }
        }
        viewModel.loadExternalSubtitle(path)
    }

    /// 页签块自绘形状：顶部两端圆角 8（= modal 背景圆角半径，用户确认
    /// margin=1 保留 + radius=8）。底部直角（连体）。选中顶部高亮条与
    /// hover 块避开圆角区（x=8 起，与弧端点相切）。
    /// ShapePath 的 parent 是 Shape——TabBlock 属性须经 shape.parent 引用
    ///（直接 parent.fill 是 Shape 不存在的属性 → undefined → 渲染白色）。
    /// PathArc 半径 0 = 直线（SVG 椭圆弧语义）——条件圆角无需 if 元素。
    component TabBlock: Item {
        property bool roundLeft: false
        property bool roundRight: false
        property color fill: "transparent"
        property bool showTopBar: false
        property bool showHover: false

        Shape {
            id: shape
            anchors.fill: parent
            ShapePath {
                // shape.parent（TabBlock）在组件初始化早期为 null——
                // 直接引用 width/height 求值 undefined → QML 警告
                //（Unable to assign [undefined] to double），防御写法
                fillColor: shape.parent ? shape.parent.fill : "transparent"
                strokeColor: "transparent"
                startX: 0
                startY: shape.parent && shape.parent.roundLeft ? 8 : 0
                PathArc { x: 8; y: 0
                    radiusX: shape.parent && shape.parent.roundLeft ? 8 : 0
                    radiusY: shape.parent && shape.parent.roundLeft ? 8 : 0 }
                PathLine {
                    x: (shape.parent ? shape.parent.width : 0) - (shape.parent && shape.parent.roundRight ? 8 : 0)
                    y: 0 }
                PathArc {
                    x: shape.parent ? shape.parent.width : 0; y: 8
                    radiusX: shape.parent && shape.parent.roundRight ? 8 : 0
                    radiusY: shape.parent && shape.parent.roundRight ? 8 : 0 }
                PathLine { x: shape.parent ? shape.parent.width : 0; y: shape.parent ? shape.parent.height : 0 }
                PathLine { x: 0; y: shape.parent ? shape.parent.height : 0 }
                PathLine { x: 0; y: shape.parent && shape.parent.roundLeft ? 8 : 0 }
            }
        }
        // 选中顶部高亮条（避开圆角区——与弧端点相切）
        Rectangle {
            x: parent.roundLeft ? 8 : 0
            y: 0
            width: parent.width - x - (parent.roundRight ? 8 : 0)
            height: 2; color: "#ffcc00"
            visible: parent.showTopBar
        }
        // hover 高亮（内缩 8px 浅色块——不顶到页签块边缘）
        Rectangle {
            anchors { left: parent.left; leftMargin: 8; right: parent.right; rightMargin: 8
                      top: parent.top; topMargin: 8; bottom: parent.bottom; bottomMargin: 8 }
            radius: 4; color: "#33ffffff"
            visible: parent.showHover
        }
    }

    readonly property var audioTracks: root.trackList.filter(t => t.type === "audio")
    // 内置字幕轨（排除 external——已加载的外部字幕只显示在右列外部区，2026-08-06）
    readonly property var subTracks: root.trackList.filter(t => t.type === "sub" && !t.external)
    readonly property var videoTracks: root.trackList.filter(t => t.type === "video")

    component TrackRow: Rectangle {
        property var track
        property int idx
        property string type
        width: parent ? parent.width : 0; height: 32; radius: 4
        color: trHover.containsMouse ? "#33ffffff" : (track.selected ? "#33ffcc00" : "transparent")
        Text {
            anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
            // 显示名自包含（仅用自身 track/idx 属性，不依赖外层作用域）：
            // title > lang > "Track N"
            text: {
                var name = (track.title && track.title.length > 0) ? track.title : track.lang
                return (name && name.length > 0) ? name : qsTr("Track %1").arg(idx + 1)
            }
            color: track.selected ? "#ffcc00" : "#e0e0e0"
            font.pixelSize: 13; elide: Text.ElideRight
            width: parent.width - 20
        }
        MouseArea { id: trHover; anchors.fill: parent; hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.trackSelected(type, track.id) }
    }

    /// 外部字幕文件行。点击：已加载（trackList 有 external 轨匹配）→ 选择
    /// 现有轨；未加载 → sub-add select。目录全部文件常显，不因加载移除。
    /// 黄色高亮 = 当前选中（与内置轨互斥：mpv sid 单值）。
    component SubFileRow: Rectangle {
        property string name
        property string path
        // 绑定体内引用 root.trackList 建立属性依赖（trackList 变化重估）
        property bool selected: {
            for (var i = 0; i < root.trackList.length; i++) {
                var t = root.trackList[i]
                if (t.type === "sub" && t.external && t.filename === path && t.selected)
                    return true
            }
            return false
        }
        width: parent ? parent.width : 0; height: 32; radius: 4
        color: sfHover.containsMouse ? "#33ffffff"
             : (selected ? "#33ffcc00" : "transparent")
        Text {
            anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
            text: name
            color: selected ? "#ffcc00" : "#e0e0e0"
            font.pixelSize: 13; elide: Text.ElideRight
            width: parent.width - 20
        }
        MouseArea { id: sfHover; anchors.fill: parent; hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.onSubFileClicked(path)
            // 断开系统配色：attached ToolTip 视觉 = 样式共享实例（系统
            // palette.toolTipBase/Text）且无法自绘——改显式实例（样式自带
            // parent 上方居中定位，同 IconButton 模式）
            ToolTip {
                visible: sfHover.containsMouse
                text: qsTr("Load subtitle"); delay: 600
                background: Rectangle { color: "#d9111111"; radius: 4
                    border { width: 1; color: "#22ffffff" } }
                contentItem: Text { text: qsTr("Load subtitle"); color: "#e0e0e0"; font.pixelSize: 13 }
            } }
    }

    // ── 页签（固定）──────────────────────────────────────────
    // 点击经 TabButton onClicked 显式驱动 root.currentTab（不依赖 TabBar
    // 内部联动）；onCurrentIndexChanged 同步键盘导航。currentIndex 保持
    // 字面量（勿改绑定——绑定会拒绝内部写入，高亮/联动失效）。
    // 完全断开系统配色：Fusion 默认背景全部由系统 palette 驱动（选中页签
    // 渐变顶部的"蓝色高亮" = 系统 button 蓝灰调 #292c30 lighter 的结果），
    // 故页签全自绘、色值写死。tab 形式保留：选中页签 = 面板色与内容区
    // 连体 + 顶部 2px 高亮条（#ffcc00，应用高亮色）+ 高亮文本；未选中 =
    // #262626 块 + #e0e0e0；悬停 #33ffffff。
    // 圆角处理：页签块自身顶部两端圆角 7（TabBlock，= background 圆角
    // 8 − 边框线 1px，与边框内缘弧线贴合）——任何状态（hover/黄条）
    // 都在圆角内，等效"被 modal 边框裁剪"。MultiEffect mask 方案已
    // 放弃（采样在 popup 场景不可靠，4 轮失败）。top/left/rightMargin
    // 1 = 边框线宽（页签块不压边框线）。
    TabBar {
        id: tabBar
        // 显式 height = TabButton 高度：否则 TabBar implicitHeight 由
        // background（transparent，implicit 0）+ ListView implicitHeight
        // 计算（远小于 48），TabButton 48 高渲染溢出 TabBar 底部——
        // 压住 tabs body 的 8px 间距（用户实测：间距消失、紧贴 body）
        // margins 1 = 边框线宽（页签块不压边框线）；页签块弧线圆心
        // 相应内移 (1,1) = 局部 (7,7)（见 TabBlock 注释）
        height: 48
        anchors { left: parent.left; leftMargin: 1; right: parent.right; rightMargin: 1
                  top: parent.top; topMargin: 1 }
        currentIndex: 0
        onCurrentIndexChanged: root.currentTab = tabBar.currentIndex
        spacing: 0
        background: Rectangle { color: "transparent" }
        TabButton {
            height: 48
            // implicitHeight 与显式高度一致：ListView delegate 定位以
            // implicitHeight 为基准对齐——不一致时产生 (48−implicitHeight)/2
            // 的居中偏移（实测 implicitHeight≈31 → 偏移 −8.5px，页签块
            // 顶出 modal 顶边 7.5px；加大高度就错位）
            implicitHeight: 48
            onClicked: root.currentTab = 0
            background: TabBlock {
                roundLeft: true
                fill: root.currentTab === 0 ? "transparent" : "#66262626"
                showTopBar: root.currentTab === 0
                showHover: tabH0.hovered && root.currentTab !== 0
            }
            contentItem: Text {
                text: qsTr("Subtitles"); font.pixelSize: 13
                color: root.currentTab === 0 ? "#ffcc00" : "#e0e0e0"
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            }
            HoverHandler { id: tabH0 }
        }
        TabButton {
            height: 48
            // implicitHeight 与显式高度一致：ListView delegate 定位以
            // implicitHeight 为基准对齐——不一致时产生 (48−implicitHeight)/2
            // 的居中偏移（实测 implicitHeight≈31 → 偏移 −8.5px，页签块
            // 顶出 modal 顶边 7.5px；加大高度就错位）
            implicitHeight: 48
            onClicked: root.currentTab = 1
            background: TabBlock {
                fill: root.currentTab === 1 ? "transparent" : "#66262626"
                showTopBar: root.currentTab === 1
                showHover: tabH1.hovered && root.currentTab !== 1
            }
            contentItem: Text {
                text: qsTr("Audio"); font.pixelSize: 13
                color: root.currentTab === 1 ? "#ffcc00" : "#e0e0e0"
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            }
            HoverHandler { id: tabH1 }
        }
        TabButton {
            height: 48
            // implicitHeight 与显式高度一致：ListView delegate 定位以
            // implicitHeight 为基准对齐——不一致时产生 (48−implicitHeight)/2
            // 的居中偏移（实测 implicitHeight≈31 → 偏移 −8.5px，页签块
            // 顶出 modal 顶边 7.5px；加大高度就错位）
            implicitHeight: 48
            onClicked: root.currentTab = 2
            background: TabBlock {
                roundRight: true
                fill: root.currentTab === 2 ? "transparent" : "#66262626"
                showTopBar: root.currentTab === 2
                showHover: tabH2.hovered && root.currentTab !== 2
            }
            contentItem: Text {
                text: qsTr("Video"); font.pixelSize: 13
                color: root.currentTab === 2 ? "#ffcc00" : "#e0e0e0"
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            }
            HoverHandler { id: tabH2 }
        }
    }

    // ── 三个独立内容区（StackLayout 分页：一次只显示一页）─────────
    // 高度固定锚定（TabBar 下 → 弹窗底），整区不滚动；滚动只在列表区。
    // 注意：Repeater 的 delegate 插入为 Repeater 的父项的子项（兄弟关系，
    // qquickrepeater.cpp setParentItem(parentItem())），Repeater 上设
    // visible 无效——页可见性由 StackLayout 统一管理，delegate 宽度须
    // 显式绑定（width: parent.width）。
    StackLayout {
        anchors {
            left: parent.left; leftMargin: 16; right: parent.right; rightMargin: 16
            top: tabBar.bottom; topMargin: 16
            bottom: parent.bottom; bottomMargin: 16
        }
        currentIndex: root.currentTab

        // ── 字幕页（控制行固定 + 列表滚动 + 加载按钮固定）────────
        Item {
            Row {
                id: controlRow
                anchors { left: parent.left; right: parent.right; top: parent.top }
                spacing: 6
                Rectangle {
                    width: 86; height: 32; radius: 4
                    color: visHover.containsMouse ? "#33ffffff"
                         : (root.subVisible ? "#33ffcc00" : "transparent")
                    Text { anchors.centerIn: parent
                        text: root.subVisible ? qsTr("Visible") : qsTr("Hidden")
                        color: root.subVisible ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13 }
                    MouseArea { id: visHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.subVisibilityToggled() }
                }
                Rectangle {
                    width: 44; height: 32; radius: 4
                    color: dMinusHover.containsMouse ? "#33ffffff" : "transparent"
                    Text { anchors.centerIn: parent; text: "−0.1s"; color: "#e0e0e0"; font.pixelSize: 13 }
                    MouseArea { id: dMinusHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.subDelayAdjusted(-0.1) }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("%1s").arg(root.subDelay.toFixed(1))
                    color: "#b0b0b0"; font.pixelSize: 13
                }
                Rectangle {
                    width: 44; height: 32; radius: 4
                    color: dPlusHover.containsMouse ? "#33ffffff" : "transparent"
                    Text { anchors.centerIn: parent; text: "+0.1s"; color: "#e0e0e0"; font.pixelSize: 13 }
                    MouseArea { id: dPlusHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.subDelayAdjusted(0.1) }
                }
            }

            Rectangle {
                id: loadSubBtn
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: 32; radius: 4
                color: subFbHover.containsMouse ? "#33ffffff" : "transparent"
                Text { anchors.centerIn: parent
                    text: qsTr("Load subtitle file…")
                    color: "#e0e0e0"; font.pixelSize: 13 }
                MouseArea { id: subFbHover; anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.subFileDialogRequested() }
            }

            // 列表区（两列：内置字幕轨 | 外部字幕文件，各自独立滚动）
            Flickable {
                id: subTrackFlick
                anchors {
                    left: parent.left
                    top: controlRow.bottom; topMargin: 8
                    bottom: loadSubBtn.top; bottomMargin: 8
                }
                width: parent.width * 0.45   // 外部列更宽：字幕文件名通常长于轨道名
                contentHeight: subTrackCol.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Column {
                    id: subTrackCol
                    width: parent.width
                    spacing: 12
                    Repeater {
                        model: root.subTracks
                        delegate: TrackRow { track: modelData; idx: index; type: "sub" }
                    }
                    Text {   // 无内置字幕轨时空状态提示（隐藏/显示由控制行按钮管理，无需"无字幕"项）
                        text: qsTr("No subtitles"); color: "#808080"; font.pixelSize: 13
                        visible: root.subTracks.length === 0
                    }
                }
            }

            // 两列分隔线
            Rectangle {
                id: colSep
                anchors { left: subTrackFlick.right; leftMargin: 6
                    top: subTrackFlick.top; bottom: subTrackFlick.bottom }
                width: 1; color: "#22ffffff"
            }

            Flickable {
                id: extSubFlick
                anchors {
                    left: colSep.right; leftMargin: 6
                    right: parent.right
                    top: subTrackFlick.top; bottom: subTrackFlick.bottom
                }
                contentHeight: extSubCol.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Column {
                    id: extSubCol
                    width: parent.width
                    spacing: 12
                    Text {
                        text: qsTr("External"); color: "#b0b0b0"; font.pixelSize: 13
                        visible: viewModel.subtitleFiles.length > 0
                    }
                    Repeater {
                        model: viewModel.subtitleFiles
                        delegate: SubFileRow { name: modelData.name; path: modelData.path }
                    }
                    Text {   // 无外部字幕文件时占位（列结构保持）
                        text: qsTr("No subtitle files"); color: "#808080"; font.pixelSize: 13
                        visible: viewModel.subtitleFiles.length === 0
                    }
                }
            }
        }

        // ── 音频页 ────────────────────────────────────────────
        // 尺寸由 StackLayout 管理（fillWidth/fillHeight 默认 true）——
        // 子项上设 anchors 会被布局管理警告（undefined behavior）
        Flickable {
            contentHeight: audioCol.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: audioCol
                width: parent.width
                spacing: 12
                Repeater {
                    model: root.audioTracks
                    delegate: TrackRow { track: modelData; idx: index; type: "audio" }
                }
                Text { text: qsTr("No audio tracks"); color: "#808080"; font.pixelSize: 13
                    visible: root.audioTracks.length === 0 }
            }
        }

        // ── 视频页 ────────────────────────────────────────────
        Flickable {
            contentHeight: videoCol.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: videoCol
                width: parent.width
                spacing: 12
                Repeater {
                    model: root.videoTracks
                    delegate: TrackRow { track: modelData; idx: index; type: "video" }
                }
                Text { text: qsTr("No video tracks"); color: "#808080"; font.pixelSize: 13
                    visible: root.videoTracks.length === 0 }
            }
        }
    }

}
