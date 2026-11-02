#include "Keymap.h"

#include <QChar>
#include <QHash>
#include <QKeyCombination>
#include <QKeySequence>
#include <QSet>

namespace keymap {
namespace {

constexpr Qt::KeyboardModifiers kModMask =
    Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;

Qt::KeyboardModifiers maskMods(Qt::KeyboardModifiers m) { return m & kModMask; }

/// 字符键（`[`、`+`、`=` …）的 Shift 已经体现在键值上：Shift+`=` 报的是
/// Key_Plus。所以这类键比较时忽略 Shift，`Ctrl++` 才能真正被按到
///（用户 2026-11-02 提过"很多键盘 + 要按 Shift，避免按不出来"）。
/// 字母/数字/功能键保持严格比较——否则 `Ctrl+O` 与 `Ctrl+Shift+O` 会撞车。
bool shiftEncodedInKey(int key) {
    if (key < 0x20 || key > 0x7e)
        return false;
    return !QChar(ushort(key)).isLetterOrNumber();
}

Sequence parseSequence(const QString &text, bool *ok)
{
    const QKeySequence ks = QKeySequence::fromString(text, QKeySequence::PortableText);
    // 只支持单键序列（多段和弦 = mpv 的 input.conf 语法，本项目不用）
    if (ks.count() != 1 || ks[0].key() == 0) {
        *ok = false;
        return {};
    }
    *ok = !(ks[0].key() == 0);
    return normalized(ks[0].key(), maskMods(ks[0].keyboardModifiers()));
}

QList<Binding> buildTable()
{
    QList<Binding> t;
    t.reserve(64);

    // ── 2D / 全局：KeyRouter 直接派发（owner=Shortcut）─────────────────
    // 回放与窗口动作，键位沿用改版前 main.qml 的 35 条 Shortcut（用户已
    // 在其上工作过一段时间，不在这里重新设计键位）。
    t.append(Binding{.id = "escape", .seqs = {"Escape"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut, .handler = Handler::Qml,
                     .helpKey = "Esc"});
    t.append(Binding{.id = "play-pause", .seqs = {"Space"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut});
    // 当前条目操作：Delete = 仅从列表移除；Shift+Delete = 连同文件移入回收站
    t.append(Binding{.id = "remove-from-playlist", .seqs = {"Delete"},
                     .owner = Owner::Shortcut});
    t.append(Binding{.id = "delete-file", .seqs = {"Shift+Delete"},
                     .owner = Owner::Shortcut});
    t.append(Binding{.id = "seek-back", .seqs = {"Left"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut, .allowRepeat = true, .helpKey = "←"});
    t.append(Binding{.id = "seek-back-10", .seqs = {"Shift+Left"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut, .allowRepeat = true,
                     .helpKey = "Shift+←"});
    t.append(Binding{.id = "seek-fwd", .seqs = {"Right"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut, .allowRepeat = true, .helpKey = "→"});
    t.append(Binding{.id = "seek-fwd-10", .seqs = {"Shift+Right"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut, .allowRepeat = true,
                     .helpKey = "Shift+→"});
    t.append(Binding{.id = "volume-up", .seqs = {"Up"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut, .allowRepeat = true, .helpKey = "↑"});
    t.append(Binding{.id = "volume-down", .seqs = {"Down"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut, .allowRepeat = true, .helpKey = "↓"});
    t.append(Binding{.id = "screenshot", .seqs = {"S"}, .owner = Owner::Shortcut});
    t.append(Binding{.id = "frame-step-fwd", .seqs = {"."}, .owner = Owner::Shortcut});
    t.append(Binding{.id = "frame-step-back", .seqs = {","}, .owner = Owner::Shortcut});
    // 2D：h 与 Tab 都切 mpv 的 OSD（3D 下 Tab 也切管线 OSD，见下）
    t.append(Binding{.id = "toggle-osd", .seqs = {"h", "Tab"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut});
    // 打开右键菜单：Menu 键 / Shift+F10（Windows 惯例）/ F10
    t.append(Binding{.id = "open-menu", .seqs = {"Menu", "Shift+F10", "F10"},
                     .owner = Owner::Shortcut, .handler = Handler::Qml});
    t.append(Binding{.id = "quit", .seqs = {"Ctrl+Q"}, .owner = Owner::Shortcut});
    t.append(Binding{.id = "playlist-prev", .seqs = {"Ctrl+Left"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut});
    t.append(Binding{.id = "playlist-next", .seqs = {"Ctrl+Right"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut});
    t.append(Binding{.id = "speed-half", .seqs = {"["}, .owner = Owner::Shortcut});
    t.append(Binding{.id = "speed-double", .seqs = {"]"}, .owner = Owner::Shortcut});
    t.append(Binding{.id = "speed-normal", .seqs = {"\\"}, .owner = Owner::Shortcut});
    t.append(Binding{.id = "open-file", .seqs = {"Ctrl+O"}, .owner = Owner::Shortcut,
                     .handler = Handler::Qml});
    t.append(Binding{.id = "open-folder", .seqs = {"Ctrl+Shift+O"},
                     .owner = Owner::Shortcut, .handler = Handler::Qml});
    t.append(Binding{.id = "open-url", .seqs = {"Ctrl+L"}, .owner = Owner::Shortcut,
                     .handler = Handler::Qml});
    // 偏移（对齐 mpv 的输入惯例）：字幕 z / Shift+Z，音频 Ctrl++ / Ctrl+-；
    // 归零分开：字幕 Ctrl+0、音频 Ctrl+Alt+0（用户 2026-11-02 定，不要一起归零）。
    // popupOk：右键菜单开着时窗口级动作全部让位，但这几个由菜单照旧派发。
    t.append(Binding{.id = "sub-delay-down", .seqs = {"z"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut, .popupOk = true, .allowRepeat = true,
                     .helpKey = "Z"});
    t.append(Binding{.id = "sub-delay-up", .seqs = {"Shift+Z"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut, .popupOk = true, .allowRepeat = true,
                     .helpKey = "Shift+Z"});
    t.append(Binding{.id = "sub-delay-reset", .seqs = {"Ctrl+0"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut, .popupOk = true});
    t.append(Binding{.id = "audio-delay-up", .seqs = {"Ctrl++", "Ctrl+="},
                     .scope = Scope::TwoD, .owner = Owner::Shortcut, .popupOk = true,
                     .allowRepeat = true});
    t.append(Binding{.id = "audio-delay-down", .seqs = {"Ctrl+-"}, .scope = Scope::TwoD,
                     .owner = Owner::Shortcut, .popupOk = true, .allowRepeat = true});
    t.append(Binding{.id = "audio-delay-reset", .seqs = {"Ctrl+Alt+0"},
                     .scope = Scope::TwoD, .owner = Owner::Shortcut, .popupOk = true});
    t.append(Binding{.id = "toggle-subtitles", .seqs = {"V"}, .owner = Owner::Shortcut});
    t.append(Binding{.id = "toggle-playlist", .seqs = {"P"}, .owner = Owner::Shortcut,
                     .handler = Handler::Qml});
    t.append(Binding{.id = "toggle-fullscreen", .seqs = {"F"},
                     .owner = Owner::Shortcut});

    // ── 3D / 单眼：过滤器在 QML 之前吞键（owner=Filter）────────────────
    // 画面比例：A 正序 / Shift+A 逆序，列表照抄 mpv input.conf:156。
    // 2D 也走这里（2D 也要能修正片源比例）。
    t.append(Binding{.id = "aspect-next", .seqs = {"A"}, .owner = Owner::Filter,
                     .helpKey = "A"});
    t.append(Binding{.id = "aspect-prev", .seqs = {"Shift+A"}, .owner = Owner::Filter});
    t.append(Binding{.id = "stereo-layout", .seqs = {"J"}, .scope = Scope::ThreeD,
                     .owner = Owner::Filter, .helpKey = "J"});
    t.append(Binding{.id = "stereo-swap", .seqs = {"K"}, .scope = Scope::ThreeD,
                     .owner = Owner::Filter, .helpKey = "K"});
    t.append(Binding{.id = "stereo-half", .seqs = {"L"}, .scope = Scope::ThreeD,
                     .owner = Owner::Filter, .helpKey = "L"});
    // 3D 的 H = 3D 快捷键帮助面板（2D 的 h = mpv OSD，见上）
    t.append(Binding{.id = "toggle-help", .seqs = {"H"}, .scope = Scope::ThreeD,
                     .owner = Owner::Filter, .helpKey = "H"});
    // 3D 的 Tab/Shift+Tab = 切管线状态 OSD（必须由过滤器吞掉，否则焦点遍历）
    t.append(Binding{.id = "toggle-osd", .seqs = {"Tab", "Shift+Tab"},
                     .scope = Scope::ThreeD, .owner = Owner::Filter, .helpKey = "Tab"});
    t.append(Binding{.id = "play-pause", .seqs = {"Space"}, .scope = Scope::ThreeD,
                     .owner = Owner::Filter});
    t.append(Binding{.id = "seek-back", .seqs = {"Left"}, .scope = Scope::ThreeD,
                     .owner = Owner::Filter, .allowRepeat = true, .helpKey = "←"});
    t.append(Binding{.id = "seek-back-10", .seqs = {"Shift+Left"},
                     .scope = Scope::ThreeD, .owner = Owner::Filter,
                     .allowRepeat = true, .helpKey = "Shift+←"});
    t.append(Binding{.id = "seek-fwd", .seqs = {"Right"}, .scope = Scope::ThreeD,
                     .owner = Owner::Filter, .allowRepeat = true, .helpKey = "→"});
    t.append(Binding{.id = "seek-fwd-10", .seqs = {"Shift+Right"},
                     .scope = Scope::ThreeD, .owner = Owner::Filter,
                     .allowRepeat = true, .helpKey = "Shift+→"});
    t.append(Binding{.id = "volume-up", .seqs = {"Up"}, .scope = Scope::ThreeD,
                     .owner = Owner::Filter, .allowRepeat = true, .helpKey = "↑"});
    t.append(Binding{.id = "volume-down", .seqs = {"Down"}, .scope = Scope::ThreeD,
                     .owner = Owner::Filter, .allowRepeat = true, .helpKey = "↓"});
    t.append(Binding{.id = "playlist-prev", .seqs = {"Ctrl+Left"}, .scope = Scope::ThreeD,
                     .owner = Owner::Filter, .helpKey = "Ctrl+←"});
    t.append(Binding{.id = "playlist-next", .seqs = {"Ctrl+Right"},
                     .scope = Scope::ThreeD, .owner = Owner::Filter,
                     .helpKey = "Ctrl+→"});
    // 3D 下 Esc 一律吞掉（主窗口可能仍被合成器聚焦，Esc 退全屏会把窗口切回
    // 焦点屏）；退出 3D 用桌面的 3D → Off。
    t.append(Binding{.id = "escape-swallow", .seqs = {"Escape"}, .scope = Scope::ThreeD,
                     .owner = Owner::Filter, .handler = Handler::Swallow});

    // ── 菜单导航（右键菜单自己的 Keys.onPressed）──────────────────────
    // 菜单在 2D/3D 都可打开，行为必须一致，所以不吃 scope 的 2D/3D 语义。
    t.append(Binding{.id = "menu-up", .seqs = {"Up"}, .scope = Scope::Menu,
                     .owner = Owner::Menu});
    t.append(Binding{.id = "menu-down", .seqs = {"Down"}, .scope = Scope::Menu,
                     .owner = Owner::Menu});
    t.append(Binding{.id = "menu-left", .seqs = {"Left"}, .scope = Scope::Menu,
                     .owner = Owner::Menu});
    t.append(Binding{.id = "menu-right", .seqs = {"Right"}, .scope = Scope::Menu,
                     .owner = Owner::Menu});
    t.append(Binding{.id = "menu-home", .seqs = {"Home"}, .scope = Scope::Menu,
                     .owner = Owner::Menu});
    t.append(Binding{.id = "menu-end", .seqs = {"End"}, .scope = Scope::Menu,
                     .owner = Owner::Menu});
    // 主键盘 Enter 与数字键盘 Enter 都要能用
    t.append(Binding{.id = "menu-enter", .seqs = {"Return", "Enter"}, .scope = Scope::Menu,
                     .owner = Owner::Menu});
    t.append(Binding{.id = "menu-escape", .seqs = {"Escape"}, .scope = Scope::Menu,
                     .owner = Owner::Menu});

    // 解析序列（失败的行由 selfCheck 报出来）
    for (Binding &b : t) {
        for (const QString &s : b.seqs) {
            bool ok = false;
            const Sequence seq = parseSequence(s, &ok);
            if (ok)
                b.parsed.append(seq);
        }
    }
    return t;
}

} // namespace

Sequence normalized(int key, Qt::KeyboardModifiers mods)
{
    Sequence s;
    s.key = key;
    // Shift+Tab 在 Qt 里是 Key_Backtab + Shift；统一成 Tab + Shift 才好和
    // 表里的 "Tab" / "Shift+Tab" 对齐（3D 下两者都切管线 OSD）。
    if (s.key == Qt::Key_Backtab) {
        s.key = Qt::Key_Tab;
        mods |= Qt::ShiftModifier;
    }
    mods = maskMods(mods);
    // 符号键：Shift 已在键值里；小键盘：与主键区等价
    if (shiftEncodedInKey(s.key))
        mods &= ~Qt::ShiftModifier;
    s.mods = mods;
    return s;
}

Sequence normalized(const Sequence &s) { return normalized(s.key, s.mods); }

const QList<Binding> &bindings()
{
    static const QList<Binding> table = buildTable();
    return table;
}

namespace {

bool inMode(Scope scope, bool mode3d)
{
    if (scope == Scope::Menu)
        return false;
    return mode3d ? (scope == Scope::Both || scope == Scope::ThreeD)
                  : (scope == Scope::Both || scope == Scope::TwoD);
}

/// ⚠ 不能写成 `QKeySequence(key, mods)`：那个构造函数是
/// `QKeySequence(int k1, int k2, int k3, int k4)`（**多段和弦**），
/// 会把修饰位当成第二段按键，结果 "Shift+Delete" 打印成 "Del, Shift+"。
QString keyText(int key, Qt::KeyboardModifiers mods)
{
    return QKeySequence(QKeyCombination(mods, Qt::Key(key)))
        .toString(QKeySequence::PortableText);
}

QString keyText(const Sequence &s) { return keyText(s.key, s.mods); }

} // namespace

int find(int key, Qt::KeyboardModifiers mods, bool mode3d)
{
    const Sequence want = normalized(key, mods);
    const QList<Binding> &t = bindings();
    for (int i = 0; i < t.size(); ++i) {
        const Binding &b = t.at(i);
        if (b.owner == Owner::Menu || !inMode(b.scope, mode3d))
            continue;
        for (const Sequence &s : b.parsed)
            if (s.key == want.key && s.mods == want.mods)
                return i;
    }
    return -1;
}

int findMenu(int key, Qt::KeyboardModifiers mods)
{
    const Sequence want = normalized(key, mods);
    const QList<Binding> &t = bindings();
    for (int i = 0; i < t.size(); ++i) {
        const Binding &b = t.at(i);
        if (b.scope != Scope::Menu && !b.popupOk)
            continue;
        for (const Sequence &s : b.parsed)
            if (s.key == want.key && s.mods == want.mods)
                return i;
    }
    return -1;
}

int indexOfAction(const QString &id)
{
    const QList<Binding> &t = bindings();
    int fallback = -1;
    for (int i = 0; i < t.size(); ++i) {
        if (t.at(i).id != id)
            continue;
        if (t.at(i).owner == Owner::Filter)
            return i;          // 3D 那一行才是帮助面板要显示的
        if (fallback < 0)
            fallback = i;
    }
    return fallback;
}

QString helpKeyFor(const QString &id)
{
    const int i = indexOfAction(id);
    if (i < 0)
        return QString();
    const Binding &b = bindings().at(i);
    if (!b.helpKey.isEmpty())
        return b.helpKey;
    return b.parsed.isEmpty() ? (b.seqs.isEmpty() ? QString() : b.seqs.first())
                              : keyText(b.parsed.first());
}

QStringList selfCheck()
{
    QStringList errs;
    QSet<QString> seenIds;
    const QList<Binding> &t = bindings();

    for (int i = 0; i < t.size(); ++i) {
        const Binding &b = t.at(i);
        const QString where = QStringLiteral("row %1 (%2)").arg(i).arg(b.id);
        if (b.id.isEmpty())
            errs << QStringLiteral("%1: empty action id").arg(where);
        if (b.seqs.isEmpty())
            errs << QStringLiteral("%1: no sequences").arg(where);
        if (b.parsed.size() != b.seqs.size()) {
            for (const QString &s : b.seqs) {
                bool ok = false;
                parseSequence(s, &ok);
                if (!ok)
                    errs << QStringLiteral("%1: cannot parse sequence '%2'")
                                .arg(where, s);
            }
        }
        if (b.owner == Owner::Menu && b.scope != Scope::Menu)
            errs << QStringLiteral("%1: owner=menu but scope!=menu").arg(where);
        if (b.scope == Scope::Menu && b.owner != Owner::Menu)
            errs << QStringLiteral("%1: scope=menu but owner!=menu").arg(where);
        if (b.popupOk && b.handler == Handler::Qml)
            errs << QStringLiteral("%1: popupOk actions must be handled in C++")
                        .arg(where);
        seenIds.insert(b.id);
    }

    // 同上下文重复按键（左：2D，中：3D，右：菜单）
    auto dupCheck = [&](const char *ctxName, bool twoD, bool threeD, bool menu) {
        QHash<QString, QString> seen;
        for (int i = 0; i < t.size(); ++i) {
            const Binding &b = t.at(i);
            bool inCtx = false;
            if (menu)
                inCtx = (b.scope == Scope::Menu || b.popupOk);
            else if (twoD)
                inCtx = inMode(b.scope, false);
            else if (threeD)
                inCtx = inMode(b.scope, true);
            if (!inCtx)
                continue;
            for (const Sequence &s : b.parsed) {
                const Sequence n = normalized(s);
                const QString k = QStringLiteral("%1|%2")
                                      .arg(n.key)
                                      .arg(int(n.mods));
                if (seen.contains(k)) {
                    errs << QStringLiteral("%1: duplicate key %2 (actions '%3' / '%4')")
                                .arg(QLatin1String(ctxName), keyText(n),
                                     seen.value(k), b.id);
                } else {
                    seen.insert(k, b.id);
                }
            }
        }
    };
    dupCheck("2d", true, false, false);
    dupCheck("3d", false, true, false);
    dupCheck("menu", false, false, true);
    return errs;
}

QVariantList dump()
{
    QVariantList out;
    for (const Binding &b : bindings()) {
        QVariantList seqs;
        QStringList parsed;
        for (const Sequence &s : b.parsed) {
            parsed << keyText(s);
            seqs << QVariantMap{{"key", s.key}, {"mods", int(s.mods)}};
        }
        out.append(QVariantMap{
            {"id", b.id},
            {"seqs", b.seqs},
            {"parsed", parsed},
            {"scope", b.scope == Scope::Both     ? "both"
                      : b.scope == Scope::TwoD   ? "2d"
                      : b.scope == Scope::ThreeD ? "3d"
                                                 : "menu"},
            {"owner", b.owner == Owner::Shortcut ? "shortcut"
                      : b.owner == Owner::Filter ? "filter"
                                                 : "menu"},
            {"handler", b.handler == Handler::Cpp   ? "cpp"
                        : b.handler == Handler::Qml ? "qml"
                                                    : "swallow"},
            {"popup_ok", b.popupOk},
            {"repeat", b.allowRepeat},
            {"help_key", helpKeyFor(b.id)},
            {"raw", seqs},
        });
    }
    return out;
}

} // namespace keymap
