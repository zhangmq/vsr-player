import QtQuick
import QtQuick.Controls
import "components"

/// 右键菜单（播放器惯例）。复刻 PopupBase 外观；定位 = 鼠标位置
///（anchorTarget 语义不适用），宿主席位 showAt() 后 open()。
/// 条目可带 `submenu: [{label, value}]`（目前只有"反交错"）→ 悬停/点击在右侧
/// 展开二级面板，选中后关闭整个菜单。
///
/// ⚠ 二级面板画在**同一个 Popup 里**（打开子菜单时 popup 加宽、面板占右半边），
/// 不用嵌套 Popup：两个 popup 会各自抢 grab，点面板会被外层判成"点击外部"
/// → 菜单自己关掉。代价是 popup 几何在子菜单打开时变化，所以背景用两条独立的
/// Rectangle 分别画"主菜单"和"二级面板"（PopupBase 的默认背景是整块）。
PopupBase {
    id: root

    padding: 0
    width: menuWidth + (subOpen ? subMenuGap + subMenuWidth : 0)

    readonly property int menuWidth: 230
    readonly property int subMenuWidth: 150
    readonly property int subMenuGap: 4
    readonly property int rowH: 30
    readonly property int padY: 8

    property bool subOpen: false

    signal openFilesRequested()
    signal openFolderRequested()
    signal openUrlRequested()
    signal appendFilesRequested()
    signal appendFolderRequested()
    /// 二级菜单里选中了某个反交错档位（0=Off / 1=Auto / 2=Force）。
    signal deinterlaceSelected(int mode)
    signal quitRequested()

    /// 菜单项（sig 须与上方 signal 声明同名；新增项同步改三处：
    /// signal 声明 + 此数组 + main.qml handler）。`hint` 可为字符串或函数
    /// （动态值）；`submenu` 存在即渲染为二级菜单入口。
    readonly property var items: [
        {icon: "󰉋", text: qsTr("Open file"),          hint: "Ctrl+O", sig: "openFilesRequested"},
        {icon: "󰉖", text: qsTr("Open folder"), hint: "Ctrl+Shift+O", sig: "openFolderRequested"},
        {icon: "󰌷", text: qsTr("Open URL"), hint: "Ctrl+L", sig: "openUrlRequested"},
        {icon: "󰐒", text: qsTr("Add files to playlist"),    hint: "", sig: "appendFilesRequested"},
        {icon: "󰉗", text: qsTr("Add folder to playlist"),   hint: "", sig: "appendFolderRequested"},
        {sep: true},
        {icon: "󰉸", text: qsTr("Deinterlace"),
         hint: function() { return root.deintLabel() },
         submenu: [
             {label: qsTr("Off"),   value: 0},
             {label: qsTr("Auto"),  value: 1},
             {label: qsTr("Force"), value: 2}
         ]},
        {sep: true},
        {icon: "󰐥", text: qsTr("Quit"),               hint: "Ctrl+Q", sig: "quitRequested"}
    ]

    /// 反交错档位文案（= mpv `--deinterlace` 的 no/auto/yes）。
    function deintLabel() {
        switch (viewModel.deintMode) {
        case 1: return qsTr("Auto")
        case 2: return qsTr("Force")
        default: return qsTr("Off")
        }
    }

    /// 二级面板的 y：与所属条目行对齐（行高/分隔线逐个累加）。
    readonly property real subY: {
        var y = root.padY
        for (var i = 0; i < items.length; i++) {
            if (items[i].submenu !== undefined) return y
            y += items[i].sep === true ? 1 : rowH
        }
        return y
    }

    /// 二级菜单条目的子项模型（目前只有反交错）
    readonly property var subItems: {
        for (var i = 0; i < items.length; i++)
            if (items[i].submenu !== undefined) return items[i].submenu
        return []
    }

    /// 每次打开都从收起状态开始（不记忆上次展开的子菜单）。
    onAboutToShow: subOpen = false

    /// 在 item 坐标系 (x,y)（即鼠标位置）弹出，屏幕边缘钳制。
    /// 横向按**展开二级菜单后的宽度**钳制：面板只在悬停时出现，但这时 popup
    /// 已经加宽，若不预留空间会把面板顶到屏幕外。
    function showAt(item, x, y) {
        if (!item || !parent) return
        var p = item.mapToItem(parent, x, y)
        var fullW = root.menuWidth + root.subMenuGap + root.subMenuWidth
        root.x = Math.min(Math.max(p.x, 4), Math.max(4, parent.width - fullW - 4))
        root.y = Math.min(Math.max(p.y, 4), Math.max(4, parent.height - root.height - 4))
        open()
    }

    background: Item {
        // 主菜单底
        Rectangle {
            width: root.menuWidth
            height: parent.height
            color: "#d9111111"; radius: 8
            border { width: 1; color: "#22ffffff" }
        }
        // 二级面板底（比条目行上下各多 6px）
        Rectangle {
            visible: root.subOpen
            x: root.menuWidth + root.subMenuGap
            y: root.subY - 6
            width: root.subMenuWidth
            height: root.subItems.length * root.rowH + 12
            color: "#d9111111"; radius: 8
            border { width: 1; color: "#22ffffff" }
        }
    }

    contentItem: Item {
        implicitWidth: root.menuWidth
        implicitHeight: mainColumn.implicitHeight + 2 * root.padY

        Column {
            id: mainColumn
            x: 0; y: root.padY
            width: root.menuWidth
            Repeater {
                model: root.items
                delegate: Item {
                    // modelData 字段缺失时为 undefined——显式 bool/默认值
                    //（undefined 赋 bool/QString 报 "Unable to assign"）
                    property bool isSep: modelData.sep === true
                    property bool hasSub: modelData.submenu !== undefined
                    property var hintSrc: modelData.hint
                    property string hintText: typeof hintSrc === "function" ? hintSrc() : (hintSrc || "")
                    width: parent.width
                    height: isSep ? 1 : root.rowH

                    Rectangle {
                        anchors.fill: parent
                        color: !isSep && miHover.containsMouse ? "#33ffffff" : "transparent"
                        visible: !isSep
                    }
                    // 分隔线（sep 行）
                    Rectangle {
                        anchors { left: parent.left; leftMargin: 10; right: parent.right; rightMargin: 10 }
                        height: 1; color: "#22ffffff"; visible: isSep
                    }
                    Row {
                        anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                        spacing: 8
                        visible: !isSep
                        Text { font.family: iconFont; font.pixelSize: 16; text: modelData.icon || ""
                            color: "#e0e0e0"; renderType: Text.NativeRendering }
                        Text { text: modelData.text || ""; color: "#e0e0e0"; font.pixelSize: 13 }
                    }
                    // 右侧：hint（键位/当前档位）+ 二级菜单箭头
                    Row {
                        anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                        spacing: 4
                        visible: !isSep
                        Text { text: hintText; color: "#808080"; font.pixelSize: 13
                            visible: hintText !== ""; anchors.verticalCenter: parent.verticalCenter }
                        Text { font.family: iconFont; font.pixelSize: 14; text: "󰅂"
                            color: "#808080"; visible: hasSub
                            renderType: Text.NativeRendering
                            anchors.verticalCenter: parent.verticalCenter }
                    }
                    MouseArea {
                        id: miHover
                        anchors.fill: parent
                        hoverEnabled: true
                        visible: !isSep
                        cursorShape: Qt.PointingHandCursor
                        // 悬停即展开二级菜单；移到别的条目上收起
                        onEntered: root.subOpen = hasSub
                        onClicked: {
                            if (hasSub)    root.subOpen = true
                            else { root.close(); root[modelData.sig]() }
                        }
                    }
                }
            }
        }

        // ── 二级菜单面板（反交错档位）────────────────────────────────
        Column {
            id: subColumn
            visible: root.subOpen
            x: root.menuWidth + root.subMenuGap
            y: root.subY
            width: root.subMenuWidth
            Repeater {
                model: root.subItems
                delegate: Item {
                    width: subColumn.width
                    height: root.rowH
                    readonly property bool current: modelData.value === viewModel.deintMode
                    Rectangle {
                        anchors.fill: parent
                        color: subHover.containsMouse ? "#33ffffff" : "transparent"
                    }
                    Text {
                        anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                        text: modelData.label
                        color: current ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13
                    }
                    Text {
                        anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                        font.family: iconFont; font.pixelSize: 14; text: "󰄬"
                        color: "#ffcc00"; visible: current
                        renderType: Text.NativeRendering
                    }
                    MouseArea {
                        id: subHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.deinterlaceSelected(modelData.value)
                            root.subOpen = false
                            root.close()
                        }
                    }
                }
            }
        }
    }
}
