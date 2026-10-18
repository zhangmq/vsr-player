import QtQuick
import QtQuick.Controls

/// 单参数行：标签 + 当前值 + 滑块 + Reset 按钮（TrueHDR 高级弹框用）。
/// 拖动**实时提交但节流**（leading + trailing）——滤镜侧改参数走 `NvVFX_SetU32`
/// 热更新（实测 Load 后生效、零引擎重建，见 vf_truehdr.c engine_params_apply），
/// 所以不必等松手；节流只为限制 vf-command 频率（默认 120 ms 一次）。
/// 配色**照搬播放进度条**（ProgressSlider.qml：轨道 #44ffffff、已填充 #e0e0e0、
/// 滑块 #ffffff 14×14 圆角 7）——不引入项目里没有的视觉语言。
Item {
    id: root
    property string label: ""
    property int value: 0
    property int from: 0
    property int to: 100
    property string valueSuffix: ""
    property bool enabled: true         // false → 整行变灰、不可交互
    property int throttleMs: 120        // 提交节流间隔
    signal moved(int v)
    signal resetRequested()

    implicitHeight: 54
    opacity: enabled ? 1.0 : 0.4

    // ── 提交节流：leading edge 立即发 + 拖动中每 throttleMs 最多一次 + 尾值必发 ──
    property int _pending: 0
    property int _lastSent: -1
    property bool _syncing: false       // 程序回写 slider 时不触发提交
    /// 构造完成前**一律不提交**：Slider 的属性按声明顺序初始化，`from: 10/400`
    /// 一旦生效，内部 value 会从默认 0 被**夹到 from** 并触发 valueChanged ——
    /// 那一刻 `Component.onCompleted` 还没跑，`_syncing` 守卫来不及生效，于是把
    /// 最小值当成用户输入提交（实测：middle-gray 被写成 10、luminance 被写成 400
    /// 并落盘）。所以提交必须同时看 `_ready`。
    property bool _ready: false

    function submitThrottled(v) {
        root._pending = v
        if (!throttleTimer.running) {
            root._lastSent = v
            root.moved(v)               // 立即生效（用户马上看到结果）
            throttleTimer.start()
        }
    }

    Timer {
        id: throttleTimer
        interval: root.throttleMs
        repeat: false
        onTriggered: {
            if (root._pending !== root._lastSent) {
                root._lastSent = root._pending
                root.moved(root._pending)
            }
            if (root._pending !== root._lastSent)
                restart()               // 还在拖 → 继续按节流节奏提交
        }
    }

    Text {
        id: lab
        anchors { left: parent.left; top: parent.top }
        text: root.label
        color: "#b0b0b0"
        font.pixelSize: 13
    }
    IconButton {
        id: resetBtn
        anchors { right: parent.right; top: parent.top; topMargin: -8 }
        size: 14
        label: qsTr("Reset")
        enabled: root.enabled
        onClicked: root.resetRequested()
    }
    Text {
        anchors { right: resetBtn.left; rightMargin: 10; verticalCenter: resetBtn.verticalCenter }
        text: Math.round(slider.value) + root.valueSuffix
        color: "#808080"
        font.pixelSize: 12
    }
    Slider {
        id: slider
        anchors { left: parent.left; right: parent.right; top: lab.bottom; topMargin: 4 }
        from: root.from
        to: root.to
        stepSize: 1
        enabled: root.enabled
        // ⚠ 不用 `value: root.value` 绑定：用户拖动时控件内部写 value 会**打断绑定**，
        // 此后 VM 回写就同步不上。改为显式双向同步（下方 Connections）。
        // ⚠ 初始化赋值**不能**触发提交（见 _ready 注释：构造期 from/to 生效时
        // value 会被夹到 from 并触发一次 valueChanged）。onCompleted 里先把滑块
        // 定到真值，再放开提交。
        Component.onCompleted: {
            root._syncing = true
            value = root.value
            root._syncing = false
            root._ready = true
        }
        onValueChanged: if (root._ready && !root._syncing)
                            root.submitThrottled(Math.round(value))
        // 松手补发最后一个值（节流窗口里可能还没轮到它）
        onPressedChanged: if (!pressed && root._pending !== root._lastSent) {
            root._lastSent = root._pending
            root.moved(root._pending)
        }
        // 配色照搬 ProgressSlider（轨道/已填充/滑块三色 + 滑块 14×14 r7）
        background: Rectangle {
            x: slider.leftPadding
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            width: slider.availableWidth
            height: 6
            radius: 3
            color: "#44ffffff"
            Rectangle {
                width: slider.visualPosition * parent.width
                height: parent.height
                radius: 3
                color: "#e0e0e0"
            }
        }
        handle: Rectangle {
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            implicitWidth: 14
            implicitHeight: 14
            radius: 7
            color: "#ffffff"
        }
    }
    Connections {
        target: root
        // VM 侧真值变化（含夹取/Reset 回读）→ 同步；拖动中不打断用户
        function onValueChanged() {
            if (slider.pressed) return
            root._syncing = true
            slider.value = root.value
            root._syncing = false
        }
    }
}
