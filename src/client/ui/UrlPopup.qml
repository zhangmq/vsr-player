import QtQuick
import QtQuick.Controls
import "components"

/// 打开 URL（网络流播放）：TextField + 打开/取消。Enter 确认。
/// 参考 TracksPopup 样式：定高（标题/输入/按钮固定区，无布局重叠）、
/// 输入框完全自绘（无系统控件残留——光标/选中/字体与项目一致）、
/// modal 居中。
PopupBase {
    id: root
    width: 460
    height: 132   // 定高：padding 16×2 + 标题 14 + 输入 34 + 按钮 30 + 间距
    // 居中（无 anchorTarget——PopupBase 默认 x/y=0 靠左；modal 同 TracksPopup）
    modal: true
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0

    signal urlEntered(string url)

    Text {
        id: titleText
        anchors { left: parent.left; right: parent.right; top: parent.top }
        text: qsTr("Open URL"); color: "#e0e0e0"; font.pixelSize: 13
    }

    TextField {
        id: urlField
        anchors { left: parent.left; right: parent.right; top: titleText.bottom; topMargin: 10 }
        height: 34
        // 完全自绘（无系统主题残留）
        font.pixelSize: 13
        color: "#e0e0e0"
        placeholderTextColor: "#808080"
        selectionColor: "#33ffcc00"
        selectedTextColor: "#e0e0e0"
        leftPadding: 10; rightPadding: 10
        background: Rectangle { color: "#33ffffff"; radius: 4 }
        Keys.onReturnPressed: root.confirm()
        Keys.onEnterPressed: root.confirm()
        focus: true
    }

    Row {
        anchors { right: parent.right; bottom: parent.bottom }
        spacing: 8
        Rectangle {
            width: 64; height: 30; radius: 4
            color: okHover.containsMouse ? "#33ffffff" : "#33ffcc00"
            Text { anchors.centerIn: parent; text: qsTr("Open"); color: "#ffcc00"; font.pixelSize: 13 }
            MouseArea { id: okHover; anchors.fill: parent; hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.confirm() }
        }
        Rectangle {
            width: 64; height: 30; radius: 4
            color: cancelHover.containsMouse ? "#33ffffff" : "transparent"
            Text { anchors.centerIn: parent; text: qsTr("Cancel"); color: "#e0e0e0"; font.pixelSize: 13 }
            MouseArea { id: cancelHover; anchors.fill: parent; hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.close() }
        }
    }

    function confirm() {
        var u = urlField.text.trim()
        if (u.length > 0) {
            root.urlEntered(u)
            root.close()
        }
    }

    onOpened: { urlField.text = ""; urlField.forceActiveFocus() }
}
