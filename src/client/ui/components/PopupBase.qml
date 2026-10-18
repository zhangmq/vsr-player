import QtQuick
import QtQuick.Controls

/// 统一弹窗外观 + 按钮定位助手（组件化：定位逻辑从 overlay 收拢到此处）。
/// 子类设置 anchorTarget 后 open() 时自动水平居中于目标、显示在其上方，
/// 屏幕边缘自动钳制；上方空间不足时自动移到下方。
Popup {
    id: root

    property Item anchorTarget: null
    property real targetMarginY: 8
    property real screenMargin: 8

    padding: 16
    // 非 modal：无主窗口遮罩（mask），打开时主窗口保持可交互。
    // 点击外部关闭由 overlay 机制处理（CloseOnPressOutside 与 modal 无关）。
    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    /// **不加背景遮罩**（用户 2026-10-18 要求）：弹窗里的参数要能立刻看到画面
    /// 效果，半透明遮罩会干扰判断。`modal: true` 的子类（TracksPopup、TrueHDR
    /// 高级弹框）仍然保留"挡住外部点击"的语义，只是把默认的调暗层换成空 Item。
    Overlay.modal: Item {}

    background: Rectangle {
        color: "#d9111111"; radius: 8
        border { width: 1; color: "#22ffffff" }
    }

    onOpened: positionToTarget()
    onAnchorTargetChanged: if (opened) positionToTarget()

    function positionToTarget() {
        if (!anchorTarget || !parent) return
        var p = anchorTarget.mapToItem(parent, 0, 0)
        var nx = p.x + anchorTarget.width / 2 - root.width / 2
        nx = Math.min(Math.max(nx, screenMargin), parent.width - root.width - screenMargin)
        var ny = p.y - root.height - targetMarginY
        if (ny < screenMargin)
            ny = p.y + anchorTarget.height + targetMarginY
        root.x = nx
        root.y = ny
    }
}
