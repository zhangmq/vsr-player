import QtQuick
import QtQuick.Controls
import "components"

/// SDR→HDR10 上变换（VFX TrueHDR）的**参数**弹框——由 QualityPopup 的
/// "Advanced…" 打开（modal 居中）。
///
/// 参数全部来自 C++（viewModel.upHdr*），区间/默认值来自 viewModel.upHdrLimits
///（单一来源 `src/client/TrueHdrParams.h` = SDK 参数文档串）。SDK **没有**预设概念，
/// 所以每个参数的 Reset = 回 SDK 默认值；Luminance 例外：Reset = 回当前面板峰值
///（它默认就是"自动跟面板"）。
///
/// ⚠ **UI 上不写说明文案**（用户 2026-10-18）：项目其它选项也都没有解释文字——
/// 只留控件标签/标题；参数含义、默认值这类信息留在代码注释与本文件里。
///
/// 交互约束：滑块实时提交但节流（120 ms），滤镜侧走 SetU32 热更新、零引擎重建。
PopupBase {
    id: root
    padding: 16
    modal: true
    width: 420
    // modal 居中（显式，Popup 默认 x/y=0 靠左）
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0

    // ── 数据（main.qml 绑定 viewModel）─────────────────────────────
    property bool  upHdrOn: false          // 总开关（策略 opt-in）
    property bool  upHdrEffective: false   // 策略实际生效
    property bool  lumOverride: false
    property int   lum: 650                // 用户值（覆盖打开时生效）
    property int   lumAuto: 400            // 面板自动峰值（只读显示）
    property int   saturation: 100
    property int   contrast: 100
    property int   middleGray: 50
    property bool  debanding: true
    property var   limits: ({})

    signal lumOverrideToggled(bool on)
    signal lumMoved(int v)
    signal saturationMoved(int v)
    signal contrastMoved(int v)
    signal middleGrayMoved(int v)
    signal debandingToggled(bool on)

    readonly property int _satDef:    limits.satDef !== undefined ? limits.satDef : 100
    readonly property int _conDef:    limits.conDef !== undefined ? limits.conDef : 100
    readonly property int _mgDef:     limits.mgDef  !== undefined ? limits.mgDef  : 50

    Column {
        spacing: 10
        anchors { left: parent.left; right: parent.right }

        Text {
            text: qsTr("SDR → HDR10")
            color: "#e0e0e0"; font.pixelSize: 14
        }

        // ── Luminance：覆盖开关 + 滑块（覆盖关时滑块/Reset 禁用）─────────
        Row {
            spacing: 8
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Target peak (Luminance)")
                color: "#b0b0b0"; font.pixelSize: 13
            }
            SegToggle {
                value: root.lumOverride
                offLabel: qsTr("Auto")
                onLabel: qsTr("Manual")
                onToggled: function(v) { root.lumOverrideToggled(v) }
            }
        }
        ParamSliderRow {
            width: parent.width
            label: ""
            value: root.lumOverride ? root.lum : root.lumAuto
            from: root.limits.lumMin !== undefined ? root.limits.lumMin : 400
            to:   root.limits.lumMax !== undefined ? root.limits.lumMax : 2000
            valueSuffix: qsTr(" nit")
            enabled: root.lumOverride
            onMoved: function(v) { root.lumMoved(v) }
            onResetRequested: {
                // Luminance 的 Reset = 回**面板自动值**（不是 SDK 650）：
                // 这个参数默认就是"跟面板"，用户按 Reset 想要的是回到自动结果。
                root.lumMoved(root.lumAuto)
            }
        }

        ParamSliderRow {
            width: parent.width
            label: qsTr("Saturation")
            value: root.saturation
            from: root.limits.satMin !== undefined ? root.limits.satMin : 0
            to:   root.limits.satMax !== undefined ? root.limits.satMax : 200
            onMoved: function(v) { root.saturationMoved(v) }
            onResetRequested: root.saturationMoved(root._satDef)
        }
        ParamSliderRow {
            width: parent.width
            label: qsTr("Contrast")
            value: root.contrast
            from: root.limits.conMin !== undefined ? root.limits.conMin : 0
            to:   root.limits.conMax !== undefined ? root.limits.conMax : 200
            onMoved: function(v) { root.contrastMoved(v) }
            onResetRequested: root.contrastMoved(root._conDef)
        }
        ParamSliderRow {
            width: parent.width
            label: qsTr("Middle gray")
            value: root.middleGray
            from: root.limits.mgMin !== undefined ? root.limits.mgMin : 10
            to:   root.limits.mgMax !== undefined ? root.limits.mgMax : 100
            onMoved: function(v) { root.middleGrayMoved(v) }
            onResetRequested: root.middleGrayMoved(root._mgDef)
        }

        // ── 抗色带（SDK DebandingOff 的反相；默认开）─────────────────
        Row {
            spacing: 8
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Debanding")
                color: "#b0b0b0"; font.pixelSize: 13
            }
            SegToggle {
                value: root.debanding
                onToggled: function(v) { root.debandingToggled(v) }
            }
        }
    }
}
