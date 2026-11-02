import QtQuick
import QtQuick.Controls
import "components"

/// 右键菜单（播放器惯例）。复刻 PopupBase 外观；定位 = 鼠标位置
///（anchorTarget 语义不适用），宿主席位 showAt() 后 open()。
/// 条目可带 `submenu: [{label, value}]`（反交错 / 旋转 / 3D / 轨道 / 网络流）
/// → 悬停/点击在右侧展开二级面板，选中后关闭整个菜单。多个二级菜单共用同
/// 一个面板：subIndex 记录当前展开的是哪一条（悬停切换）。
///
/// `submenu` 可以是数组，也可以是**函数**（返回数组）——轨道/网络流是动态的，
/// 用函数可以让面板每次展开时取最新值，而不必重建整个 items。
///
/// ⚠ 二级面板画在**同一个 Popup 里**（打开子菜单时 popup 加宽、面板占右半边），
/// 不用嵌套 Popup：两个 popup 会各自抢 grab，点面板会被外层判成"点击外部"
/// → 菜单自己关掉。代价是 popup 几何在子菜单打开时变化，所以背景用两条独立的
/// Rectangle 分别画"主菜单"和"二级面板"（PopupBase 的默认背景是整块）。
PopupBase {
    id: root

    padding: 0
    width: menuWidth + (subOpen ? subMenuGap + subMenuWidth : 0)
    // 主菜单自身高度（popup 高度 = 它；二级面板必须在这个范围内排布，
    // 否则会被 popup 裁掉 —— popup 是固定大小的自绘 Item，不随子项长高）。
    readonly property real mainPanelH: mainColumn
        ? mainColumn.implicitHeight + 2 * padY : 0

    readonly property int menuWidth: 230
    /// 二级面板加宽到能放下网络流标题（"1080P 高清 · AV1 · 1.9M"）
    readonly property int subMenuWidth: 240
    readonly property int subMenuGap: 4
    readonly property int rowH: 30
    readonly property int padY: 8

    property bool subOpen: false
    /// 当前展开二级菜单的条目下标（-1 = 收起）。两个二级菜单共用面板。
    property int subIndex: -1

    signal openFilesRequested()
    signal openFolderRequested()
    signal openUrlRequested()
    signal appendFilesRequested()
    signal appendFolderRequested()
    /// 从播放列表移除当前条目（不碰磁盘）。
    signal removeFromPlaylistRequested()
    /// 删除当前文件（移到回收站）。仅本地可写文件时可见。
    signal deleteFileRequested()
    /// 二级菜单里选中了某个反交错档位（0=Off / 1=Auto / 2=Force）。
    signal deinterlaceSelected(int mode)
    /// 二级菜单里选中了某个旋转角度（0=Off / 90=顺时针 / 270=逆时针 90°）。
    signal rotateSelected(int mode)
    /// 3D 三态（0=不使用 / 1=3D片源 / 2=2D转3D；运行期状态，不持久化）。
    /// group: 0 = 显示方式（mode），1 = 输出设备（outputType）——二级面板分两组
    signal stereoSelected(int value, int group)
    signal quitRequested()

    // ── 轨道 / 流（原本在底栏的轨道弹窗里，2026-11-02 移到右键菜单）──
    signal videoTrackPicked(int id)          // -2 = 自动（vid auto）
    signal audioTrackPicked(int id)          // -2 = 自动（aid auto）
    signal subtitlePicked(int id)            // -1 = 关（sid no）
    signal subtitleFilePickRequested()       // "加载字幕文件…"
    signal externalSubtitleRemoveRequested(int id)
    signal nearbySubtitlePicked(string path) // 目录里扫到的未加载字幕
    signal editionPicked(int id)             // 网络流（mpv edition）
    // 延迟：±50ms 步进（debounce 在 PlayerViewModel 里做，500ms 合并一次写入）
    signal subDelayNudged(real delta)
    signal subDelayResetRequested()
    signal audioDelayNudged(real delta)
    signal audioDelayResetRequested()

    /// 菜单项（sig 须与上方 signal 声明同名；新增项同步改三处：
    /// signal 声明 + 此数组 + main.qml handler）。`hint` 可为字符串或函数
    /// （动态值）；`submenu` 存在即渲染为二级菜单入口（数组或函数）。
    readonly property var items: [
        {icon: "󰉋", text: qsTr("Open file"),          hint: "Ctrl+O", sig: "openFilesRequested"},
        {icon: "󰉖", text: qsTr("Open folder"), hint: "Ctrl+Shift+O", sig: "openFolderRequested"},
        {icon: "󰌷", text: qsTr("Open URL"), hint: "Ctrl+L", sig: "openUrlRequested"},
        {icon: "󰐒", text: qsTr("Add files to playlist"),    hint: "", sig: "appendFilesRequested"},
        {icon: "󰉗", text: qsTr("Add folder to playlist"),   hint: "", sig: "appendFolderRequested"},
        {sep: true},
        {icon: "󰐓", text: qsTr("Remove from playlist"), hint: "Del", sig: "removeFromPlaylistRequested",
         visibleWhen: function() { return viewModel.hasMedia }},
        {icon: "󰆴", text: qsTr("Delete file"), hint: "Shift+Del", sig: "deleteFileRequested",
         visibleWhen: function() { return viewModel.hasMedia && viewModel.canDeleteCurrentFile }},
        {sep: true},

        // ── 轨道 / 流（按文件；无媒体时整块隐藏）──────────────────────
        {header: qsTr("Tracks"), visibleWhen: function() { return root.hasTracks() }},
        // 轨道/流入口**不显示当前值**：轨道名/流标题都很长（实测 39 条轨道、
        // "1080P 高清 · AV1 · 1.9M" 这类），在 230px 宽的菜单里会盖住条目名。
        // 当前值在二级面板里用勾选表达。
        {icon: "󰕧", text: qsTr("Video"),
         subSig: "videoTrackPicked",
         current: function() { return root.selectedTrackId("video") },
         submenu: function() { return root.videoTracksMenu() },
         visibleWhen: function() { return root.hasTracks() }},
        {icon: "󰝚", text: qsTr("Audio"),
         subSig: "audioTrackPicked",
         current: function() { return root.selectedTrackId("audio") },
         submenu: function() { return root.audioTracksMenu() },
         visibleWhen: function() { return root.hasTracks() }},
        // 字幕**恒显示**（哪怕本片一条字幕轨都没有）：还要能"加载字幕文件…"
        // 和从"附近字幕"里加，所以不能按"有没有字幕轨"来决定是否显示。
        {icon: "󰨖", text: qsTr("Subtitles"),
         subSig: "subtitlePicked",
         current: function() { return root.selectedTrackId("sub") },
         submenu: function() { return root.subtitleMenu() },
         visibleWhen: function() { return root.hasTracks() }},
        {control: "delay", kind: "sub", text: qsTr("Subtitle delay"),
         visibleWhen: function() { return root.hasTracks() }},
        {control: "delay", kind: "audio", text: qsTr("Audio delay"),
         visibleWhen: function() { return root.hasTracks() }},
        {icon: "󱜠", text: qsTr("Network stream"),
         subSig: "editionPicked",
         current: function() { return viewModel.currentEdition },
         submenu: function() { return root.editionsMenu() },
         visibleWhen: function() { return viewModel.networkSource && root.hasEditions() }},

        // ── 画面处理（反交错 / 旋转 等"处理画面"的选项归成一组）──────────
        {sep: true},
        {header: qsTr("Picture processing")},
        {icon: "󰉸", text: qsTr("Deinterlace"),
         hint: function() { return root.deintLabel() },
         subSig: "deinterlaceSelected",
         current: function() { return viewModel.deintMode },
         submenu: [
             {label: qsTr("Off"),   value: 0},
             {label: qsTr("Auto"),  value: 1},
             {label: qsTr("Force"), value: 2}
         ]},
        {icon: "󰚖", text: qsTr("Rotate"),
         hint: function() { return root.rotLabel() },
         subSig: "rotateSelected",
         current: function() { return viewModel.rotMode },
         submenu: [
             {label: qsTr("Off"),  value: 0},
             {label: qsTr("90°"),  value: 90},
             {label: qsTr("270°"), value: 270}
         ]},
        // ── 立体（3D 分组 = 一级菜单里的小块，三条入口）──────────────────
        {header: qsTr("3D")},
        {icon: "", text: qsTr("Output mode"),
         hint: function() { return root.stereoModeLabel() },
         subSig: "stereoSelected",
         current: function(g) { return g === 0 ? root.stereoOutputMode() : -1 },
         submenu: [
             {label: qsTr("Off"),        value: 0, group: 0},
             {label: qsTr("3D glasses"), value: 1, group: 0},
             {label: qsTr("2D output"),  value: 2, group: 0}
         ]},
        {icon: "", text: qsTr("Source layout"),
         hint: function() { return root.layoutLabel() },
         subSig: "stereoSelected",
         current: function(g) { return g === 1 ? root.layoutIndex() : -1 },
         submenu: [
             {label: qsTr("Side by side · full"), value: 0, group: 1},
             {label: qsTr("Side by side · half"), value: 1, group: 1},
             {label: qsTr("Top and bottom · full"), value: 2, group: 1},
             {label: qsTr("Top and bottom · half"), value: 3, group: 1}
         ]},
        {icon: "", text: qsTr("Output option"),
         hint: function() { return root.eyeLabel() },
         subSig: "stereoSelected",
         current: function(g) { return g === 2 ? viewModel.eyePref : -1 },
         submenu: [
             {label: qsTr("Left eye"),  value: 0, group: 2},
             {label: qsTr("Right eye"), value: 1, group: 2}
         ]},
        {sep: true},
        {icon: "󰐥", text: qsTr("Quit"),               hint: "Ctrl+Q", sig: "quitRequested"}
    ]

    // ── 轨道 / 流 数据 ────────────────────────────────────────────────
    /// 轨道行文本：`标题 / 语言 · 编码`（外部字幕再加 "外部 · 文件名"）
    function trackLabel(t) {
        var parts = []
        var name = t.title && t.title.length > 0 ? t.title
                 : (t.lang && t.lang.length > 0 ? t.lang : qsTr("Track %1").arg(t.id))
        parts.push(name)
        if (t.lang && t.lang.length > 0 && name !== t.lang)
            parts.push(t.lang)
        if (t.codec && t.codec.length > 0 && t.codec !== "null")
            parts.push(t.codec)
        if (t.external) {
            parts.push(qsTr("External"))
            var f = String(t.filename || "")
            if (f.length > 0) {
                var base = f.substring(f.lastIndexOf("/") + 1)
                if (base.length > 0) parts.push(base)
            }
        }
        return parts.join(" · ")
    }

    /// 轨道组是否显示：直接看 track-list 有没有条目（媒体一加载就填充）。
    /// 不用 hasMedia——那是个"fileLoaded_"标志位，多一层间接来源。
    function hasTracks() { return viewModel.trackList.length > 0 }

    function tracksOf(type) {
        var out = []
        for (var i = 0; i < viewModel.trackList.length; i++) {
            var t = viewModel.trackList[i]
            if (t.type === type) out.push(t)
        }
        return out
    }

    function selectedTrackId(type) {
        var ts = root.tracksOf(type)
        for (var i = 0; i < ts.length; i++)
            if (ts[i].selected) return ts[i].id
        return -1
    }

    function trackHint(type) {
        var id = root.selectedTrackId(type)
        var ts = root.tracksOf(type)
        for (var i = 0; i < ts.length; i++)
            if (ts[i].id === id) return root.trackLabel(ts[i])
        return qsTr("Auto")
    }

    function videoTracksMenu() {
        var out = [{label: qsTr("Auto"), value: -2}]
        var ts = root.tracksOf("video")
        for (var i = 0; i < ts.length; i++)
            out.push({label: root.trackLabel(ts[i]), value: ts[i].id})
        return out
    }

    function audioTracksMenu() {
        var out = [{label: qsTr("Auto"), value: -2}]
        var ts = root.tracksOf("audio")
        for (var i = 0; i < ts.length; i++)
            out.push({label: root.trackLabel(ts[i]), value: ts[i].id})
        return out
    }

    /// 当前选中的字幕轨（外部轨同时给出 id）
    function currentSubtitleTrack() {
        var ts = root.tracksOf("sub")
        for (var i = 0; i < ts.length; i++)
            if (ts[i].selected) return ts[i]
        return null
    }

    function subtitleHint() {
        if (!viewModel.subVisible) return qsTr("Off")
        var t = root.currentSubtitleTrack()
        return t ? root.trackLabel(t) : qsTr("Off")
    }

    /// 内嵌字幕轨（外部字幕不进"字幕轨道"区——它们属于"附近字幕"那一组）
    function internalSubTracks() {
        var out = []
        var ts = root.tracksOf("sub")
        for (var i = 0; i < ts.length; i++)
            if (!ts[i].external) out.push(ts[i])
        return out
    }

    /// 已加载的外部字幕：filename → track id（"附近字幕"里点已加载的项 = 选轨，不重复 sub-add）
    function loadedExternalSubs() {
        var map = ({})
        var ts = root.tracksOf("sub")
        for (var i = 0; i < ts.length; i++)
            if (ts[i].external && ts[i].filename) map[String(ts[i].filename)] = ts[i].id
        return map
    }

    /// 字幕二级面板。顺序（2026-11-02 用户定）：
    ///   关 / 加载字幕文件… / 移除外部字幕
    ///   ── 附近字幕（固定高度滚动区，内容超了显示上下小三角）
    ///   ── 字幕轨道（固定高度滚动区）
    /// 逻辑：用户既然往片源目录里放了字幕文件，就说明内嵌字幕不合适 —— 所以
    /// "附近字幕"排在"字幕轨道"前面。
    function subtitleMenu() {
        var out = []
        out.push({label: qsTr("Off"), value: -1})
        out.push({label: qsTr("Load subtitle file…"), sig: "subtitleFilePickRequested"})
        var cur = root.currentSubtitleTrack()
        if (cur && cur.external)
            out.push({label: qsTr("Remove external subtitle"),
                      sig: "externalSubtitleRemoveRequested", arg: cur.id})

        var files = viewModel.subtitleFiles
        if (files && files.length > 0) {
            out.push({sep: true})
            out.push({header: qsTr("Nearby subtitles")})
            var loaded = root.loadedExternalSubs()
            for (var i = 0; i < files.length; i++) {
                var f = files[i]
                var path = String(f.path)
                var entry = {
                    label: String(f.name),
                    section: "nearby", sectionRows: root.nearbySectionRows
                }
                if (loaded[path] !== undefined)
                    entry.value = loaded[path]      // 已加载 → 点击 = 选这条轨
                else {
                    entry.sig = "nearbySubtitlePicked"   // 未加载 → 点击 = sub-add + 选中
                    entry.arg = path
                }
                out.push(entry)
            }
        }

        var ts = root.internalSubTracks()
        if (ts.length > 0) {
            out.push({sep: true})
            out.push({header: qsTr("Subtitle tracks")})
            for (var j = 0; j < ts.length; j++)
                out.push({label: root.trackLabel(ts[j]), value: ts[j].id,
                          section: "tracks", sectionRows: root.trackSectionRows})
        }
        return out
    }

    // ── 网络流（mpv edition）──────────────────────────────────────────
    function hasEditions() { return viewModel.editions.length > 1 }

    /// mpv 给的标题形如 "1080P 高清 (av1 1920x1080 29 fps 1922 kbps)"：
    /// 整理成 "1080P 高清 · AV1 · 1.9M"（跳过 null 编码），解析失败就原样显示。
    function editionLabel(title) {
        var t = String(title || "")
        var m = t.match(/^(.*?)\s*\(([^)]*)\)\s*$/)
        if (!m) return t
        var name = m[1]
        var meta = m[2].split(/\s+/)
        var parts = [name]
        for (var i = 0; i < meta.length; i++) {
            var w = meta[i]
            if (w === "null" || w.length === 0) continue
            if (w === "av1" || w === "h264" || w === "hevc" || w === "vp9" || w === "avc1")
                parts.push(w.toUpperCase())
            else if (/x[0-9]+$/i.test(w) || /^[0-9]+x[0-9]+$/.test(w))
                parts.push(w)   // 分辨率
            else if (/kbps$/.test(w) || /^[0-9]+$/.test(w)) {
                // "1922 kbps" → 两项分开出现：数字后面紧跟 kbps 时合并
                var kbps = parseInt(w, 10)
                if (!isNaN(kbps)) {
                    if (kbps >= 1000) parts.push((kbps / 1000).toFixed(1) + "M")
                    else parts.push(kbps + "k")
                }
            }
        }
        return parts.join(" · ")
    }

    function editionsMenu() {
        var out = []
        var eds = viewModel.editions
        for (var i = 0; i < eds.length; i++)
            out.push({label: root.editionLabel(eds[i].title), value: eds[i].id})
        return out
    }

    function editionHint() {
        var eds = viewModel.editions
        var cur = viewModel.currentEdition
        for (var i = 0; i < eds.length; i++)
            if (eds[i].id === cur) return root.editionLabel(eds[i].title)
        return ""
    }

    // ── 延迟（字幕 / 音频）：±50ms，显示统一用毫秒 ──────────────────────
    function delayValue(kind) {
        return kind === "audio" ? viewModel.audioDelay : viewModel.subDelay
    }
    function delayText(sec) {
        var ms = Math.round(sec * 1000)
        return (ms > 0 ? "+" : "") + ms + "ms"
    }

    function delayNudge(kind, delta) {
        if (kind === "audio") root.audioDelayNudged(delta)
        else root.subDelayNudged(delta)
    }
    function delayReset(kind) {
        if (kind === "audio") root.audioDelayResetRequested()
        else root.subDelayResetRequested()
    }

    /// 反交错档位文案（= mpv `--deinterlace` 的 no/auto/yes）。
    function deintLabel() {
        switch (viewModel.deintMode) {
        case 1: return qsTr("Auto")
        case 2: return qsTr("Force")
        default: return qsTr("Off")
        }
    }

    /// 输出方式三态：0=关闭 / 1=眼镜 3D / 2=单眼 2D。
    /// 3D 的 active 由 StereoController 管，单眼由 viewModel.eyeMode 管（两者互斥，
    /// 互斥在 main.qml 的 onStereoSelected 里保证）。
    function stereoOutputMode() {
        if (stereo.active) return 1
        return viewModel.eyeMode !== 0 ? 2 : 0
    }
    function stereoModeLabel() {
        switch (stereoOutputMode()) {
        case 1: return qsTr("3D glasses")
        case 2: return qsTr("2D output")
        default: return qsTr("Off")
        }
    }
    /// 片源格式：0=左右·全 / 1=左右·半 / 2=上下·全 / 3=上下·半。
    function layoutIndex() {
        return (stereo.layout === 1 ? 2 : 0) + (stereo.half ? 1 : 0)
    }
    function layoutLabel() {
        return (stereo.layout === 1 ? qsTr("Top and bottom") : qsTr("Side by side"))
             + " · " + (stereo.half ? qsTr("half") : qsTr("full"))
    }
    /// 输出选项：单眼输出用哪只眼（独立状态，不随输出方式变）。
    function eyeLabel() {
        return viewModel.eyePref === 1 ? qsTr("Right eye") : qsTr("Left eye")
    }
    /// 旋转角度文案（0=Off / 90=顺时针 / 270=逆时针 90°）。
    function rotLabel() {
        switch (viewModel.rotMode) {
        case 90:  return qsTr("90°")
        case 270: return qsTr("270°")
        default:  return qsTr("Off")
        }
    }

    /// 条目是否显示（`visibleWhen` 缺省 = 恒显示；不满足条件的条目不占高度）。
    function itemShown(it) {
        return it.visibleWhen === undefined ? true : it.visibleWhen()
    }

    /// 二级面板内容行数 → 高度（分组标题行矮 4px，分隔线 1px）
    function subRowH(item) {
        if (item.sep === true) return 1
        return item.header !== undefined ? root.rowH - 4 : root.rowH
    }

    /// 二级面板的 y：与所属条目行对齐。
    /// **取委托的实际布局位置**，不再用 items 数组重推行高：隐藏行（visibleWhen，
    /// 例如 URL/直播没有"删除文件"）由 Column 布局决定高度 0，而 items 侧的
    /// visibleWhen 求值时机可能与委托的 shown 不同步——重算会多算/少算隐藏行，
    /// 面板就会整体偏下/偏上（2026-10-25 实测）。
    readonly property real subY: {
        // popup 的 contentItem 是懒创建的：首次打开前 mainColumn 还不存在。
        // 读 mainColumn.height 同时登记"布局变化"依赖（行显示/隐藏时重算）。
        if (!mainColumn)
            return root.padY
        var mh = mainColumn.height
        if (!root.subOpen)
            return mainColumn.y + mh
        var y = mainColumn.y + mh
        for (var i = 0; i < mainColumn.children.length; i++) {
            var c = mainColumn.children[i]
            if (c.itemIndex === root.subIndex) {
                y = mainColumn.y + c.y
                break
            }
        }
        // 底部钳制：面板不能超出**主菜单自身**的下边缘（popup 高度 = 主菜单
        // 高度，超出部分会被裁掉）。分组后 3D 面板有 6 行，条目靠下时整块
        // 跑出可视区 → 整体上移到刚好放得下。
        var maxY = root.mainPanelH - root.subViewH - root.padY
        if (y > maxY)
            y = Math.max(root.padY, maxY)
        return y
    }

    /// 两个滚动区固定显示的行数（用户 2026-11-02：固定高度 + 上下小三角提示）
    readonly property int nearbySectionRows: 4
    readonly property int trackSectionRows: 8
    /// 上下小三角所在行的高度
    readonly property int triH: 14

    /// 二级面板分组：连续的、带同一个 `section` 的条目归为一组（组内固定高度可滚动），
    /// 其余条目按原样逐行渲染。返回 [{section, items, maxRows}]
    readonly property var subGroups: {
        var m = root.subItems
        var groups = []
        var cur = null
        for (var i = 0; i < m.length; i++) {
            var it = m[i]
            var sec = it.section !== undefined ? String(it.section) : ""
            var rows = it.sectionRows !== undefined ? it.sectionRows : 4
            if (cur && cur.section === sec && (sec !== "" || cur.items.length === 0)) {
                cur.items.push(it); cur.maxRows = rows
            } else {
                cur = { section: sec, items: [it], maxRows: rows }
                groups.push(cur)
            }
        }
        return groups
    }

    function groupScrolls(g) {
        return g.section !== "" && g.items.length > g.maxRows
    }

    /// 组高度：滚动组 = 固定行数 + 上下三角；其它 = 各行自然高度
    function groupHeight(g) {
        if (root.groupScrolls(g))
            return g.maxRows * root.rowH + 2 * root.triH
        var h = 0
        for (var i = 0; i < g.items.length; i++)
            h += root.subRowH(g.items[i])
        return h
    }

    /// 二级面板内容总高 / 可视高
    readonly property real subContentH: {
        var h = 0
        var gs = root.subGroups
        for (var i = 0; i < gs.length; i++)
            h += root.groupHeight(gs[i])
        return h
    }
    readonly property real subViewH: root.subContentH

    /// 某一行是否是"当前值"（勾选态）
    function isCurrentSubRow(row) {
        var it = root.subIndex >= 0 ? root.items[root.subIndex] : null
        if (!it || !it.current) return false
        if (row.header !== undefined || row.sep === true || row.sig !== undefined) return false
        return row.value === it.current(row.group || 0)
    }

    /// 点击二级面板里的某一行：动作行发自己的 sig；普通行交给所属条目的 subSig
    function activateSubRow(row) {
        var it = root.subIndex >= 0 ? root.items[root.subIndex] : null
        if (row.sig !== undefined) {
            if (row.arg !== undefined) root[row.sig](row.arg)
            else root[row.sig]()
        } else if (it && it.subSig) {
            root[it.subSig](row.value, row.group || 0)
        }
        root.subOpen = false
        root.subIndex = -1
        root.close()
    }

    /// 当前展开的二级菜单子项模型（subIndex 指向的条目；submenu 可为函数）
    readonly property var subItems: {
        if (root.subIndex < 0 || root.subIndex >= items.length) return []
        var sub = items[root.subIndex].submenu
        return typeof sub === "function" ? sub() : (sub || [])
    }

    /// 每次打开都从收起状态开始（不记忆上次展开的子菜单）。
    onAboutToShow: { subOpen = false; subIndex = -1 }
    /// 展开二级面板时 panel 可能比主菜单高：把整个菜单往上推，避免面板出屏。
    onSubOpenChanged: {
        if (subOpen && parent)
            root.y = Math.max(4, Math.min(root.y, parent.height - root.height - 4))
    }

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
        // 主菜单底（高度 = 主菜单自身，不随 popup 长高拉伸）
        Rectangle {
            width: root.menuWidth
            height: root.mainPanelH
            color: "#d9111111"; radius: 8
            border { width: 1; color: "#22ffffff" }
        }
        // 二级面板底（比条目行上下各多 6px；高度 = 可视高度，内部可滚动）
        Rectangle {
            visible: root.subOpen
            x: root.menuWidth + root.subMenuGap
            y: root.subY - 6
            width: root.subMenuWidth
            height: root.subViewH + 12
            color: "#d9111111"; radius: 8
            border { width: 1; color: "#22ffffff" }
        }
    }

    // ── 二级面板的一行（分组标题 / 分隔线 / 普通项 / 动作项）──────────────
    component SubRow: Item {
        id: subRow
        property var row
        signal activated()
        readonly property bool isSep: row.sep === true
        readonly property bool isHeader: row.header !== undefined
        readonly property bool cur: root.isCurrentSubRow(row)
        width: parent ? parent.width : 0
        height: isSep ? 1 : (isHeader ? root.rowH - 4 : root.rowH)
        Rectangle {
            anchors.fill: parent
            color: !isHeader && !isSep && rowHover.containsMouse ? "#33ffffff" : "transparent"
        }
        Rectangle {
            visible: isSep
            anchors { left: parent.left; leftMargin: 8; right: parent.right; rightMargin: 8 }
            height: 1; color: "#22ffffff"
        }
        Text {
            anchors { left: parent.left; leftMargin: 12; right: parent.right
                      rightMargin: subRow.cur ? 28 : 12
                      verticalCenter: parent.verticalCenter }
            text: isSep ? "" : (isHeader ? row.header : (row.label || ""))
            color: isHeader ? "#808080" : (subRow.cur ? "#ffcc00" : "#e0e0e0")
            font.pixelSize: 13
            elide: Text.ElideRight
            visible: !isSep
        }
        Text {
            anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
            font.family: iconFont; font.pixelSize: 14; text: "󰄬"
            color: "#ffcc00"; visible: subRow.cur
            renderType: Text.NativeRendering
        }
        MouseArea {
            id: rowHover
            anchors.fill: parent
            hoverEnabled: !isHeader && !isSep
            enabled: !isHeader && !isSep
            cursorShape: Qt.PointingHandCursor
            onClicked: subRow.activated()
        }
    }

    contentItem: Item {
        implicitWidth: root.menuWidth
        // 二级面板比主菜单高时，popup 也要跟着长高，否则面板被裁
        implicitHeight: Math.max(mainColumn.implicitHeight + 2 * root.padY,
                                 root.subOpen ? root.subY + root.subViewH + root.padY : 0)

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
                    property bool isHeader: modelData.header !== undefined
                    property bool isControl: modelData.control === "delay"
                    property bool hasSub: modelData.submenu !== undefined
                    property var hintSrc: modelData.hint
                    property string hintText: typeof hintSrc === "function" ? hintSrc() : (hintSrc || "")
                    property bool shown: root.itemShown(modelData)
                    // 供 subY 用实际委托位置对齐二级面板（见 subY 注释）
                    readonly property int itemIndex: index
                    width: parent.width
                    height: !shown ? 0 : (isSep ? 1 : (isHeader ? root.rowH - 4 : root.rowH))
                    visible: shown

                    Rectangle {
                        anchors.fill: parent
                        color: !isSep && !isHeader && !isControl && miHover.containsMouse
                               ? "#33ffffff" : "transparent"
                        visible: !isSep
                    }
                    // 分隔线（sep 行）
                    Rectangle {
                        anchors { left: parent.left; leftMargin: 10; right: parent.right; rightMargin: 10 }
                        height: 1; color: "#22ffffff"; visible: isSep
                    }
                    // 分组上方的细线（与二级面板"两组之间"的分隔线同款；
                    // 第一个条目不加，避免贴边线）
                    Rectangle {
                        visible: isHeader && index > 0
                        anchors { left: parent.left; leftMargin: 10; right: parent.right; rightMargin: 10; top: parent.top }
                        height: 1; color: "#22ffffff"
                    }
                    // 一级分组标题（与二级面板的分组标题同款：灰字、不可点）
                    Text {
                        anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                        visible: isHeader
                        text: modelData.header || ""
                        color: "#808080"
                        font.pixelSize: 13
                    }
                    Row {
                        anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                        spacing: 8
                        visible: !isSep && !isHeader
                        Text { font.family: iconFont; font.pixelSize: 16; text: modelData.icon || ""
                            color: "#e0e0e0"; visible: (modelData.icon || "") !== ""
                            renderType: Text.NativeRendering }
                        Text { text: modelData.text || ""; color: "#e0e0e0"; font.pixelSize: 13 }
                    }
                    // ── 延迟控件行：[−] 值 [+] │ 重置 ─────────────────────
                    // 点按钮**不关菜单**（可连点）；写入在 PlayerViewModel 里
                    // 按 500ms debounce 合并，避免音频反复重同步。
                    Row {
                        id: delayCtl
                        visible: isControl
                        anchors { right: parent.right; rightMargin: 10
                                  verticalCenter: parent.verticalCenter }
                        spacing: 2
                        Item {   // −
                            width: 22; height: 22
                            Text {
                                anchors.centerIn: parent
                                font.family: iconFont; font.pixelSize: 14; text: "󰍴"
                                color: "#e0e0e0"; renderType: Text.NativeRendering
                            }
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: root.delayNudge(modelData.kind, -0.05) }
                        }
                        Text {
                            width: 64; horizontalAlignment: Text.AlignHCenter
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.delayText(root.delayValue(modelData.kind))
                            color: root.delayValue(modelData.kind) !== 0 ? "#ffcc00" : "#e0e0e0"
                            font.pixelSize: 13
                        }
                        Item {   // +
                            width: 22; height: 22
                            Text {
                                anchors.centerIn: parent
                                font.family: iconFont; font.pixelSize: 14; text: "󰐕"
                                color: "#e0e0e0"; renderType: Text.NativeRendering
                            }
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: root.delayNudge(modelData.kind, 0.05) }
                        }
                        Rectangle {   // 分隔线
                            width: 1; height: 16; color: "#0fffffff"
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Item {   // 重置（为 0 时置灰不可点）
                            width: 22; height: 22
                            readonly property bool canReset: root.delayValue(modelData.kind) !== 0
                            Text {
                                anchors.centerIn: parent
                                font.family: iconFont; font.pixelSize: 14; text: "󰦛"
                                color: parent.canReset ? "#e0e0e0" : "#808080"
                                renderType: Text.NativeRendering
                            }
                            MouseArea { anchors.fill: parent
                                cursorShape: parent.canReset ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: if (parent.canReset) root.delayReset(modelData.kind) }
                        }
                    }
                    // 右侧：hint（键位/当前档位）+ 二级菜单箭头
                    Row {
                        anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                        spacing: 4
                        visible: !isSep && !isHeader && !isControl
                        Text {
                            text: hintText; color: "#808080"; font.pixelSize: 13
                            visible: hintText !== ""
                            anchors.verticalCenter: parent.verticalCenter
                            // 上限 = 菜单宽度一半，超出省略（防长值撑出菜单边界）
                            width: Math.min(implicitWidth, root.menuWidth * 0.5)
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignRight
                        }
                        Text { font.family: iconFont; font.pixelSize: 14; text: "󰅂"
                            color: "#808080"; visible: hasSub
                            renderType: Text.NativeRendering
                            anchors.verticalCenter: parent.verticalCenter }
                    }
                    MouseArea {
                        id: miHover
                        anchors.fill: parent
                        hoverEnabled: !isHeader && !isControl
                        enabled: !isHeader && !isControl
                        visible: !isSep
                        cursorShape: Qt.PointingHandCursor
                        // 悬停即展开本条目的二级菜单；移到无子菜单的条目上收起
                        onEntered: {
                            if (hasSub) { root.subIndex = index; root.subOpen = true }
                            else { root.subIndex = -1; root.subOpen = false }
                        }
                        onClicked: {
                            if (hasSub) { root.subIndex = index; root.subOpen = true }
                            else { root.close(); root[modelData.sig]() }
                        }
                    }
                }
            }
        }

        // ── 二级菜单面板 ────────────────────────────────────────────────
        // 「附近字幕」「字幕轨道」是**固定高度的滚动区**（用户 2026-11-02 要求）：
        // 内容超出时在区顶/区底显示上下小三角（可点，翻一行；灰=已到头）。
        // 区内的滚轮由 Flickable 自己处理；面板本身不再整体滚动。
        Item {
            id: subPanel
            visible: root.subOpen
            x: root.menuWidth + root.subMenuGap
            y: root.subY
            width: root.subMenuWidth
            height: root.subViewH

            Column {
                id: subColumn
                width: parent.width
                Repeater {
                    model: root.subGroups
                    delegate: Item {
                        id: groupItem
                        width: subColumn.width
                        height: root.groupHeight(modelData)
                        readonly property var gitems: modelData.items
                        readonly property bool scrolls: root.groupScrolls(modelData)
                        readonly property int maxRows: modelData.maxRows

                        // ── 非滚动组：逐行（分隔线/标题/普通项）──────────────
                        Column {
                            visible: !groupItem.scrolls
                            width: parent.width
                            Repeater {
                                model: groupItem.gitems
                                delegate: SubRow {
                                    row: modelData
                                    onActivated: root.activateSubRow(row)
                                }
                            }
                        }

                        // ── 滚动组：▲ + 固定高度 Flickable + ▼ ────────────────
                        Column {
                            visible: groupItem.scrolls
                            width: parent.width
                            spacing: 0
                            Item {   // 上小三角
                                width: parent.width; height: root.triH
                                Text {
                                    anchors.centerIn: parent
                                    font.family: iconFont; font.pixelSize: 12
                                    text: "󱨊"; renderType: Text.NativeRendering
                                    color: gFlick.contentY > 0
                                           ? (upTri.containsMouse ? "#e0e0e0" : "#b0b0b0")
                                           : "#505050"
                                }
                                MouseArea {
                                    id: upTri
                                    anchors.fill: parent; hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: gFlick.contentY = Math.max(0, gFlick.contentY - root.rowH)
                                }
                            }
                            Flickable {
                                id: gFlick
                                width: parent.width
                                height: groupItem.maxRows * root.rowH
                                contentWidth: width
                                contentHeight: rowsCol.height
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds
                                interactive: true
                                Column {
                                    id: rowsCol
                                    width: gFlick.width
                                    Repeater {
                                        model: groupItem.gitems
                                        delegate: SubRow {
                                            row: modelData
                                            onActivated: root.activateSubRow(row)
                                        }
                                    }
                                }
                            }
                            Item {   // 下小三角
                                width: parent.width; height: root.triH
                                Text {
                                    anchors.centerIn: parent
                                    font.family: iconFont; font.pixelSize: 12
                                    text: "󱨉"; renderType: Text.NativeRendering
                                    color: gFlick.contentY < gFlick.contentHeight - gFlick.height - 1
                                           ? (downTri.containsMouse ? "#e0e0e0" : "#b0b0b0")
                                           : "#505050"
                                }
                                MouseArea {
                                    id: downTri
                                    anchors.fill: parent; hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: gFlick.contentY =
                                        Math.min(gFlick.contentHeight - gFlick.height,
                                                 gFlick.contentY + root.rowH)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
