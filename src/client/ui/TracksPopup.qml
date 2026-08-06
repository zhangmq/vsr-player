import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

/// 轨道选择 + 字幕控制（字幕/音频/视频三页签，标准 TabBar + StackLayout 分页）。
/// 数据源：viewModel.trackList（track-list 属性观察器）+ viewModel.subtitleFiles
///（目录扫描，优先级：精确同名 > 语言后缀 > 其余）。modal 居中、尺寸
/// 随窗口自适应（宽 55% / 高 80%，下限 480×420 防局促，窗口过小钳制
/// 防溢出）。内容区高度固定锚定（定高 − 页签），整区不滚动；滚动只在
/// 各页列表区（字幕页控制行/加载按钮固定，2026-08-06）。
PopupBase {
    id: root
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
            ToolTip.text: qsTr("Load subtitle"); ToolTip.delay: 600
            ToolTip.visible: sfHover.containsMouse }
    }

    // ── 页签（固定）──────────────────────────────────────────
    // 点击经 TabButton onClicked 显式驱动 root.currentTab（不依赖 TabBar
    // 内部联动）；onCurrentIndexChanged 同步键盘导航。currentIndex 保持
    // 字面量（勿改绑定——绑定会拒绝内部写入，高亮/联动失效）。
    TabBar {
        id: tabBar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        currentIndex: 0
        onCurrentIndexChanged: root.currentTab = tabBar.currentIndex
        TabButton { text: qsTr("Subtitles"); onClicked: root.currentTab = 0 }
        TabButton { text: qsTr("Audio"); onClicked: root.currentTab = 1 }
        TabButton { text: qsTr("Video"); onClicked: root.currentTab = 2 }
    }

    // ── 三个独立内容区（StackLayout 分页：一次只显示一页）─────────
    // 高度固定锚定（TabBar 下 → 弹窗底），整区不滚动；滚动只在列表区。
    // 注意：Repeater 的 delegate 插入为 Repeater 的父项的子项（兄弟关系，
    // qquickrepeater.cpp setParentItem(parentItem())），Repeater 上设
    // visible 无效——页可见性由 StackLayout 统一管理，delegate 宽度须
    // 显式绑定（width: parent.width）。
    StackLayout {
        anchors {
            left: parent.left; right: parent.right
            top: tabBar.bottom; topMargin: 8
            bottom: parent.bottom
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
        Flickable {
            anchors.fill: parent
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
            anchors.fill: parent
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
