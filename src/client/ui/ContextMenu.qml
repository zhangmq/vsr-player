import QtQuick
import QtQuick.Controls
import "components"

/// 右键菜单（播放器惯例）。复刻 PopupBase 外观；定位 = 鼠标位置
///（anchorTarget 语义不适用），宿主席位 showAt() 后 open()。
/// 条目可带 `submenu: [{label, value}]`（反交错 / 旋转 / 3D / 轨道 / 网络流）
/// → 悬停/点击在右侧展开二级面板，选中后关闭整个菜单。多个二级菜单共用同
/// 一个面板：展开哪一条由**当前行**决定（currentIndex，鼠标/键盘同源）。
///
/// `submenu` 可以是数组，也可以是**函数**（返回数组）——轨道/网络流是动态的，
/// 用函数可以让面板每次展开时取最新值，而不必重建整个 items。
///
/// ⚠ 二级面板是**独立的子 Popup**（2026-11-28 方案 A），不再和主菜单共用
/// 一个 popup。共用时为了让面板不被裁掉，popup 要加宽到 `主菜单 + 面板`，
/// 于是面板上方/下方的空白区域落在 **popup 内部**：`CloseOnPressOutside`
/// 不触发、也没有行接住这次按压 → 点空白处菜单不关（用户报的 bug；是否复现
/// 取决于二级菜单的行数和从哪一行展开，看起来像"偶发"）。
/// 拆成两个 popup 后主菜单的几何就是它自己，面板外的空白天然是"popup 之外"。
/// ⚠ 子面板的 closePolicy 必须带 `CloseMultiple`：Qt 默认只让**最上层**的
/// popup 吃到"点击外部"，没有它时子面板关掉后主菜单会留在原处
///（Qt 6.12 `QQuickOverlay::childMouseEventFilter` 的 close cascade）。
/// 子面板 `focus: false`：键盘仍归主菜单的 contentItem（方向键/Enter/Esc）。
/// 子面板的 `parent` 是主菜单的 contentItem，`CloseOnPressOutsideParent` 因此
/// 表示"按在主菜单里不关面板"——点主菜单换行时面板不会先闪掉。
PopupBase {
    id: root

    padding: 0
    /// 主菜单宽度：二级面板是独立 popup，不再算进这里（见文件头）
    width: menuWidth
    /// 主菜单自身高度（= popup 高度）。二级面板的高度按它钳制（subY），
    /// 于是面板永远落在主菜单竖直范围内 —— 与"面板画在同一个 popup 里"时
    /// 的排版完全一致，不引入新的视觉变化。
    readonly property real mainPanelH: mainColumn
        ? mainColumn.implicitHeight + 2 * padY : 0
    /// 主菜单内容总高：布局还没完成（首次打开）时用行模型估算，二者取大。
    readonly property real mainContentH: Math.max(root.estimatedPanelH(), root.mainPanelH)
    /// 主菜单可视高度上限 = 窗口内留 4px 边距；超过就**滚动**而不是被窗口裁掉
    ///（用户 2026-11-28：二级面板已经"超出可滚"，一级菜单也要能全部看到）。
    readonly property real mainMaxH: parent ? Math.max(root.rowH * 3, parent.height - 8) : 0
    readonly property bool mainScrolls: root.mainMaxH > 0 && root.mainContentH > root.mainMaxH
    /// popup 高度 = 滚动后的可视高度（不再等于内容高度）
    readonly property real mainViewH: root.mainScrolls ? root.mainMaxH : root.mainContentH

    /// 英文界面下 "Add folder to playlist" + "Ctrl+Shift+O"、
    /// "Subtitle delay" + 延迟控件 会顶到一起 → 230 不够（2026-11-02 实测重叠）
    readonly property int menuWidth: 268
    /// 二级面板：英文条目（"Remove external subtitle"）与网络流标题都要放得下
    readonly property int subMenuWidth: 268
    readonly property int subMenuGap: 4
    readonly property int rowH: 30
    readonly property int padY: 8

    // ── 当前行状态（鼠标/键盘共用；鼠标只是改变当前行的手段之一）──────
    /// 主菜单当前行（-1 = 无：指针在标题/分隔线上，或刚打开菜单）
    property int currentIndex: -1
    /// 二级面板当前行（面板可见时才有意义）
    property int subCurrentIndex: -1
    /// 二级面板是否展开：当前行有二级菜单就展开，移到没有的行/标题/分隔线就收起。
    /// 这是面板唯一的开关状态（鼠标/键盘/Esc 都只改它，子 popup 由下面的
    /// onSubVisibleChanged 跟着开合）。
    property bool subVisible: false
    /// Esc 由 handleKey 两段式处理（先收面板、再关菜单），不让 Popup 直接关
    closePolicy: Popup.CloseOnPressOutside
    /// 打开时抢键盘焦点：否则方向键/Enter 会被窗口级 Keys/Shortcut 吃掉
    focus: true
    /// 键盘焦点是否在二级面板内（决定 ↑↓←→/Enter 作用在哪个面板）
    property bool focusSub: false
    /// 键盘焦点是否在「字幕延迟」「音频延迟」的 −/+/↺ 控件上（Enter 进入）
    property bool focusControl: false
    /// 指针上次的窗口坐标：只有**指针真的移动**才改当前行——固定高度列表
    /// 滚动时指针底下的行会变，但指针没动，不该把当前行夺走（滚轮不参与）。
    property point lastPointerPos: Qt.point(-1, -1)

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
    /// 右键菜单「界面」二级项：翻转某个界面元素（0 = 缩略图）。
    signal uiElementToggled(int id)
    signal titlePicked(int id)               // disc title (mpv edition, not a track)
    signal chapterPicked(int index)          // chapter index, 0-based
    signal discMenuBootToggled()             // Preferences -> Disc: flip boot-into-menu
    /// 「关于」弹窗（图标 / 名称 / 版本 / 许可 / 仓库）。
    signal aboutRequested()
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
        // ── 文件（打开 / 删除；用户 2026-11-15 定的分组）────────────────
        {header: qsTr("File")},
        {icon: "󰉋", text: qsTr("Open…"),
         submenu: [
             {label: qsTr("File"),   hint: "Ctrl+O",       sig: "openFilesRequested"},
             {label: qsTr("Folder"), hint: "Ctrl+Shift+O", sig: "openFolderRequested"},
             {label: qsTr("URL"),    hint: "Ctrl+L",       sig: "openUrlRequested"}
         ]},
        {icon: "󰆴", text: qsTr("Delete file"), hint: "Shift+Del", sig: "deleteFileRequested",
         enabledWhen: function() { return viewModel.hasMedia && viewModel.canDeleteCurrentFile }},

        // ── 播放列表（添加 / 移除）──────────────────────────────────────
        {sep: true},
        {header: qsTr("Playlist")},
        {icon: "󰐒", text: qsTr("Add…"),
         submenu: [
             {label: qsTr("Folder"), sig: "appendFolderRequested"},
             {label: qsTr("File"),   sig: "appendFilesRequested"}
         ]},
        {icon: "󰐓", text: qsTr("Remove from playlist"), hint: "Del",
         sig: "removeFromPlaylistRequested",
         enabledWhen: function() { return viewModel.hasMedia }},

        // ── 首选项（可 toggle 的界面元素；当前只有缩略图）────────────────
        {sep: true},
        {header: qsTr("Preferences")},
        {icon: "󰨇", text: qsTr("Interface"),
         submenu: function() { return root.interfaceMenu() }},
        {icon: "󰥠", text: qsTr("Disc"),
         submenu: function() { return root.discPrefsMenu() }},

        // ── 轨道 / 流（按文件；无媒体时整块隐藏）──────────────────────
        {sep: true},
        {header: qsTr("Tracks"),
         visibleWhen: function() { return !root.hideInvalid || root.hasTracks() }},
        // 轨道/流入口**不显示当前值**：轨道名/流标题都很长（实测 39 条轨道、
        // "1080P 高清 · AV1 · 1.9M" 这类），在 230px 宽的菜单里会盖住条目名。
        // 当前值在二级面板里用勾选表达。
        // 「视频」= 网络流（选哪条码流）+ 视频轨道（当前码流内的第几条轨）。
        // 两者是**不同层级**：edition 决定源（分辨率/编码/码率），vid 只是源内的轨。
        // 网络视频每个 edition 恒 1 条视频轨，本地多视频轨 MKV 才有第二段。
        {icon: "󰕧", text: qsTr("Video"),
         submenu: function() { return root.videoMenu() },
         enabledWhen: function() { return root.hasVideoMenu() }},
        // A disc's titles are mpv editions: neither streams (the "Network stream"
        // section above) nor video tracks. A disc has a single playlist entry, so this
        // is the only place to pick a title (model decided 2026-11-26; upstream 0.42's
        // edition *is* "a title on the disc"). Rows read `Title 6 · 1:18:30`, plus a
        // `Disc menu` entry when the disc supports navigation.
        {icon: "󰿎", text: qsTr("Title"),
         hint: function() { return root.currentDiscTitleLabel() },
         subSig: "titlePicked",
         current: function() { return viewModel.currentEdition },
         submenu: function() { return root.discTitlesMenu() },
         enabledWhen: function() { return root.hasDiscTitles() }},
        // Chapters: a mirror of mpv's chapter-list (= upstream menu.conf's $chapters).
        // A disc title often has a dozen or more of them (the test disc's main title has
        // 16), and without a list the only way in is the seek bar. Row labels use the
        // chapter name when the disc/container provides one, otherwise index + start.
        {icon: "󰉻", text: qsTr("Chapter"),
         hint: function() { return root.currentChapterLabel() },
         subSig: "chapterPicked",
         current: function() { return root.currentChapterIndex() },
         submenu: function() { return root.chaptersMenu() },
         enabledWhen: function() { return viewModel.chapters.length > 0 }},
        {icon: "󰝚", text: qsTr("Audio"),
         subSig: "audioTrackPicked",
         current: function() { return root.selectedTrackId("audio") },
         submenu: function() { return root.audioTracksMenu() },
         enabledWhen: function() { return root.hasTracks() }},
        // 字幕**恒显示**（哪怕本片一条字幕轨都没有）：还要能"加载字幕文件…"
        // 和从"附近字幕"里加，所以不能按"有没有字幕轨"来决定是否显示。
        {icon: "󰨖", text: qsTr("Subtitles"),
         subSig: "subtitlePicked",
         current: function() { return root.selectedTrackId("sub") },
         submenu: function() { return root.subtitleMenu() },
         enabledWhen: function() { return root.hasTracks() }},
        {control: "delay", kind: "sub", text: qsTr("Subtitle delay"),
         enabledWhen: function() { return root.hasTracks() }},
        {control: "delay", kind: "audio", text: qsTr("Audio delay"),
         enabledWhen: function() { return root.hasTracks() }},

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
        // 整块受 Preferences → Interface →「Show 3D menu」控制（默认开）；
        // 关掉时 5 行（含首尾分隔线）都不占高度，相邻两组的分隔线由
        // sepShown() 收敛成一条。
        {sep: true, visibleWhen: function() { return viewModel.show3dMenu }},
        {header: qsTr("3D"), visibleWhen: function() { return viewModel.show3dMenu }},
        {icon: "", text: qsTr("Output mode"),
         hint: function() { return root.stereoModeLabel() },
         subSig: "stereoSelected",
         visibleWhen: function() { return viewModel.show3dMenu },
         current: function(g) { return g === 0 ? root.stereoOutputMode() : -1 },
         submenu: [
             {label: qsTr("Off"),        value: 0, group: 0},
             {label: qsTr("3D glasses"), value: 1, group: 0},
             {label: qsTr("2D output"),  value: 2, group: 0}
         ]},
        {icon: "", text: qsTr("Source layout"),
         hint: function() { return root.layoutLabel() },
         subSig: "stereoSelected",
         visibleWhen: function() { return viewModel.show3dMenu },
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
         visibleWhen: function() { return viewModel.show3dMenu },
         current: function(g) { return g === 2 ? viewModel.eyePref : -1 },
         submenu: [
             {label: qsTr("Left eye"),  value: 0, group: 2},
             {label: qsTr("Right eye"), value: 1, group: 2}
         ]},
        {sep: true, visibleWhen: function() { return viewModel.show3dMenu }},
        {icon: "󰋽", text: qsTr("About"),              sig: "aboutRequested"},
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

    /// 「界面」二级面板：可 toggle 的界面元素（当前只有缩略图）。
    /// toggle 型行 = 点一下翻转、菜单保持打开；勾选态由 row.toggle 决定
    ///（`isCurrentSubRow` 已支持）。subSig 收到 value 作为元素 id。
    function interfaceMenu() {
        return [
            {label: qsTr("Thumbnails"),
             toggle: viewModel.previewThumbnails,
             subSig: "uiElementToggled", value: 0},
            {label: qsTr("Hide unavailable menu items"),
             toggle: viewModel.menuHideInvalid,
             subSig: "uiElementToggled", value: 1},
            // 一级菜单里的「3D」块（输出方式 / 片源格式 / 输出选项）在不用 3D
            // 时是纯噪音（用户 2026-11-28："右键菜单有些臃肿"）→ 可关。
            {label: qsTr("Show 3D menu"),
             toggle: viewModel.show3dMenu,
             subSig: "uiElementToggled", value: 2}
        ]
    }

    /// Preferences -> Disc: a single boolean, so one toggle row (same shape as the
    /// thumbnail switch under Interface: a click flips it and the menu stays open).
    function discPrefsMenu() {
        return [
            {label: qsTr("Boot into disc menu"),
             toggle: viewModel.discMenuBoot,
             subSig: "discMenuBootToggled"}
        ]
    }

    /// 本片是不是"盘"：edition-list 代表盘上的 title，而不是网络码流。
    function hasDiscTitles() {
        return viewModel.discSource && viewModel.editions.length > 1
    }

    /// mpv labels disc titles as `title: 6 (01:18:30.539) (00003.mpls)`; when the disc
    /// supports navigation demux_disc also appends a synthetic `Disc Menu` (no duration
    /// and no playlist).
    function discTitleLabel(title) {
        var t = String(title || "")
        if (t === "Disc Menu") return qsTr("Disc menu")
        var m = t.match(/^title:\s*(\d+)\s*\((\d+):(\d+):(\d+)/)
        // 小时去掉前导零（mpv 的标签是 "01:18:30"），分钟/秒保持两位
        if (m) return qsTr("Title %1 · %2:%3:%4").arg(m[1])
                    .arg(String(parseInt(m[2], 10))).arg(m[3]).arg(m[4])
        return t
    }

    /// Current title shown as the hint of the Title row (no mpls number)
    function currentDiscTitleLabel() {
        var eds = viewModel.editions
        for (var i = 0; i < eds.length; i++)
            if (eds[i].id === viewModel.currentEdition)
                return root.discTitleLabel(eds[i].title)
        return qsTr("Auto")
    }

    /// 「Title」二级面板：盘上的 title 列表（= mpv 的 edition-list）
    function discTitlesMenu() {
        var out = []
        var eds = viewModel.editions
        for (var i = 0; i < eds.length; i++)
            out.push({label: root.discTitleLabel(eds[i].title),
                      value: eds[i].id,
                      subSig: "titlePicked",
                      current: eds[i].id === viewModel.currentEdition,
                      section: "titles", sectionRows: root.trackSectionRows})
        return out
    }

    /// 当前章节序号（按 currentTime 落在哪个章节起点之后；无章节 → -1）
    function currentChapterIndex() {
        var ch = viewModel.chapters
        if (!ch || ch.length === 0) return -1
        var t = viewModel.currentTime / 1000
        var idx = 0
        for (var i = 0; i < ch.length; i++) {
            if (ch[i].time <= t + 0.05) idx = i
            else break
        }
        return idx
    }

    function chapterTimeLabel(sec) {
        var s = Math.max(0, Math.floor(sec))
        var h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), ss = s % 60
        var p2 = function(v) { return (v < 10 ? "0" : "") + v }
        return h > 0 ? h + ":" + p2(m) + ":" + p2(ss) : m + ":" + p2(ss)
    }

    /// Chapter row label: the container/disc name when there is one, else
    /// "Chapter N" + start time
    function chapterLabel(index) {
        var ch = viewModel.chapters
        if (!ch || index < 0 || index >= ch.length) return ""
        var name = String(ch[index].title || "")
        if (name.length === 0) name = qsTr("Chapter %1").arg(index + 1)
        return name + " · " + root.chapterTimeLabel(ch[index].time)
    }

    function currentChapterLabel() {
        var i = root.currentChapterIndex()
        return i < 0 ? "" : root.chapterLabel(i)
    }

    /// 「章节」二级面板（固定高度滚动区；长片几十章也只在区内滚）
    function chaptersMenu() {
        var out = []
        var ch = viewModel.chapters
        var cur = root.currentChapterIndex()
        for (var i = 0; i < ch.length; i++)
            out.push({label: root.chapterLabel(i), value: i, subSig: "chapterPicked",
                      current: i === cur,
                      section: "chapters", sectionRows: root.trackSectionRows})
        return out
    }

    /// 「视频」二级面板：网络流 + 视频轨道两段（各自固定高度滚动区）。
    /// 行自带 `current`（勾选）与 `subSig`（点它调哪个信号）——两段语义不同，
    /// 不能再共用父条目的 subSig。
    function videoMenu() {
        var out = []
        if (root.hasStreamChoices()) {
            out.push({header: qsTr("Network stream")})
            var eds = viewModel.editions
            for (var i = 0; i < eds.length; i++)
                out.push({label: root.editionLabel(eds[i].title),
                          value: eds[i].id,
                          subSig: "editionPicked",
                          current: eds[i].id === viewModel.currentEdition,
                          section: "editions", sectionRows: root.editionSectionRows})
        }
        var ts = root.tracksOf("video")
        if (ts.length > 1) {          // 单视频轨的文件没必要占一行
            if (out.length > 0) out.push({sep: true})
            out.push({header: qsTr("Video tracks")})
            out.push({label: qsTr("Auto"), value: -2, subSig: "videoTrackPicked",
                      section: "vtracks", sectionRows: root.trackSectionRows})
            for (var j = 0; j < ts.length; j++)
                out.push({label: root.trackLabel(ts[j]), value: ts[j].id,
                          subSig: "videoTrackPicked",
                          current: ts[j].id === root.selectedTrackId("video"),
                          section: "vtracks", sectionRows: root.trackSectionRows})
        }
        return out
    }

    /// Whether the Video entry is shown: there are streams to pick (network source) or
    /// this file has more than one video track
    function hasVideoMenu() {
        return root.hasStreamChoices() || root.tracksOf("video").length > 1
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

    /// Real **selectable streams** (one edition per ytdl format).
    /// ⚠ A disc's edition-list is its title list, not streams: those used to be listed
    /// under "Network stream" in the Video panel (reported 2026-11-26: disc titles
    /// showed up in the video-track panel with a "Network stream" heading). Same for
    /// local files with several editions (mpv has no edition menu of its own either).
    function hasStreamChoices() {
        return !viewModel.discSource && viewModel.networkSource && root.hasEditions()
    }

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

    // ── 延迟（字幕 / 音频）：±50ms，显示统一用毫秒 ──────────────────────
    function delayValue(kind) {
        return kind === "audio" ? viewModel.audioDelay : viewModel.subDelay
    }
    /// 取整到毫秒（显示与"是否为 0"的判定都用它，避免浮点残值导致显示 0ms 却仍高亮）
    function delayMs(kind) {
        return Math.round(root.delayValue(kind) * 1000) | 0
    }
    function delayText(sec) {
        // |0 把 -0 归一成 0（否则会显示 "-0ms"）
        var ms = Math.round(sec * 1000) | 0
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
        if (it.visibleWhen !== undefined && !it.visibleWhen()) return false
        // 「隐藏不可用项」打开时退回旧行为：不可用 = 直接不显示（不占高度）
        if (root.hideInvalid && it.enabledWhen !== undefined && !it.enabledWhen())
            return false
        return true
    }

    /// 「隐藏无效菜单项」开关（用户 2026-11-15：界面菜单第二个 toggle）。
    /// 默认 false = 当前上下文用不了的项**灰显占位**；true = 直接隐藏。
    /// 用词"无效"（此刻不成立）——用户定的口径，宁可长一点也要准确。
    readonly property bool hideInvalid: viewModel.menuHideInvalid

    /// 条目是否可用（`enabledWhen`）。不可用 = **仍然显示**但置灰、不参与
    /// 悬停/键盘/点击——用户 2026-11-15 要求"没有目标时禁用而不是隐藏"
    /// （隐藏会让菜单结构随内容跳动；禁用还顺带告诉用户这里有什么能力）。
    function itemEnabled(it) {
        if (it.enabledWhen === undefined) return true
        return root.hideInvalid ? true : it.enabledWhen()
    }

    /// 分隔线是否画：向上找第一个可见条目，如果它也是分隔线就不画。
    /// 否则两条 1px 会叠成 2px（用户 2026-11-02 报"分割线从上到下越来越细"：
    /// 相邻两组之间的行被 visibleWhen 隐藏时就会发生）。
    function sepShown(idx) {
        for (var i = idx - 1; i >= 0; i--) {
            if (!root.itemShown(root.items[i])) continue
            return root.items[i].sep !== true
        }
        return false
    }

    /// 二级面板内容行数 → 高度（分组标题行矮 4px，分隔线 1px）
    function subRowH(item) {
        if (item.sep === true) return 1
        return item.header !== undefined ? root.rowH - 4 : root.rowH
    }

    // ── 当前行：查询 / 移动 / 激活 ────────────────────────────────
    /// 这一行是否可作为当前行（跳过隐藏行、标题、分隔线）
    function rowFocusable(i) {
        var it = root.items[i]
        if (!it) return false
        if (it.sep === true || it.header !== undefined) return false
        return root.itemShown(it) && root.itemEnabled(it)
    }
    /// 这一行是「字幕延迟」/「音频延迟」（行内有 −/+/↺ 控件）
    function isDelayRow(i) {
        var it = root.items[i]
        return !!it && it.control === "delay"
    }
    function hasSubRow(i) {
        var it = root.items[i]
        return !!it && it.submenu !== undefined
    }

    /// 指针真的移动了吗？内容滚动引起的 hover 变化不算。
    function pointerMoved(pos) {
        if (root.lastPointerPos.x === pos.x && root.lastPointerPos.y === pos.y)
            return false
        root.lastPointerPos = Qt.point(pos.x, pos.y)
        return true
    }

    /// 设置主菜单当前行。
    /// openPanel=true（鼠标 hover）→ 有二级就展开；false（键盘 ↑↓）→ 不自动展开，
    /// 由 →/Enter 显式打开（用户 2026-11-02：←/→ 必须有可见效果）。
    function setCurrentRow(i, openPanel) {
        if (i === root.currentIndex) return
        root.currentIndex = i
        root.focusSub = false
        root.focusControl = false
        if (root.hasSubRow(i) && openPanel === true) {
            root.subVisible = true
            root.subCurrentIndex = root.initialSubRow()
        } else {
            root.subVisible = false
            root.subCurrentIndex = -1
        }
    }

    /// 主菜单键盘移动（跳过不可聚焦行；循环）
    function moveCurrent(delta) {
        var n = root.items.length
        if (n === 0) return
        var i = root.currentIndex
        for (var k = 0; k < n; k++) {
            i = (i + delta + n) % n
            if (root.rowFocusable(i)) { root.setCurrentRow(i, false); return }
        }
    }

    function firstFocusableRow(fromEnd) {
        for (var k = 0; k < root.items.length; k++) {
            var i = fromEnd ? root.items.length - 1 - k : k
            if (root.rowFocusable(i)) return i
        }
        return -1
    }

    /// 二级面板：行是否可聚焦 / 初始行（落在已勾选项上，否则第一个可聚焦行）
    function subRowFocusable(j) {
        var it = root.subItems[j]
        if (!it) return false
        return it.sep !== true && it.header === undefined
    }
    function initialSubRow() {
        var m = root.subItems
        for (var j = 0; j < m.length; j++)
            if (root.isCurrentSubRow(m[j]) && root.subRowFocusable(j)) return j
        for (var k = 0; k < m.length; k++)
            if (root.subRowFocusable(k)) return k
        return -1
    }
    function moveSub(delta) {
        var m = root.subItems
        if (m.length === 0) return
        var j = root.subCurrentIndex
        for (var k = 0; k < m.length; k++) {
            j = (j + delta + m.length) % m.length
            if (root.subRowFocusable(j)) { root.subCurrentIndex = j; return }
        }
    }

    /// Enter：面板内执行当前行；主菜单有二级则进面板，否则执行动作项
    ///（「字幕延迟」「音频延迟」没有下级，Enter 不做事——方向键直接调值）
    function activateCurrent() {
        if (root.focusSub) {
            var row = root.subItems[root.subCurrentIndex]
            if (row) root.activateSubRow(row)
            return
        }
        var i = root.currentIndex
        if (i < 0) return
        // 「字幕延迟」/「音频延迟」：Enter 进/出 −/+/↺ 控件态，
        // 之后 ←/→ 调值（±50ms），Esc/Enter/↑↓ 退出
        if (root.isDelayRow(i)) {
            root.focusControl = !root.focusControl
            return
        }
        if (root.hasSubRow(i)) {
            root.subVisible = true
            root.focusSub = true
            root.subCurrentIndex = root.initialSubRow()
            return
        }
        var it = root.items[i]
        if (it.sig) { root.close(); root[it.sig]() }
    }

    /// ←/→ 只做导航（用户 2026-11-02：方向键不做执行）：
    /// ← 从面板退回主菜单；→ 进入当前行的二级面板。没有二级的行上两者都不做事
    ///（「字幕延迟」「音频延迟」的 −/+/↺ 是鼠标操作；键盘要调值需另行设计
    /// 行内可聚焦目标，不在本期）。
    function arrowHorizontal(dir) {
        // 行内控件态：←/→ 调值（±50ms）——这是"Enter 后才调值"的那条路径，
        // 平时方向键只导航（用户 2026-11-02 定）
        if (root.focusControl) {
            root.delayNudge(root.items[root.currentIndex].kind, dir * 0.05)
            return
        }
        var i = root.currentIndex
        if (dir < 0) {                      // ←：收起面板（有可见效果），不退到别处
            if (root.subVisible) {
                root.subVisible = false
                root.focusSub = false
                root.subCurrentIndex = -1
            }
            return
        }
        if (dir > 0 && root.hasSubRow(i)) { // →：展开面板并把焦点放进去
            root.subVisible = true
            root.focusSub = true
            root.subCurrentIndex = root.initialSubRow()
        }
    }

    /// 鼠标移动到主菜单某一行（仅指针真的移动时生效）
    function hoverMain(i, pos) {
        if (!root.pointerMoved(pos)) return
        root.focusSub = false
        root.setCurrentRow(root.rowFocusable(i) ? i : -1, true)
    }
    /// 鼠标移动到二级面板某一行
    function hoverSub(j, pos) {
        if (!root.pointerMoved(pos)) return
        if (!root.subRowFocusable(j)) return
        root.subCurrentIndex = j
        root.focusSub = true
    }

    /// 键盘入口（键盘事件与 RPC menu-key 共用）。
    /// 键位不再写在这个文件里：菜单导航键（menu-up/…）与"弹窗内可用动作"
    /// （字幕/音频延迟，键位表里 popupOk 的行）都由 C++ 的同一张表回答，
    /// 查不到就交还给默认处理（2026-11-03 删掉了这里的第二份偏移键映射）。
    function runAction(id) {
        // menu-* = 菜单导航；其余是 C++ 侧动作（KeyRouter::exec）
        if (id.lastIndexOf("menu-", 0) === 0) {
            keys.noteNav(id)          // 记账（RPC key-log 断言用）
            root.navAction(id.substring(5))
            return true
        }
        return keys.trigger(id)
    }

    function keyAction(key, modifiers) {
        var id = keys.menuAction(key, modifiers)
        if (id === "") return false
        return root.runAction(id)
    }

    /// RPC menu-key 入口：nav 名称（up/down/left/right/home/end/enter/esc）
    /// 或动作 id（如 sub-delay-down）
    function handleKey(token) {
        switch (token) {
        case "up": case "down": case "left": case "right":
        case "home": case "end": case "enter": case "esc": case "escape":
            root.navAction(token === "esc" ? "escape" : token)
            return
        }
        root.runAction(token)
    }

    /// 菜单导航（鼠标/键盘共用同一套"当前行"状态）
    /// 键盘把当前行移出可视区时滚动过去（主菜单超出窗口时才有意义）
    function ensureMainRowVisible() {
        if (!root.mainScrolls || !mainFlick || !mainColumn)
            return
        if (root.currentIndex < 0)
            return
        for (var i = 0; i < mainColumn.children.length; i++) {
            var c = mainColumn.children[i]
            if (c.itemIndex !== root.currentIndex)
                continue
            if (c.y < mainFlick.contentY)
                mainFlick.contentY = c.y
            else if (c.y + c.height > mainFlick.contentY + mainFlick.height)
                mainFlick.contentY = c.y + c.height - mainFlick.height
            return
        }
    }
    onCurrentIndexChanged: Qt.callLater(root.ensureMainRowVisible)

    function navAction(nav) {
        switch (nav) {
        case "up":
            root.focusControl = false
            if (root.focusSub) root.moveSub(-1); else root.moveCurrent(-1)
            break
        case "down":
            root.focusControl = false
            if (root.focusSub) root.moveSub(1); else root.moveCurrent(1)
            break
        case "left":  root.arrowHorizontal(-1); break
        case "right": root.arrowHorizontal(1); break
        case "home":
            if (root.focusSub) { root.subCurrentIndex = root.initialSubRow() }
            else root.setCurrentRow(root.firstFocusableRow(false), false)
            break
        case "end":
            if (root.focusSub) {
                var m = root.subItems
                for (var j = m.length - 1; j >= 0; j--)
                    if (root.subRowFocusable(j)) { root.subCurrentIndex = j; break }
            } else root.setCurrentRow(root.firstFocusableRow(true), false)
            break
        case "enter": root.activateCurrent(); break
        case "escape":
            if (root.focusControl) { root.focusControl = false; break }
            if (root.subVisible) {
                root.subVisible = false
                root.focusSub = false
                root.subCurrentIndex = -1
            } else {
                root.close()
            }
            break
        }
    }

    /// 状态快照（RPC get-menu-state / menu-key 回显，验证导航不用截图）
    readonly property string navState: {
        var cur = root.currentIndex >= 0 && root.items[root.currentIndex]
                  ? String(root.items[root.currentIndex].text || "-") : "-"
        var sub = "-"
        if (root.subVisible && root.subCurrentIndex >= 0) {
            var it = root.subItems[root.subCurrentIndex]
            if (it) sub = String(it.label || it.header || "-")
        }
        return "cur=" + root.currentIndex + "(" + cur + ") sub=" + (root.subVisible ? 1 : 0)
               + "[" + root.subCurrentIndex + ":" + sub + "] focusSub=" + (root.focusSub ? 1 : 0)
               + " ctl=" + (root.focusControl ? 1 : 0)
               + " ms=" + root.delayMs("sub") + "/" + root.delayMs("audio")
    }

    /// 二级面板的 y：与所属条目行对齐。
    /// **取委托的实际布局位置**，不再用 items 数组重推行高：隐藏行（visibleWhen，
    /// 例如 URL/直播没有"删除文件"）由 Column 布局决定高度 0，而 items 侧的
    /// visibleWhen 求值时机可能与委托的 shown 不同步——重算会多算/少算隐藏行，
    /// 面板就会整体偏下/偏上（2026-10-25 实测）。
    readonly property real subY: {
        // popup 的 contentItem 是懒创建的：首次打开前 mainColumn 还不存在。
        // 读 mainColumn.height 同时登记"布局变化"依赖（行显示/隐藏时重算）。
        if (!mainColumn || !mainFlick)
            return root.padY
        // 主菜单可视区里"第 0 行"的 popup 坐标：滚动后要减掉 contentY
        var top = root.padY + mainUpTri.height - mainFlick.contentY
        var mh = mainColumn.height
        if (!root.subVisible)
            return top + mh
        var y = top + mh
        for (var i = 0; i < mainColumn.children.length; i++) {
            var c = mainColumn.children[i]
            if (c.itemIndex === root.currentIndex) {
                y = top + c.y
                break
            }
        }
        // 底部钳制：面板保持在**主菜单可视范围**内（子 popup 本身不会被裁，
        // 这里沿用原排版：面板不跑到菜单下边缘之外）。
        var maxY = root.mainViewH - root.subViewH - root.padY
        if (y > maxY)
            y = Math.max(root.padY, maxY)
        return y
    }

    /// 两个滚动区固定显示的行数（用户 2026-11-02：固定高度 + 上下小三角提示）
    readonly property int nearbySectionRows: 4
    readonly property int trackSectionRows: 8
    /// 网络流滚动区显示行数（实测某条 B 站视频 12 条可选）
    readonly property int editionSectionRows: 8
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
                cur = { section: sec, items: [it], maxRows: rows, start: i }
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

    /// 某一行是否是"当前值"（勾选态）。
    /// 行自带 `current` 时优先用它（同一个入口里混了两种语义的分段：网络流/视频轨），
    /// 否则退回父条目的 `current(group)` 老机制（反交错/旋转/3D 那三条）。
    function isCurrentSubRow(row) {
        if (row.toggle !== undefined) return row.toggle === true
        if (row.current !== undefined) return row.current === true
        var it = root.currentIndex >= 0 ? root.items[root.currentIndex] : null
        if (!it || !it.current) return false
        if (row.header !== undefined || row.sep === true || row.sig !== undefined) return false
        return row.value === it.current(row.group || 0)
    }

    /// 点击二级面板里的某一行：动作行发自己的 sig；行自带 subSig 用它；
    /// 否则交给所属条目的 subSig（老机制）。
    function activateSubRow(row) {
        var it = root.currentIndex >= 0 ? root.items[root.currentIndex] : null
        if (row.toggle !== undefined) {
            // toggle 型：翻转后**保持打开**（还要能连点其它界面元素）；
            // 勾选态由 row.toggle 的绑定（viewModel 属性）刷新。
            // 没有 value 的 toggle 行（如 Preferences → Disc）是零参信号，不能硬塞
            // (value, group)：QML 会报 "Too many arguments, ignoring 1"。
            if (row.subSig) {
                if (row.value !== undefined) root[row.subSig](row.value, row.group || 0)
                else root[row.subSig]()
            }
            return
        }
        if (row.sig !== undefined) {
            if (row.arg !== undefined) root[row.sig](row.arg)
            else root[row.sig]()
        } else if (row.subSig) {
            root[row.subSig](row.value, row.group || 0)
        } else if (it && it.subSig) {
            root[it.subSig](row.value, row.group || 0)
        }
        root.subVisible = false
        root.focusSub = false
        root.subCurrentIndex = -1
        root.close()
    }

    /// 二级面板的子项模型（currentIndex 指向的条目；submenu 可为函数）
    readonly property var subItems: {
        if (root.currentIndex < 0 || root.currentIndex >= items.length) return []
        var sub = items[root.currentIndex].submenu
        return typeof sub === "function" ? sub() : (sub || [])
    }

    /// 各滚动区当前 contentY（RPC 验证"当前行已滚进可视区"）
    readonly property var subScrolls: {
        var out = []
        if (!subColumn) return out
        for (var i = 0; i < subColumn.children.length; i++) {
            var g = subColumn.children[i]
            if (g.gstart === undefined) continue
            // 组委托里的 Flickable 没有 id 暴露——用属性登记（见下）
            out.push(g.scrollPos !== undefined ? g.scrollPos : -1)
        }
        return out
    }

    /// 逐行几何快照（RPC get-menu-state 用）：mainColumn 是内部 id，外部
    /// 读不到，这里显式暴露出来，排查"分隔线粗细/行高异常"不用截图。
    readonly property var rowGeom: {
        var out = []
        if (!mainColumn) return out
        for (var i = 0; i < mainColumn.children.length; i++) {
            var c = mainColumn.children[i]
            if (c.itemIndex === undefined) continue
            out.push({ idx: c.itemIndex, y: Math.round(c.y), h: Math.round(c.height),
                       vis: c.visible })
        }
        return out
    }

    /// 每次打开都从收起状态开始（不记忆上次展开的子菜单）。
    onAboutToShow: {
        currentIndex = -1
        subCurrentIndex = -1
        subVisible = false
        focusSub = false
        lastPointerPos = Qt.point(-1, -1)
    }

    /// 打开后让内容项拿到键盘焦点（否则 ↑↓/Enter 收不到）
    onOpened: if (contentRoot) contentRoot.forceActiveFocus()
    /// 二级面板是独立 popup：subVisible 是唯一开关，子 popup 跟着它开合。
    onSubVisibleChanged: {
        if (subVisible) subMenu.open()
        else subMenu.close()
    }
    /// 主菜单关掉时子面板必须一起关：两个 popup 各自注册在窗口 overlay 上，
    /// 父 popup 关闭只解绑自己的 popupItem，不替子面板收尾
    ///（QQuickPopupPrivate::setWindow 只 setParentItem(nullptr)，不 close）。
    onAboutToHide: subMenu.close()
    /// 子面板的 opened 状态（RPC get-menu-state 断言用：两个 popup 是否真的都关了）
    readonly property bool subPopupOpened: subMenu ? subMenu.opened : false
    /// 子面板的场景几何（RPC get-menu-state 用：测试要点面板中央/空白处时
    /// 不用自己按 subY/menuWidth 推算）
    readonly property var subGeom: {
        if (!contentRoot) return ({ x: 0, y: 0, w: 0, h: 0 })
        var p = contentRoot.mapToItem(null, subMenu.x, subMenu.y)
        return ({ x: Math.round(p.x), y: Math.round(p.y),
                  w: Math.round(subMenu.width), h: Math.round(subMenu.height) })
    }

    /// 在 item 坐标系 (x,y)（即鼠标位置）弹出，屏幕边缘钳制。
    /// 横向按**展开二级菜单后的宽度**钳制：面板只在悬停时出现，但这时 popup
    /// 已经加宽，若不预留空间会把面板顶到屏幕外。
    /// 这一次要开在哪（窗口坐标）；钳位可重入，靠它重算。
    property point pendingPos: Qt.point(-1, -1)

    /// 主菜单高度估算（不依赖布局）。首次打开时 Popup 的内容/布局可能还没
    /// 成型，`root.height` 会是 0 —— 用 0 钳位等于没钳，菜单会跑到窗口外被裁
    /// （用户 2026-11-28 报"第一次用鼠标打开不按窗口适配，后续弹出正常"）。
    /// 行高规则与委托一致：分隔线 1、分组标题 rowH-4、其余 rowH；隐藏行不计。
    function estimatedPanelH() {
        var h = 2 * root.padY
        var items = root.items || []
        for (var i = 0; i < items.length; ++i) {
            var it = items[i]
            if (!root.itemShown(it)) continue
            if (it.sep === true) h += 1
            else if (it.header !== undefined) h += root.rowH - 4
            else h += root.rowH
        }
        return h
    }

    /// 把 pendingPos 钳进窗口（"尽可能在窗口内"：菜单比窗口还高时贴顶）。
    function clampedPos() {
        var fullW = root.menuWidth + root.subMenuGap + root.subMenuWidth
        var h = root.height > 0 ? root.height : root.estimatedPanelH()
        return Qt.point(
            Math.min(Math.max(pendingPos.x, 4), Math.max(4, parent.width - fullW - 4)),
            Math.min(Math.max(pendingPos.y, 4), Math.max(4, parent.height - h - 4)))
    }

    /// 首次打开后真实高度才可用时再钳一次（高度没变则不动，避免抖动）。
    function reclamp() {
        if (!root.visible || pendingPos.x < 0 || !parent) return
        var c = root.clampedPos()
        if (Math.abs(c.x - root.x) > 0.5) root.x = c.x
        if (Math.abs(c.y - root.y) > 0.5) root.y = c.y
    }

    function showAt(item, x, y) {
        if (!item || !parent) return
        pendingPos = item.mapToItem(parent, x, y)
        var c = clampedPos()
        root.x = c.x
        root.y = c.y
        open()
        // 内容晚一帧布局时（首次打开）高度才准 → 下一轮事件循环再钳一次
        Qt.callLater(root.reclamp)
    }

    /// 高度变化（首次测量完成 / 行数变化）时重新钳位
    onHeightChanged: root.reclamp()

    background: Rectangle {
        color: "#d9111111"; radius: 8
        border { width: 1; color: "#22ffffff" }
    }

    // ── 二级面板的一行（分组标题 / 分隔线 / 普通项 / 动作项）──────────────
    component SubRow: Item {
        id: subRow
        property var row
        /// 该行在 subItems 里的**全局**下标（分组渲染时 Repeater 的 index 是组内序号，
        /// 不能直接用来比当前行——否则滚动区里的高亮会错位/不显示）
        property int rowIndex: -1
        signal activated()
        readonly property bool isSep: row.sep === true
        readonly property bool isHeader: row.header !== undefined
        readonly property bool cur: root.isCurrentSubRow(row)
        width: parent ? parent.width : 0
        height: isSep ? 1 : (isHeader ? root.rowH - 4 : root.rowH)
        Rectangle {
            anchors.fill: parent
            color: !isHeader && !isSep && root.subVisible && subRow.rowIndex === root.subCurrentIndex
                   ? "#33ffffff" : "transparent"
        }
        Rectangle {
            visible: isSep
            anchors { left: parent.left; leftMargin: 8; right: parent.right; rightMargin: 8 }
            height: 1; color: "#22ffffff"
        }
        // 右侧快捷键提示（「打开…」展开后的 Ctrl+O 等；主菜单同款灰字）
        Text {
            id: subHint
            anchors { right: parent.right; rightMargin: subRow.cur ? 28 : 12
                      verticalCenter: parent.verticalCenter }
            text: (row && row.hint) || ""
            color: "#808080"; font.pixelSize: 13
            visible: !isSep && !isHeader && text !== ""
        }
        Text {
            anchors { left: parent.left; leftMargin: 12
                      right: subHint.visible ? subHint.left : parent.right
                      rightMargin: subHint.visible ? 10 : (subRow.cur ? 28 : 12)
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
        // 悬停：把本行设成面板的当前行（高亮由 subCurrentIndex 驱动，
        // 这样鼠标/键盘是同一套状态；重建 SubRow 时漏过这条 → 悬停没反应）
        HoverHandler {
            enabled: !subRow.isHeader && !subRow.isSep && subRow.rowIndex >= 0
            onPointChanged: root.hoverSub(subRow.rowIndex, point.scenePosition)
        }
        MouseArea {
            id: rowHover
            anchors.fill: parent
            hoverEnabled: false
            enabled: !isHeader && !isSep
            cursorShape: Qt.PointingHandCursor
            onClicked: subRow.activated()
        }
    }

    contentItem: Item {
        id: contentRoot
        focus: true
        Keys.onPressed: function(ev) {
            if (root.keyAction(ev.key, ev.modifiers))
                ev.accepted = true
        }
        implicitWidth: root.menuWidth
        // 主菜单高度 = **可视高度**（内容超出窗口时滚动，见 mainScrolls）。
        implicitHeight: root.mainViewH

        // 可视窗口：内容超出时上下滚动（与二级面板同一套小三角提示）
        Item {
            id: mainView
            x: 0; y: root.padY
            width: root.menuWidth
            height: root.mainViewH - 2 * root.padY

            // ▲ 上滚一行（到底/到顶时灰显，与二级面板一致）
            Item {
                id: mainUpTri
                visible: root.mainScrolls        // 高度 0 时 Text 仍会画出来 → 显式隐藏
                width: parent.width
                height: root.mainScrolls ? root.triH : 0
                Text {
                    anchors.centerIn: parent
                    font.family: iconFont; font.pixelSize: 12
                    text: "󱨊"; renderType: Text.NativeRendering
                    color: mainFlick.contentY > 0
                           ? (upMain.containsMouse ? "#e0e0e0" : "#b0b0b0") : "#505050"
                }
                MouseArea {
                    id: upMain
                    anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: mainFlick.contentY = Math.max(0, mainFlick.contentY - root.rowH)
                }
            }

            Flickable {
                id: mainFlick
                y: mainUpTri.height
                width: parent.width
                height: mainView.height - mainUpTri.height - mainDownTri.height
                contentWidth: width
                contentHeight: mainColumn.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                interactive: true

                Column {
                    id: mainColumn
                    x: 0; y: 0
                    width: mainFlick.width
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
                            property bool shown: isSep ? root.sepShown(index) : root.itemShown(modelData)
                            property bool usable: !isSep && !isHeader && root.itemEnabled(modelData)
                            // 禁用行整体不接输入（enabled 会传播到 HoverHandler/MouseArea/
                            // 延迟控件的 ±/↺），视觉上只置灰、不隐藏
                            enabled: usable
                            // 供 subY 用实际委托位置对齐二级面板（见 subY 注释）
                            readonly property int itemIndex: index
                            width: parent.width
                            height: !shown ? 0 : (isSep ? 1 : (isHeader ? root.rowH - 4 : root.rowH))
                            visible: shown

                            Rectangle {
                                anchors.fill: parent
                                color: !isSep && !isHeader && usable
                                       && index === root.currentIndex && root.currentIndex >= 0
                                       ? "#33ffffff" : "transparent"
                                visible: !isSep
                            }
                            // 分隔线（sep 行）
                            Rectangle {
                                anchors { left: parent.left; leftMargin: 10; right: parent.right; rightMargin: 10 }
                                height: 1; color: "#22ffffff"; visible: isSep
                            }
                            // 一级分组标题（与二级面板的分组标题同款：灰字、不可点）
                            Text {
                                anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                                visible: isHeader
                                text: modelData.header || ""
                                color: "#808080"
                                font.pixelSize: 13
                            }
                            // 左侧标签：右边界锚到右侧控件（hint / 延迟控件）的左缘，
                            // 并按剩余宽度省略——英文文本更长，单靠 menuWidth 兜不住
                            // （2026-11-02 用户报英文界面文本重叠）。
                            Row {
                                id: labelRow
                                anchors {
                                    left: parent.left; leftMargin: 10
                                    right: isControl ? delayCtl.left
                                                     : (hintRow.visible ? hintRow.left : parent.right)
                                    rightMargin: 8
                                    verticalCenter: parent.verticalCenter
                                }
                                spacing: 8
                                visible: !isSep && !isHeader
                                Text {
                                    id: iconText
                                    font.family: iconFont; font.pixelSize: 16
                                    text: modelData.icon || ""
                                    color: usable ? "#e0e0e0" : "#5a5a5a"; visible: text !== ""
                                    renderType: Text.NativeRendering
                                }
                                Text {
                                    text: modelData.text || ""
                                    color: usable ? "#e0e0e0" : "#5a5a5a"; font.pixelSize: 13
                                    width: Math.max(0, labelRow.width -
                                                    (iconText.visible ? iconText.width + labelRow.spacing : 0))
                                    elide: Text.ElideRight
                                }
                            }
                            // ── 延迟控件行：[−] 值 [+] │ 重置 ─────────────────────
                            // 点按钮**不关菜单**（可连点）；写入在 PlayerViewModel 里
                            // 按 500ms debounce 合并，避免音频反复重同步。
                            // 控件态外框（Enter 进入后才有；用项目强调色，不新增样式语言）
                            Rectangle {
                                visible: isControl && root.focusControl && index === root.currentIndex
                                anchors.fill: delayCtl
                                anchors.margins: -4
                                radius: 6
                                color: "transparent"
                                border { width: 1; color: "#ffcc00" }
                            }
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
                                    color: root.delayMs(modelData.kind) !== 0 ? "#ffcc00" : "#e0e0e0"
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
                                    readonly property bool canReset: root.delayMs(modelData.kind) !== 0
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
                                id: hintRow
                                anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                                spacing: 4
                                visible: !isSep && !isHeader && !isControl
                                Text {
                                    text: hintText; color: usable ? "#808080" : "#464646"; font.pixelSize: 13
                                    visible: hintText !== ""
                                    anchors.verticalCenter: parent.verticalCenter
                                    // 上限 = 菜单宽度一半，超出省略（防长值撑出菜单边界）
                                    width: Math.min(implicitWidth, root.menuWidth * 0.5)
                                    elide: Text.ElideRight
                                    horizontalAlignment: Text.AlignRight
                                }
                                Text { font.family: iconFont; font.pixelSize: 14; text: "󰅂"
                                    color: usable ? "#808080" : "#464646"; visible: hasSub
                                    renderType: Text.NativeRendering
                                    anchors.verticalCenter: parent.verticalCenter }
                            }
                            HoverHandler {
                                // 被动观察：只改当前行，不参与点击；指针真的移动才生效
                                onPointChanged: root.hoverMain(index, point.scenePosition)
                            }
                            MouseArea {
                                id: miHover
                                anchors.fill: parent
                                hoverEnabled: false
                                enabled: !isHeader && !isControl
                                visible: !isSep
                                cursorShape: usable ? Qt.PointingHandCursor : Qt.ArrowCursor
                                // 悬停/点击都只是"把当前行设成我"——面板开合由当前行派生
                                //（所以移到「字幕延迟」「音频延迟」上时面板会自然收起）
                                onClicked: {
                                    if (isControl) return
                                    root.setCurrentRow(index)
                                    if (hasSub) {
                                        root.subVisible = true
                                        root.focusSub = true
                                        root.subCurrentIndex = root.initialSubRow()
                                    } else {
                                        var it = root.items[index]
                                        root.close()
                                        if (it && it.sig) root[it.sig]()
                                    }
                                }
                            }
                        }
                    }
                }            }

            // ▼ 下滚一行
            Item {
                id: mainDownTri
                visible: root.mainScrolls
                y: mainFlick.y + mainFlick.height
                width: parent.width
                height: root.mainScrolls ? root.triH : 0
                Text {
                    anchors.centerIn: parent
                    font.family: iconFont; font.pixelSize: 12
                    text: "󱨉"; renderType: Text.NativeRendering
                    color: mainFlick.contentY < mainFlick.contentHeight - mainFlick.height - 1
                           ? (downMain.containsMouse ? "#e0e0e0" : "#b0b0b0") : "#505050"
                }
                MouseArea {
                    id: downMain
                    anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: mainFlick.contentY =
                        Math.min(Math.max(0, mainFlick.contentHeight - mainFlick.height),
                                 mainFlick.contentY + root.rowH)
                }
            }
        }

        // ── 二级菜单面板（独立 popup，见文件头）───────────────────────────
        // ── 二级菜单面板（独立 popup，见文件头）───────────────────────────
        // 「附近字幕」「字幕轨道」是**固定高度的滚动区**（用户 2026-11-02 要求）：
        // 内容超出时在区顶/区底显示上下小三角（可点，翻一行；灰=已到头）。
        // 区内的滚轮由 Flickable 自己处理；面板本身不再整体滚动。
        Popup {
            id: subMenu
            parent: contentRoot
            /// 面板贴在主菜单右侧、与所属条目行对齐（subY 见上）
            x: root.menuWidth + root.subMenuGap
            y: root.subY - 6
            width: root.subMenuWidth
            height: root.subViewH + 12
            padding: 0
            /// 键盘始终归主菜单：方向键/Enter/Esc 在 contentRoot 的 Keys 上
            focus: false
            /// 主菜单外按下才关（点主菜单换行时面板不闪）；CloseMultiple 让同一次
            /// 按压继续传给它下面的主菜单（见文件头）
            closePolicy: Popup.CloseOnPressOutsideParent | Popup.CloseMultiple
            background: Rectangle {
                color: "#d9111111"; radius: 8
                border { width: 1; color: "#22ffffff" }
            }
            contentItem: Item {
                Item {
                    id: subPanel
                    // 背景上下各比内容多 6px（子 popup 的 y 已经减掉上面的 6px）
                    x: 0
                    y: 6
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
                                readonly property int gstart: modelData.start !== undefined ? modelData.start : 0

                                /// 当前行落到本组时把它滚进可视区（键盘 ↓ 到第 9 行以后
                                /// 只改下标不动滚动，看起来"没反应"）
                                function ensureRowVisible(j) {
                                    if (!scrolls || j < gstart || j >= gstart + gitems.length)
                                        return
                                    var rowTop = (j - gstart) * root.rowH
                                    if (rowTop < gFlick.contentY)
                                        gFlick.contentY = rowTop
                                    else if (rowTop + root.rowH > gFlick.contentY + gFlick.height)
                                        gFlick.contentY = rowTop + root.rowH - gFlick.height
                                }
                                readonly property real scrollPos: gFlick ? gFlick.contentY : -1
                                Connections {
                                    target: root
                                    function onSubCurrentIndexChanged() {
                                        groupItem.ensureRowVisible(root.subCurrentIndex)
                                    }
                                }

                                // ── 非滚动组：逐行（分隔线/标题/普通项）──────────────
                                Column {
                                    visible: !groupItem.scrolls
                                    width: parent.width
                                    Repeater {
                                        model: groupItem.gitems
                                        delegate: SubRow {
                                            row: modelData
                                            rowIndex: groupItem.gstart + index
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
                                                    rowIndex: groupItem.gstart + index
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
    }
}
