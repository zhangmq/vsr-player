#include "SessionIdle.h"

#include "Log.h"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QLibrary>
#include <QTimer>
#include <QWindow>

#ifdef HAVE_QTDBUS
#include <QDBusConnection>
#include <QDBusMessage>
#endif

// ── IdleInhibitor ──────────────────────────────────────────────────────

namespace {

#ifdef HAVE_QTDBUS
constexpr const char *SS_SERVICE = "org.freedesktop.ScreenSaver";
constexpr const char *SS_PATH    = "/ScreenSaver";
constexpr const char *SS_IFACE   = "org.freedesktop.ScreenSaver";
/// D-Bus 调用超时（毫秒）：调用只发生在播放/暂停翻转时（非每帧），但会话总线
/// 异常时不应让 GUI 线程无限等——1s 足够本地服务应答。
constexpr int DBUS_TIMEOUT_MS = 1000;
#endif

/// Wayland 模块（libvsr-idle-wayland.so）搜索路径：
///   1. 可执行文件同目录（dev 构建树：build/src/client/）
///   2. <appdir>/../lib/vsr-player/（安装后与其它 bundled 库同处）
QString modulePath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList cands = {
        appDir + QStringLiteral("/libvsr-idle-wayland.so"),
        QDir(appDir + QStringLiteral("/../lib/vsr-player")).absolutePath()
            + QStringLiteral("/libvsr-idle-wayland.so"),
    };
    for (const QString &p : cands)
        if (QFileInfo::exists(p))
            return p;
    return QString();
}

using inhibit_fn = int (*)(void *, void **);
using release_fn = void (*)(void *);

}  // namespace

IdleInhibitor::IdleInhibitor(QWindow *window, QObject *parent)
    : QObject(parent), m_window(window)
{
    prepareWayland();
}

void IdleInhibitor::prepareWayland()
{
    using init_fn = int (*)();
    auto init = reinterpret_cast<init_fn>(moduleSymbol("vsr_idle_wl_init"));
    if (!init)
        return;
    const int bound = init();
    MLOG_INFO("wayland idle-inhibit module ready (bound=%d)", bound);
}

IdleInhibitor::~IdleInhibitor()
{
    release();
}

IdleInhibitor::FnPtr IdleInhibitor::moduleSymbol(const char *name)
{
    if (!m_moduleTried) {
        m_moduleTried = true;
        const QString path = modulePath();
        if (path.isEmpty()) {
            MLOG_INFO("wayland idle-inhibit module not found — using D-Bus");
            return nullptr;
        }
        m_module = new QLibrary(path, this);
        if (!m_module->load()) {
            // 可选模块：目标机无 QtWaylandClient 时属预期情况（回退 D-Bus）
            MLOG_INFO("wayland idle-inhibit module load failed (%s): %s",
                      path.toUtf8().constData(),
                      m_module->errorString().toUtf8().constData());
            delete m_module;
            m_module = nullptr;
            return nullptr;
        }
    }
    return m_module ? m_module->resolve(name) : nullptr;
}

bool IdleInhibitor::acquireWayland()
{
    auto inhibit = reinterpret_cast<inhibit_fn>(moduleSymbol("vsr_idle_wl_inhibit"));
    if (!inhibit)
        return false;
    void *handle = nullptr;
    const int rc = inhibit(m_window, &handle);
    if (rc == 0 && handle) {
        m_wlInhibitor = handle;
        m_backend = Backend::Wayland;
        MLOG_INFO("screen saver inhibited via wayland idle-inhibit protocol");
        return true;
    }
    // -2 = 合成器无该全局（X11/XWayland/不支持）；-3/-4 = Qt 未给出 wl_surface
    MLOG_INFO("wayland idle-inhibit unavailable (rc=%d) — falling back to D-Bus", rc);
    return false;
}

