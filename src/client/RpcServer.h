#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

struct mpv_handle;
class MpvController;
class PlayerViewModel;
class QQuickWindow;

/// mpv-compatible JSON IPC server (Unix domain socket).
///
/// 协议语义（压测脚本作者必读）：
///  - 命令类（play/pause/stop/seek/set_property/set-vsr）经 QueuedConnection
///    异步投递主线程执行——响应 "success" 表示"已接受"而非"已执行"；
///    set 后立即 get 可能读到旧值（陈旧窗口 ~1 事件循环迭代），脚本应
///    轮询直至收敛或间隔 0.3s+。
///  - get 类（get_property/get-vsr）RPC 线程同步直读（libmpv client API
///    线程安全 / viewModel 只读 atomic）。
///  - command/dump-vsr 保持 mpv 直通（同步）。
///  - seek/volume 边界换算：seek 秒→毫秒；volume 0-100 → viewModel 0..1
///    （>100 钳制到 100）。
///  - routed slots 在主线程执行同步 mpv 调用（部分 async）——高频突发
///    命令可能造成帧抖动，压测时保持 <100Hz。
///  - idle（无活动文件）时 set_property pause no 委托 play() 重载
///    lastPath；idle 且无历史路径时该命令为 no-op。
///  - quit 经回调路由到 Qt 事件循环（跨线程）；render API 仅在主线程。
class RpcServer {
public:
    RpcServer() = default;
    ~RpcServer();

    /// Start listening. Returns false on bind failure (runs without IPC).
    bool start(const std::string &socket_path);
    void stop();

    void setMpv(MpvController *mpv) { mpv_ = mpv; }
    void setViewModel(PlayerViewModel *vm) { vm_ = vm; }
    void setWindow(QQuickWindow *win) { win_ = win; }   // resize 命令路由目标
    void setQuitCallback(std::function<void()> cb) { quit_cb_ = std::move(cb); }

private:
    void listenLoop();
    void handleClient(int fd);
    void handleLine(int fd, const std::string &line);

    /// Parse {"command": [name, args...]} (mpv subset). Returns error
    /// string (empty on success).
    static std::string parseCommand(const std::string &json_text,
                                    std::string &out_name,
                                    std::vector<std::string> &out_args);
    std::string handleSetProperty(const std::string &name, const std::string &value);
    std::string handleGetProperty(const std::string &name);
    std::string handleSetVsr(const std::string &param, const std::string &value);
    std::string handleGetVsr(const std::string &param);
    /// [videoname] part of the default dump path (basename, no extension).
    static std::string dumpBaseName(mpv_handle *h);
    /// [Nx] part: filter output width / decoded width.
    static std::string dumpScaleLabel(mpv_handle *h);
    /// data is embedded raw as JSON (number, quoted+escaped string, or object).
    static std::string formatResponse(const std::string &error,
                                      const std::string &data = "");
    static std::string jsonEscape(const std::string &s);

    std::thread thread_;
    int server_fd_ = -1;
    std::string socket_path_;
    std::atomic<bool> running_{false};
    MpvController *mpv_ = nullptr;
    PlayerViewModel *vm_ = nullptr;
    QQuickWindow *win_ = nullptr;
    std::function<void()> quit_cb_;
};
