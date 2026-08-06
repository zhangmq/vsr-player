import QtQuick
import QtQuick.Controls
import "components"

/// 播放列表面板（数据源：mpv `playlist` 属性 → PlaylistModel 增量镜像）。
/// 点击条目 → viewModel.playlistPlayIndex（mpv playlist-play-index）。
/// 性能：reuseItems 复用 delegate（滚动零创建销毁，ToolTip 实例数 =
/// 可见项数而非条目总数）。ToolTip 用显式实例（attached 共享实例视觉
/// 取系统 palette，无法断开系统配色——2026-08-06）。
/// 显示 basename（model.display），完整路径放 tooltip（model.path）。
Drawer {
    id: root
    edge: Qt.RightEdge
    // Drawer 默认 modal:true 会压暗主窗口（mask）——与音量/画质等
    // popup 一致，无需 mask。点击外部/Esc 关闭（与 PopupBase 同款）。
    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: 320; height: parent ? parent.height : 600; z: 10
    dragMargin: 0
    topPadding: 0; bottomPadding: 0; leftPadding: 0; rightPadding: 0
    // 配色同 PopupBase：面板底 #d9111111 + 1px 白边 + 圆角
    //（原纯黑 #d9000000 非方案色）
    background: Rectangle { color: "#d9111111"; radius: 8
        border { width: 1; color: "#22ffffff" } }

    Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 48; color: "#22ffffff"
        Text { anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            text: qsTr("Playlist"); color: "#e0e0e0"; font.pixelSize: 13; font.bold: true }
        Text { anchors { right: clearBtn.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
            text: viewModel.playlistModel.currentIndex >= 0
                  ? (viewModel.playlistModel.currentIndex + 1) + "/" + viewModel.playlistModel.count
                  : ""
            color: "#b0b0b0"; font.pixelSize: 13 }

        // 清空列表（无确认弹窗——误点损失低，文件可随时重拖入，YAGNI）
        IconButton {
            id: clearBtn
            anchors { right: closeBtn.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
            label: qsTr("Clear"); size: 22
            tooltip: qsTr("Clear playlist")
            onClicked: viewModel.playlistClear()
        }

        Item { id: closeBtn; width: 34; height: 34
            anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
            Rectangle { anchors.fill: parent; radius: 4
                color: clHover.hovered ? "#33ffffff" : "transparent"
                Behavior on color { ColorAnimation { duration: 150 } } }
            Text { anchors.centerIn: parent; font.family: iconFont; font.pixelSize: 18
                text: "󰅖"; color: clHover.hovered ? "#ffffff" : "#c8c8c8"
                Behavior on color { ColorAnimation { duration: 150 } }
                renderType: Text.NativeRendering }
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; hoverEnabled: true
                onClicked: root.close() }
            HoverHandler { id: clHover }
        }
    }

    ListView {
        id: playlistView
        anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 48
                  bottom: parent.bottom }
        model: viewModel.playlistModel
        cacheBuffer: 200
        clip: true
        // 滚动条（attached ScrollBar；policy 默认 AsNeeded——条目数
        // 不足一屏时自动隐藏）。thumb 几何由 control 自动设置（宽 =
        // availableWidth × visualSize），但滚动条自身宽度来自
        // implicitContentWidth（Basic 模板 6+padding 2×2=10）——不给
        // implicitWidth 则 thumb 为 0 宽，完全不可见。
        ScrollBar.vertical: ScrollBar {
            contentItem: Rectangle {
                implicitWidth: 6
                radius: 3
                color: parent.pressed ? "#ffffffff"
                     : (parent.hovered ? "#b3ffffff" : "#80ffffff")
            }
        }

        // 滚轮加速：Qt 默认每 120° 滚 wheelScrollLines(3)×24 = 72px
        //（约 1.7 行/格，偏慢）。接管 wheel 事件每格滚 4 行（168px），
        // 即时滚动（无 300ms 缓动动画，消除粘滞感）；触控板平滑滚动
        //（pixelDelta）不加速保持精确。acceptedButtons: NoButton——
        // 只处理滚轮，点击/hover/拖拽全部穿透。
        // contentY 直接赋值不自动钳制（setContentY 只 setValue，fixup
        // 延迟到下一帧）→ 手动 clamp 到 [0, contentHeight-height]。
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            onWheel: function(wheel) {
                var delta = 0
                if (wheel.pixelDelta.y !== 0)
                    delta = wheel.pixelDelta.y
                else if (wheel.angleDelta.y !== 0)
                    delta = wheel.angleDelta.y / 120.0 * 168
                if (delta === 0) return
                var maxY = playlistView.contentHeight - playlistView.height
                playlistView.contentY = Math.max(0, Math.min(maxY, playlistView.contentY - delta))
                wheel.accepted = true
            }
        }
        // delegate 实例复用（Qt 5.15+）：滚动只更新绑定不复建对象树。
        // 旧实现无 reuseItems，每滚一行销毁/创建 2 个 delegate（各 7 对象）。
        reuseItems: true

        delegate: Rectangle {
            id: plDelegate
            width: ListView.view.width; height: 42; clip: true
            // 选中/悬停对齐全局方案（选中 #33ffcc00 / 悬停 #33ffffff）
            color: plMouse.containsMouse ? "#33ffffff"
                 : (index === viewModel.playlistModel.currentIndex ? "#33ffcc00" : "transparent")

            Row {
                anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }

                Text {
                    text: (index + 1) + ". " + model.display
                    width: 260
                    color: index === viewModel.playlistModel.currentIndex ? "#ffcc00" : "#e0e0e0"
                    font.pixelSize: 13
                    elide: Text.ElideMiddle
                    renderType: Text.NativeRendering
                }
            }

            // 行尾移除按钮（hover 显示；z 高于 plMouse——否则点击被抢）
            Text {
                id: removeBtn
                z: 2
                width: 24; height: 24
                anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                text: "×"; color: "#c8c8c8"; font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                visible: plMouse.containsMouse
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: viewModel.playlistRemove(index)
                }
            }

            // 点击 + hover 合并一个 MouseArea（去 HoverHandler）：
            // tooltip 显示完整路径（model.path），列表项只显示文件名。
            // ToolTip 用附加属性（共享视觉实例，位置由框架跟随鼠标——
            // 自定义实例需 parent 绑定，无 parent 的 open() 定位异常）。
            // 附加属性零对象开销，文本按 item 存储。
            MouseArea {
                id: plMouse
                anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                hoverEnabled: true
                onClicked: viewModel.playlistPlayIndex(index)
                // 断开系统配色：attached ToolTip 视觉 = 样式共享实例（系统
                // palette）且无法自绘——改显式实例（样式自带 parent 上方
                // 居中定位；reuseItems 复用下实例数 = 可见项数，可接受）
                ToolTip {
                    visible: plMouse.containsMouse
                    text: model.path; delay: 600
                    background: Rectangle { color: "#d9111111"; radius: 4
                        border { width: 1; color: "#22ffffff" } }
                    contentItem: Text { text: model.path; color: "#e0e0e0"; font.pixelSize: 13 }
                }
            }
        }
    }

    // 拖入文件 → 一律追加到列表（不打断当前播放；DropArea 在 ListView
    // 之后声明，z 更高，拖放优先于 delegate 的点击命中）
    DropArea {
        anchors.fill: parent
        onEntered: function(drag) { drag.accepted = true }
        onDropped: function(drop) {
            var paths = []
            for (var i = 0; i < drop.urls.length; i++)
                paths.push(drop.urls[i].toString())
            viewModel.openFiles(paths, 1)
        }
    }
}
