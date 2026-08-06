import QtQuick
import QtQuick.Controls
import "components"

/// 轨道选择 + 字幕控制（音轨/字幕轨/视频轨三 section，无轨自动隐藏）。
/// 数据源：viewModel.trackList（track-list 属性观察器）；选中态由
/// track-list 的 selected 字段直接提供，切换走 selectTrack。
/// modal 呈现（居中 + 遮罩）：多 section 面板锚定式 popup 易越界，
/// 居中模态更合适（用户实测反馈，2026-08-06）。
PopupBase {
    id: root
    width: 300
    // 内容为 Flickable（implicitHeight=0）——Popup 显式高度 = 内容 + padding
    height: flick.height + 32
    modal: true

    property var trackList: []
    property bool subVisible: true
    property real subDelay: 0.0

    signal trackSelected(string type, var id)
    signal subVisibilityToggled()
    signal subDelayAdjusted(real delta)

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

    // ── 内容滚动（多轨道文件限高）────────────────────────────────
    // 多字幕轨文件（36+ 轨）会使面板超高——Flickable 限高滚动（滚轮），
    // 高度钳制到窗口 80%（modal 弹窗溢出窗口会被裁剪，2026-08-06）。
    // 注意：Repeater 必须显式宽度——delegate 的 parent 是 Repeater 本身
    //（Repeater implicitWidth=0 → delegate width=0 不可见——实机复现
    // "弹窗空白"的根因，2026-08-06）。
    Flickable {
        id: flick
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: Math.min(contentCol.implicitHeight, (parent ? parent.height : 480) * 0.8)
        contentHeight: contentCol.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

    Column {
        id: contentCol
        width: flick.width
        spacing: 12

        // ── 音轨 ─────────────────────────────────────────────────
        Text { text: qsTr("Audio"); color: "#b0b0b0"; font.pixelSize: 13
            visible: root.audioTracks.length > 0 }
        Repeater {
            width: parent.width
            model: root.audioTracks
            visible: root.audioTracks.length > 0
            delegate: TrackRow { track: modelData; idx: index; type: "audio" }
        }
        Rectangle { width: parent.width; height: 1; color: "#0fffffff"
            visible: root.audioTracks.length > 0 }

        // ── 字幕 ─────────────────────────────────────────────────
        Text { text: qsTr("Subtitles"); color: "#b0b0b0"; font.pixelSize: 13
            visible: root.subTracks.length > 0 }
        Row { spacing: 6; visible: root.subTracks.length > 0
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
        // "无字幕"（关闭字幕轨，sid=no）。track 为 JS 对象字面量；
        // 对象字面量的字段是普通 JS 值而非 QML 绑定——整字面量须放在
        // 绑定表达式内整体重估（内联写法正确；拆到 property 中转会冻结）
        TrackRow {
            visible: root.subTracks.length > 0
            track: ({selected: !root.subTracks.some(t => t.selected), id: -1,
                     title: qsTr("No subtitles"), lang: ""})
            idx: -1; type: "sub"
        }
        Repeater {
            width: parent.width
            model: root.subTracks
            visible: root.subTracks.length > 0
            delegate: TrackRow { track: modelData; idx: index; type: "sub" }
        }
        Rectangle { width: parent.width; height: 1; color: "#0fffffff"
            visible: root.subTracks.length > 0 }

        // ── 视频轨 ───────────────────────────────────────────────
        Text { text: qsTr("Video"); color: "#b0b0b0"; font.pixelSize: 13
            visible: root.videoTracks.length > 0 }
        Repeater {
            width: parent.width
            model: root.videoTracks
            visible: root.videoTracks.length > 0
            delegate: TrackRow { track: modelData; idx: index; type: "video" }
        }
    }
    }   // Flickable
}
