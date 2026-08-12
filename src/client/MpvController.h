#pragma once
#include <vulkan/vulkan.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_vk.h>
#include <atomic>
#include <functional>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

/// Thin wrapper around libmpv render API + ADVANCED_CONTROL.
///
/// Threading model:
///  - render API (update/render/reportSwap) on the main thread only
///  - client API (commands, properties) from any thread (client.h
///    "Multithreading" — serialized through the core lock)
///  - event loop on a dedicated thread (mpv_wait_event: only one thread
///    may call it per handle)
class MpvController {
public:
    /// @param vf_opt       full mpv `vf` option string (e.g. "vsr:scale=4:...")
    /// @param passthrough  additional mpv options as (key, value) pairs
    /// @param dev_exts     device extensions enabled on `dev` (required for
    ///                     pl_vulkan_import: FD export/import fns stay NULL
    ///                     otherwise → SIGSEGV in cuda_vk interop)
    bool init(VkInstance inst, VkPhysicalDevice pd, VkDevice dev,
              uint32_t qfi, bool benchmark, bool hwaccel,
              const char *vf_opt,
              const std::vector<std::pair<std::string, std::string>> &passthrough,
              const VkPhysicalDeviceFeatures2 *features,
              const char *const *dev_exts, int num_dev_exts);
    void destroy();

    uint64_t update();  // → MPV_RENDER_UPDATE_FRAME
    void render(VkImage img, VkFormat fmt, int w, int h);
    void skipRender(VkImage img, VkFormat fmt, int w, int h);
    void reportSwap();
    void loadFile(const char *path);
    void setUpdateCallback(void (*cb)(void*), void *data);
    /// 摘除 update 回调（mpv VO 线程在锁下检查 null，摘除即安全）。
    /// teardown 时必须先摘除——否则 VO 线程仍可能触发回调，invokeMethod
    /// 打到正在析构的 QML 对象（use-after-free，退出崩溃根因）。
    void clearUpdateCallback();

    // ── Generic property access (thread-safe per client.h) ────────────
    int64_t propertyInt64(const char *name);
    double propertyDouble(const char *name);
    bool propertyFlag(const char *name);
    std::string propertyString(const char *name);
    bool setPropertyString(const char *name, const std::string &value);
    // 注意：FLAG/DOUBLE 同步 set（setPropertyFlag/setPropertyDouble）已删——
    // 405e7fb 起一律走下方 Async 版本（主线程同步 set 会与解码 DR 分配互等死锁）

    // ── Async property set / command (no core-lock blocking) ──────────
    /// 主线程安全、立即返回：请求投递到 mpv 核心队列异步执行。
    /// 参数在调用时同步复制（client.c: m_option_copy /
    /// mp_input_parse_cmd_strv），局部变量无需保活。播放中调用。
    /// 返回 false 表示投递失败（记录日志），执行结果不追踪。
    bool setPropertyDoubleAsync(const char *name, double value);
    bool setPropertyStringAsync(const char *name, const std::string &value);
    /// Async 版 int64 / flag 属性设置（轨道切换 sid/aid/vid、字幕可见性）。
    bool setPropertyInt64Async(const char *name, int64_t value);
    bool setPropertyFlagAsync(const char *name, bool value);
    /// Async 版 commandV（如 vf-command）。
    void commandAsync(std::initializer_list<const char *> args);

    // ── Generic commands ───────────────────────────────────────────────
    /// args must not contain nullptr; a trailing nullptr is appended.
    void commandV(std::initializer_list<const char *> args);
    void commandStr(const char *cmd);

    /// Start the dedicated event thread (mpv_wait_event loop).
    /// Handler is called on the event thread; must not call mpv APIs
    /// that would block (thread-safe mpv calls are fine).
    void startEvents(std::function<void(mpv_event *)> handler);
    /// Stop and join the event thread. Safe to call multiple times
    /// (idempotent; destroy() also stops it).
    void stopEvents();

    /// Property observation. Register BEFORE startEvents (the observer
    /// list is read by the event thread; it must not change afterwards).
    /// The callback runs on the event thread — marshal to the main
    /// thread with QMetaObject::invokeMethod if it touches QObject state.
    using PropertyObserver = std::function<void()>;
    void observeProperty(const char *name, mpv_format format, PropertyObserver cb);

    /// 事件循环空闲回调（每次 mpv_wait_event 超时醒来时调用，~100ms）。
    /// 必须在 startEvents 前注册（与 observeProperty 同约束）。
    /// 事件线程执行——可调 mpv API（线程安全）。OSD 拉取推送用。
    void setIdlePollCallback(std::function<void()> cb) { idlePollCb_ = std::move(cb); }
    // mpv 日志行回调（事件线程，LOG_MESSAGE 先经此再转发 stderr）——
    // 用于提取结构化状态行（如 rife 的 "fruc-status:"）。
    void setLogMessageCallback(std::function<void(const char *text)> cb) {
        logCb_ = std::move(cb);
    }

    mpv_handle *handle() { return mpv_; }
    int64_t videoWidth()  { return propertyInt64("width"); }
    int64_t videoHeight() { return propertyInt64("height"); }

private:
    void eventLoop();
    struct PropObs { std::string name; mpv_format format; PropertyObserver cb; };

    mpv_handle          *mpv_  = nullptr;
    mpv_render_context  *mctx_ = nullptr;
    std::thread eventThread_;
    std::atomic<bool> eventsRunning_{false};
    std::function<void(mpv_event *)> eventHandler_;
    std::function<void()> idlePollCb_;   // 事件线程空闲回调（~100ms）
    std::function<void(const char *text)> logCb_;
    std::vector<PropObs> propObs_;  // written before startEvents, read by event thread
};
