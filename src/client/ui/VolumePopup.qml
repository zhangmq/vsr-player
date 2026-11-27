import QtQuick
import QtQuick.Controls
import "components"

PopupBase {
    id: root
    property real volume: 0.0
    property bool muted: false
    signal volAdjusted(real v)
    signal muteToggled()

    width: 48; padding: 12
    onOpened: { volSlider.value = root.volume * 100 }
    // 滑杆只显示 mpv 观察上来的 volume（唯一事实源）。静音 = 音量 0：
    // 滑杆归零；取消静音由 toggleMute 恢复到静音前的值，滑杆随之回位。
    // 拖动中不同步（否则会与用户的手互抢）；点静音那一下也不能再用旧值
    // 覆盖——之前那样写就是"点静音滑杆不动、再点一次掉到 0"的根因。
    onVolumeChanged: {
        if (!volSlider.pressed)
            volSlider.value = root.volume * 100
    }
    // 拖动过程中不落盘（每次移动写配置会让 QSettings 重写整个文件 → 卡顿）；
    // 松手/关闭弹窗时才保存。
    onClosed: viewModel.persistVolume()

    Column {
        spacing: 8; anchors.horizontalCenter: parent.horizontalCenter

        Slider {
            id: volSlider
            anchors.horizontalCenter: parent.horizontalCenter
            orientation: Qt.Vertical; height: 120
            from: 0; to: 100; live: true
            onMoved: root.volAdjusted(value / 100.0)
            onPressedChanged: if (!pressed) viewModel.persistVolume()
            handle: Rectangle { implicitWidth: 14; implicitHeight: 14; radius: 7; color: "#ffffff"
                x: parent.leftPadding + parent.availableWidth / 2 - width / 2
                y: parent.topPadding + parent.visualPosition * (parent.availableHeight - height) }
            background: Rectangle { implicitWidth: 4; color: "#44ffffff"; radius: 2
                anchors.horizontalCenter: parent.horizontalCenter
                Rectangle { width: 4; radius: 2; color: "#e0e0e0"
                    anchors.bottom: parent.bottom
                    height: parent.height * volSlider.position } }
        }

        IconButton {
            anchors.horizontalCenter: parent.horizontalCenter
            codepoint: root.muted ? "󰖁" : "󰕾"; size: 20
            tooltip: root.muted ? qsTr("Unmute") : qsTr("Mute")
            onClicked: root.muteToggled()
        }
    }
}