bool IdleInhibitor::acquireDBus()
{
#ifdef HAVE_QTDBUS
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(SS_SERVICE), QString::fromLatin1(SS_PATH),
        QString::fromLatin1(SS_IFACE), QStringLiteral("Inhibit"));
    msg << QStringLiteral("vsr-player") << QStringLiteral("Video playback");

    QDBusMessage reply = QDBusConnection::sessionBus().call(
        msg, QDBus::Block, DBUS_TIMEOUT_MS);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        if (!m_warned) {
            MLOG_WARN("screen saver inhibition unavailable (%s) — display may dim while playing",
                      reply.errorMessage().toUtf8().constData());
            m_warned = true;
        }
        return false;
    }
    m_cookie = reply.arguments().first().toUInt();
    if (m_cookie == 0) {
        if (!m_warned) {
            MLOG_WARN("screen saver inhibition returned no cookie — display may dim while playing");
            m_warned = true;
        }
        return false;
    }
    m_backend = Backend::DBus;
    MLOG_INFO("screen saver inhibited via org.freedesktop.ScreenSaver (cookie=%u)", m_cookie);
    return true;
#else
    if (!m_warned) {
        MLOG_WARN("built without QtDBus — cannot inhibit dim/screensaver while playing");
        m_warned = true;
    }
    return false;
#endif
}

void IdleInhibitor::setActive(bool active)
{
    if (!active) {
        release();
        return;
    }
    if (m_backend != Backend::None)
        return;   // 已持有

    // 每次申请都先试 Wayland（模块可能在上次申请后才加载/合成器全局才可用）
    if (acquireWayland())
        return;
    acquireDBus();
}

void IdleInhibitor::release()
{
    if (m_backend == Backend::Wayland) {
        auto releaseFn = reinterpret_cast<release_fn>(moduleSymbol("vsr_idle_wl_release"));
        if (releaseFn && m_wlInhibitor)
            releaseFn(m_wlInhibitor);
        MLOG_INFO("wayland idle inhibitor released");
        m_wlInhibitor = nullptr;
    } else if (m_backend == Backend::DBus) {
#ifdef HAVE_QTDBUS
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QString::fromLatin1(SS_SERVICE), QString::fromLatin1(SS_PATH),
            QString::fromLatin1(SS_IFACE), QStringLiteral("UnInhibit"));
        msg << (unsigned)m_cookie;
        QDBusConnection::sessionBus().call(msg, QDBus::Block, DBUS_TIMEOUT_MS);
        MLOG_INFO("screen saver inhibition released (cookie=%u)", m_cookie);
#endif
        m_cookie = 0;
    }
    m_backend = Backend::None;
}

// ── CursorAutoHide ─────────────────────────────────────────────────────

CursorAutoHide::CursorAutoHide(QWindow *window, QObject *parent)
    : QObject(parent), m_window(window)
{
    if (m_window)
        m_window->installEventFilter(this);
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(IDLE_MS);
    connect(m_timer, &QTimer::timeout, this, [this] { maybeHide(); });
}

bool CursorAutoHide::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::Wheel:
    case QEvent::HoverMove:
    case QEvent::Enter:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
        show();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);   // 只观察，不消费事件
}

void CursorAutoHide::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (enabled)
        m_timer->start();   // 开始播放：重新计时
    else
        show();             // 暂停/停止：立即恢复指针
}

void CursorAutoHide::setSuspended(bool suspended)
{
    if (m_suspended == suspended)
        return;
    m_suspended = suspended;
    if (suspended)
        show();             // UI 出现：恢复指针（与 mpv OSC 可见时一致）
    else
        m_timer->start();
}

void CursorAutoHide::show()
{
    if (m_hidden) {
        if (m_window)
            m_window->unsetCursor();
        m_hidden = false;
        MLOG_INFO("cursor shown");
    }
    m_timer->start();
}

void CursorAutoHide::maybeHide()
{
    if (!m_enabled || m_suspended || m_hidden || !m_window)
        return;
    if (!m_window->isVisible())
        return;
    m_window->setCursor(Qt::BlankCursor);
    m_hidden = true;
    MLOG_INFO("cursor hidden (idle %d ms while playing)", IDLE_MS);
}
