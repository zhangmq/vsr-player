import QtQuick
import QtQuick.Controls
import "components"

/// 轨道选择 + 字幕控制（音轨/字幕轨/视频轨三 section，无轨自动隐藏）。
/// 数据源：viewModel.trackList（track-list 属性观察器）；选中态由
/// track-list 的 selected 字段直接提供，切换走 selectTrack。
PopupBase {
    id: root
    width: 300

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
            // 自包含（inline component 定义体内不可引用外层 root）：
            // 显示名 = title > lang > "Track N"
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

    Column {
        spacing: 12
        anchors { left: parent.left; right: parent.right }

        // ── 音轨 ─────────────────────────────────────────────────
        Text { text: qsTr("Audio"); color: "#b0b0b0"; font.pixelSize: 13
            visible: root.audioTracks.length > 0 }
        Repeater {
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
        // 绑定须内联（不可经 property 中转——悬空绑定）。
        TrackRow {
            visible: root.subTracks.length > 0
            track: ({selected: !root.subTracks.some(t => t.selected), id: -1,
                     title: qsTr("No subtitles"), lang: ""})
            idx: -1; type: "sub"
        }
        Repeater {
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
            model: root.videoTracks
            visible: root.videoTracks.length > 0
            delegate: TrackRow { track: modelData; idx: index; type: "video" }
        }
    }
}
