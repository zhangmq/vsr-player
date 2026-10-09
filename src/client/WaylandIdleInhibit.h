#pragma once

/// Wayland 空闲抑制模块（libvsr-idle-wayland.so）的 C ABI。
///
/// 为什么是独立模块 + dlopen：协议实现需要 QtWaylandClient
/// （QWaylandClientExtension 负责在 Qt 的 registry/事件线程上安全地 bind 全局）
/// 与 qtbase 私有头（取窗口的 wl_surface）。若把 QtWaylandClient 直接链进主程序，
/// 任何未装 qt6-wayland 的系统（GNOME/XFCE 上常见：Qt 应用走 XWayland）
/// 会在**启动时**因动态链接失败而打不开——这是"保屏"功能不该付出的代价。
/// 故：模块单独编译，主程序运行时 dlopen；加载失败/合成器不支持 → 回退 D-Bus。
///
/// 协议：zwp_idle_inhibit_manager_v1（wayland-protocols unstable v1）。
/// 语义与 mpv 在 Wayland 下的防息屏一致——由合成器直接抑制 idle，
/// 不依赖桌面是否实现 org.freedesktop.ScreenSaver。
#ifdef __cplusplus
extern "C" {
#endif

/// 预热：创建 QWaylandClientExtension 对象。必须尽早调用——全局绑定由 Qt 在
/// 事件循环里异步完成，等到首次播放才创建会因 isActive() 仍为 false 而误判
/// "合成器不支持"并回退 D-Bus。
/// 返回 1 = 已绑定（可直接使用）；0 = 已创建、绑定待完成；负值 = 不可用。
int vsr_idle_wl_init(void);

/// 为 window（QWindow*）的 wl_surface 创建 idle inhibitor。
/// 成功返回 0 并把句柄写入 *out_handle；失败返回负值（-2 合成器无该全局，
/// -3/-4 Qt 未提供 wl_surface，-5 创建失败，-1 参数错误）。
int vsr_idle_wl_inhibit(void *qwindow, void **out_handle);

/// 销毁 inhibitor（handle 来自 vsr_idle_wl_inhibit）。
void vsr_idle_wl_release(void *handle);

#ifdef __cplusplus
}
#endif
