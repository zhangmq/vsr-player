#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <Qt>

/// 键位表 —— **全应用唯一的按键定义来源**（2026-11-03 重构）。
///
/// 背景：改版前同一个键散在三处——C++ 的 StereoKeyFilter、main.qml 里 35 条
/// QML Shortcut（两个窗口各注册一份）、ContextMenu.qml 的 Keys.onPressed
/// （内含偏移快捷键的第二份映射）。改一个键要改三处，漏一处就是"某个窗口/
/// 某个状态下按键没反应"，而且互相覆盖时只能靠人肉排查。
///
/// 现在的模型：**动作（id）× 上下文（scope/owner）**。
///   - 同一 id 允许多行：同一个动作在不同上下文可以是不同物理键（2D 的
///     `h` = mpv OSD，3D 的 `H` = 3D 帮助面板），也可以走不同派发路径。
///   - 重复检测按 "上下文 + 归一化按键" 判定（normalized()），不按 id。
///
/// 三条派发路径（owner）：
///   Shortcut : KeyRouter 应用级事件过滤器，2D 与 3D 都生效
///   Filter   : 同一过滤器，只在 3D / 单眼态生效（必须抢在 QML 之前吞掉）
///   Menu     : 右键菜单自己的 Keys.onPressed（弹窗打开时窗口级动作全部让位）
///
/// 为什么不再用 QShortcut / QML Shortcut：应用级事件过滤器比 QShortcutMap
/// 更早看到 KeyPress（3D 的 Esc 一直依赖这一点），也不会产生 "Ambiguous
/// shortcut" 这类只在运行时才暴露的告警；冲突在启动自检里就挡掉。
namespace keymap {

/// 生效上下文。Both = 2D 与 3D 都生效（派发路径由 owner 决定）。
enum class Scope { Both, TwoD, ThreeD, Menu };

/// 派发路径（见文件头）。
enum class Owner { Shortcut, Filter, Menu };

/// 动作由谁执行：C++ 直接调 viewModel/stereo；Qml 发信号给 UI 层
/// （对话框、播放列表面板、右键菜单这些只有在 QML 里才存在的东西）；
/// Swallow 只吞掉按键不做动作（3D 的 Esc）。
enum class Handler { Cpp, Qml, Swallow };

/// 一条按键序列的解析结果。
struct Sequence {
    int key = 0;                                  // Qt::Key
    Qt::KeyboardModifiers mods = Qt::NoModifier;  // 只含 Shift/Ctrl/Alt/Meta
};

struct Binding {
    QString id;                 // 动作 id（唯一动作名）
    QStringList seqs;           // 触发序列（QKeySequence 语法，均为单键序列）
    QList<Sequence> parsed;     // seqs 的解析结果（buildTable 填；匹配/查重只看它）
    Scope scope = Scope::Both;
    Owner owner = Owner::Shortcut;
    Handler handler = Handler::Cpp;
    /// 弹窗打开时仍由右键菜单派发（字幕/音频延迟：菜单开着也要能调）
    bool popupOk = false;
    /// 按住不放是否重复触发（seek/音量/延迟 需要；截图/删除/全屏 不需要）
    bool allowRepeat = false;
    /// 帮助面板显示的键名（缺省取第一个序列的 PortableText；方向键给字形）
    QString helpKey;
};

/// 全表（进程内唯一实例，第一次调用时解析序列）。
const QList<Binding> &bindings();

/// 归一化按键，用于匹配与查重：
///   - Backtab 视为 Tab + Shift（Qt 对 Shift+Tab 就是报 Backtab）
///   - Keypad 修饰位去掉（小键盘方向键/加号要和主键区等价）
///   - 字符键的 Shift 去掉（`Ctrl++` 与 `Ctrl+Shift+=` 是同一个物理手势：
///     `+` 本身已经是 Shift+= 的产物，键值已编码了 Shift）
Sequence normalized(int key, Qt::KeyboardModifiers mods);
Sequence normalized(const Sequence &s);

/// 过滤器匹配：mode3d = 3D 或单眼态（见 KeyRouter::mode3d）。返回表内下标，-1 = 未命中。
int find(int key, Qt::KeyboardModifiers mods, bool mode3d);

/// 菜单匹配：菜单导航键（scope=Menu）或弹窗内可用的动作（popupOk）。
/// 不看 2D/3D —— 菜单在两种状态下都可打开，行为必须一致。
int findMenu(int key, Qt::KeyboardModifiers mods);

/// 动作 id → 表内下标（帮助面板取键名用，优先 3D 那一行）
int indexOfAction(const QString &id);

/// 动作 id 首个序列的可读文本（帮助面板/文档）
QString helpKeyFor(const QString &id);

/// 启动自检：空 id / 空序列 / 同上下文重复按键 / 序列解析失败。
/// 返回空列表 = 通过。
QStringList selfCheck();

/// 表 → QVariantList（每行一个 QVariantMap；RPC get-keys、T1 静态检查、文档）
QVariantList dump();

} // namespace keymap
