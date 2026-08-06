import QtQuick
import QtQuick.Controls
import "components"

/// 轨道选择 + 字幕控制（字幕/音频/视频三页签，标准 TabBar）。数据源：
/// viewModel.trackList（track-list 属性观察器）+ viewModel.subtitleFiles
///（目录扫描，优先级：精确同名 > 语言后缀 > 其余）。modal 居中、定高
///（用户实测反馈：内容自适应高度导致切 tab 跳变；滚动只在列表区，
/// 控制行/按钮固定，2026-08-06）。
PopupBase {
    id: root
    width: 560
    // 定高（钳制防超窗）：模态面板固定高度，不再随内容自适应
    height: Math.min(520, (parent ? parent.height : 520) - 40)
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

    readonly property var audioTracks: root.trackList.filter(t => t.type === "audio")
    readonly property var subTracks: root.trackList.filter(t => t.type === "sub")
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

    /// 未加载的外部字幕文件行（点击 → viewModel.loadExternalSubtitle = sub-add select）
    component SubFileRow: Rectangle {
        property string name
        property string path
        width: parent ? parent.width : 0; height: 32; radius: 4
        color: sfHover.containsMouse ? "#33ffffff" : "transparent"
        Text {
            anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
            text: name   // SubFileRow 的 property（Text 是其子项，作用域可见）
            color: "#e0e0e0"; font.pixelSize: 13; elide: Text.ElideRight
            width: parent.width - 20
        }
        MouseArea { id: sfHover; anchors.fill: parent; hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: viewModel.loadExternalSubtitle(path)
            ToolTip.text: qsTr("Load subtitle"); ToolTip.delay: 600
            ToolTip.visible: sfHover.containsMouse }
    }

    // ── 页签（固定；切换归零列表滚动位置）──────────────────────
    TabBar {
        id: tabBar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        currentIndex: root.currentTab
        onCurrentIndexChanged: {
            root.currentTab = tabBar.currentIndex
            flick.contentY = 0   // 切换归零——否则列表残留在旧页滚动位置
        }
        TabButton { text: qsTr("Subtitles") }
        TabButton { text: qsTr("Audio") }
        TabButton { text: qsTr("Video") }
    }

    // ── 字幕 tab 控制行（固定，不滚）────────────────────────────
    Row {
        id: controlRow
        anchors { left: parent.left; right: parent.right; top: tabBar.bottom; topMargin: 8 }
        visible: root.currentTab === 0
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

    // ── 加载字幕文件按钮（固定，不滚）──────────────────────────
    Rectangle {
        id: loadSubBtn
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 32; radius: 4
        visible: root.currentTab === 0
        color: subFbHover.containsMouse ? "#33ffffff" : "transparent"
        Text { anchors.centerIn: parent
            text: qsTr("Load subtitle file…")
            color: "#e0e0e0"; font.pixelSize: 13 }
        MouseArea { id: subFbHover; anchors.fill: parent; hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.subFileDialogRequested() }
    }

    // ── 列表区（唯一滚动区；Repeater 必须显式宽度——delegate 的
    //     parent 是 Repeater 本身，implicitWidth=0 → 宽 0 不可见，
    //     "弹窗空白"根因 2026-08-06）──────────────────────────
    Flickable {
        id: flick
        anchors {
            left: parent.left; right: parent.right
            top: root.currentTab === 0 ? controlRow.bottom : tabBar.bottom
            topMargin: 8
            bottom: root.currentTab === 0 ? loadSubBtn.top : parent.bottom
            bottomMargin: root.currentTab === 0 ? 8 : 0
        }
        contentHeight: contentCol.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: contentCol
            width: flick.width
            spacing: 12

            // ── 字幕页签：无字幕 + 已加载轨 + 外部字幕 ──────────
            // "无字幕"（关闭字幕轨，sid=no）。track 为 JS 对象字面量；
            // 对象字面量的字段是普通 JS 值而非 QML 绑定——整字面量须放
            // 在绑定表达式内整体重估（内联正确；拆 property 中转会冻结）
            TrackRow {
                visible: root.currentTab === 0
                track: ({selected: !root.subTracks.some(t => t.selected), id: -1,
                         title: qsTr("No subtitles"), lang: ""})
                idx: -1; type: "sub"
            }
            Repeater {
                width: parent.width
                visible: root.currentTab === 0
                model: root.subTracks
                delegate: TrackRow { track: modelData; idx: index; type: "sub" }
            }
            Text {
                text: qsTr("External"); color: "#b0b0b0"; font.pixelSize: 13
                visible: root.currentTab === 0 && viewModel.subtitleFiles.length > 0
            }
            Repeater {
                width: parent.width
                visible: root.currentTab === 0
                model: viewModel.subtitleFiles
                delegate: SubFileRow { name: modelData.name; path: modelData.path }
            }

            // ── 音频页签 ─────────────────────────────────────────
            Repeater {
                width: parent.width
                visible: root.currentTab === 1
                model: root.audioTracks
                delegate: TrackRow { track: modelData; idx: index; type: "audio" }
            }
            Text { text: qsTr("No audio tracks"); color: "#808080"; font.pixelSize: 13
                visible: root.currentTab === 1 && root.audioTracks.length === 0 }

            // ── 视频页签 ─────────────────────────────────────────
            Repeater {
                width: parent.width
                visible: root.currentTab === 2
                model: root.videoTracks
                delegate: TrackRow { track: modelData; idx: index; type: "video" }
            }
            Text { text: qsTr("No video tracks"); color: "#808080"; font.pixelSize: 13
                visible: root.currentTab === 2 && root.videoTracks.length === 0 }
        }
    }
}
