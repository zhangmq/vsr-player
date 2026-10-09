// Wayland 空闲抑制模块实现 —— 见 WaylandIdleInhibit.h 的架构说明。
//
// 依赖：
//   - wayland-scanner 生成的 zwp_idle_inhibit_manager_v1 客户端代码（构建期生成）
//   - QtWaylandClient：QWaylandClientExtensionTemplate 在 Qt 的 registry 上
//     bind 全局（Qt 自己拥有 wl_display 与事件线程——裸用 libwayland 从应用
//     线程读 socket 会与 Qt 的事件线程争用，故必须走这个受支持的入口）
//   - qtbase 私有头（QtGui/qpa/qplatformwindow_p.h）：取窗口的 wl_surface。
//     Qt 没有公开的 wl_surface 访问 API；QNativeInterface::Private::QWaylandWindow
//     是 Qt 为此提供的类型安全原生接口（KDE/Telegram 等同样采用）。
//
// 失败一律返回负值，调用方回退 D-Bus：本模块是"锦上添花"，不得影响播放。

#include "WaylandIdleInhibit.h"

#include <QWindow>

// 私有头两种安装形态：<QtGui/private/qpa/...>（上游布局）与
// <QtGui/qpa/...>（Arch 等发行版把 private 目录展开到版本根下）。
#if __has_include(<QtGui/private/qpa/qplatformwindow_p.h>)
#include <QtGui/private/qpa/qplatformwindow_p.h>
#else
#include <QtGui/qpa/qplatformwindow_p.h>
#endif
#include <QtWaylandClient/qwaylandclientextension.h>

#include "idle-inhibit-unstable-v1-client-protocol.h"

namespace {

/// 全局只 bind 一次；存续期 = 进程（QWaylandClientExtension 要求主线程创建）。
class IdleInhibitManagerV1 : public QWaylandClientExtensionTemplate<IdleInhibitManagerV1>
{
public:
    IdleInhibitManagerV1() : QWaylandClientExtensionTemplate<IdleInhibitManagerV1>(1) {}

    static const wl_interface *interface()
    {
        return &zwp_idle_inhibit_manager_v1_interface;
    }

    void init(wl_registry *registry, int id, int version)
    {
        m_manager = static_cast<zwp_idle_inhibit_manager_v1 *>(
            wl_registry_bind(registry, id, &zwp_idle_inhibit_manager_v1_interface,
                             static_cast<uint32_t>(version)));
    }

    zwp_idle_inhibit_manager_v1 *manager() const { return m_manager; }

private:
    zwp_idle_inhibit_manager_v1 *m_manager = nullptr;
};

IdleInhibitManagerV1 *manager()
{
    static IdleInhibitManagerV1 *s_manager = nullptr;
    if (!s_manager)
        s_manager = new IdleInhibitManagerV1();
    return s_manager;
}

}  // namespace

extern "C" int vsr_idle_wl_init(void)
{
    IdleInhibitManagerV1 *mgr = manager();
    return mgr->isActive() ? 1 : 0;
}

extern "C" int vsr_idle_wl_inhibit(void *qwindow, void **out_handle)
{
    if (!qwindow || !out_handle)
        return -1;
    *out_handle = nullptr;

    IdleInhibitManagerV1 *mgr = manager();
    if (!mgr->isActive() || !mgr->manager())
        return -2;   // 合成器未提供 zwp_idle_inhibit_manager_v1

    auto *win = static_cast<QWindow *>(qwindow);
    auto *iface = win->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!iface)
        return -3;
    wl_surface *surface = iface->surface();
    if (!surface)
        return -4;

    zwp_idle_inhibitor_v1 *inh =
        zwp_idle_inhibit_manager_v1_create_inhibitor(mgr->manager(), surface);
    if (!inh)
        return -5;
    *out_handle = inh;
    return 0;
}

extern "C" void vsr_idle_wl_release(void *handle)
{
    if (handle)
        zwp_idle_inhibitor_v1_destroy(static_cast<zwp_idle_inhibitor_v1 *>(handle));
}
