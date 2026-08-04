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
    signal seeked(real ms)

    implicitHeight: 14

    Slider {
        id: progressSlider
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: psHover.hovered || pressed ? 8 : 6
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
            onPointChanged: { if (hovered) progressSlider.hoverX = Math.min(Math.max(point.position.x, 0), progressSlider.availableWidth) }
        }

        onPressedChanged: {
            if (!pressed)
                root.seeked(value)
        }
    }

    onCurrentTimeChanged: {
        if (!progressSlider.pressed)
            progressSlider.value = root.currentTime
    }
}
