#pragma once

#include <QObject>
#include <QString>

class QLibrary;
class QTimer;
class QWindow;

/// 播放期间阻止系统空闲动作（dim / 熄屏 / 屏保 / 锁屏）。
///
/// 两级机制，按 Wayland 最佳实践取先：
///   1. **Wayland idle-inhibit 协议**（`zwp_idle_inhibit_manager_v1`）——
///      合成器直接抑制 idle，不依赖桌面是否实现 D-Bus 服务；与 mpv 在
///      Wayland 下的做法一致。实现在独立模块 libvsr-idle-wayland.so 里
///      （原因见 WaylandIdleInhibit.h：避免把 QtWaylandClient 硬链进主程序），
///      运行时 dlopen，缺模块/合成器不支持即降级。
///   2. **`org.freedesktop.ScreenSaver`（会话总线 Inhibit/UnInhibit）**——
///      X11 会话、无 idle-inhibit 全局的合成器、无 qt6-wayland 的系统；
///      Firefox/VLC/Chrome 在 Linux 上走的就是这条 freedesktop 标准接口。
/// 两者都不可用时只警告一次并保持 no-op——不影响播放。
class IdleInhibitor : public QObject {
public:
    explicit IdleInhibitor(QWindow *window, QObject *parent = nullptr);
    ~IdleInhibitor() override;

    /// playing == true → 申请抑制；false → 归还。幂等，可重复调用。
    void setActive(bool active);

private:
    enum class Backend { None, Wayland, DBus };

    void release();
    bool acquireWayland();
    bool acquireDBus();
    /// 惰性加载 Wayland 模块并解析符号（返回 nullptr = 不可用）。
    using FnPtr = void (*)();
    FnPtr moduleSymbol(const char *name);
    /// 预热 Wayland 模块（构造函数调用一次）：扩展对象的全局绑定是异步的，
    /// 首次播放才创建会误判"合成器不支持"。
    void prepareWayland();

    QWindow  *m_window = nullptr;
    QLibrary *m_module = nullptr;
    bool      m_moduleTried = false;
    Backend   m_backend = Backend::None;
    void     *m_wlInhibitor = nullptr;   ///< 模块返回的 inhibitor 句柄
    unsigned  m_cookie = 0;              ///< D-Bus Inhibit 返回的 cookie
    bool      m_warned = false;
};

