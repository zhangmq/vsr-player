#include "RpcServer.h"
#include "MpvController.h"
#include "PlayerViewModel.h"
#include "StereoController.h"
#include <QQuickView>
#include <QQuickItem>
#include <QImage>
#include "FrucBackend.h"
#include <mpv/client.h>
#include <QMetaObject>
#include <QQuickWindow>
#include <QVariant>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "Log.h"
// RPC 日志（[rpc] 前缀标识模块，走 client 分级日志体系）
#define RPCLOG(fmt, ...) MLOG_INFO("[rpc] " fmt, ##__VA_ARGS__)

// ═════════════════════════════════════════════════════════════════════
//  Lifecycle
// ═════════════════════════════════════════════════════════════════════

RpcServer::~RpcServer() {
    stop();
}

bool RpcServer::start(const std::string &socket_path) {
    socket_path_ = socket_path;
    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        RPCLOG("socket() failed: %s", strerror(errno));
        server_fd_ = -1;
        return false;
    }
    unlink(socket_path_.c_str());
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(server_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        RPCLOG("bind(%s) failed: %s", socket_path_.c_str(), strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }
    if (listen(server_fd_, 1) < 0) {
        RPCLOG("listen() failed: %s", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }
    running_.store(true);
    thread_ = std::thread(&RpcServer::listenLoop, this);
    RPCLOG("listening on %s", socket_path_.c_str());
    return true;
}

void RpcServer::stop() {
    running_.store(false);
    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }
    if (thread_.joinable())
        thread_.join();
    if (!socket_path_.empty())
        unlink(socket_path_.c_str());
}

// ═════════════════════════════════════════════════════════════════════
//  Listen loop — accept one client, serve lines, loop
// ═════════════════════════════════════════════════════════════════════

void RpcServer::listenLoop() {
    while (running_.load()) {
        int client_fd = accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (!running_.load()) break;  // stop() closed the socket
            RPCLOG("accept() failed: %s", strerror(errno));
            continue;
        }
        handleClient(client_fd);
        close(client_fd);
    }
}

void RpcServer::handleClient(int fd) {
    char buf[4096];
    std::string line;
    ssize_t n;
    while (running_.load() &&
           (n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (!line.empty()) {
                    handleLine(fd, line);
                    line.clear();
                }
            } else if (buf[i] != '\r') {
                line += buf[i];
            }
        }
        if (line.size() > 65536) line.clear();  // runaway line guard
    }
}

// ═════════════════════════════════════════════════════════════════════
//  Command dispatch — direct libmpv calls (client API is thread-safe)
// ═════════════════════════════════════════════════════════════════════

void RpcServer::handleLine(int fd, const std::string &line) {
    std::string cmd, error;
    std::vector<std::string> args;
    error = parseCommand(line, cmd, args);
    std::string response;
    if (!error.empty()) {
        response = formatResponse(error);
        response += "\n";
        send(fd, response.c_str(), response.size(), MSG_NOSIGNAL);
        return;
    }

    mpv_handle *h = mpv_ ? mpv_->handle() : nullptr;
    if (!h) {
        response = formatResponse("no mpv handle");
        response += "\n";
        send(fd, response.c_str(), response.size(), MSG_NOSIGNAL);
        return;
    }

    // viewModel 路由：命令经 QueuedConnection 投递主线程执行，立即返回
    // "success"（"命令已接受"语义，与现状一致）；get-vsr 读线程安全的
    // 字符串化方法（只读 atomic）。
    PlayerViewModel *vm = vm_;
    if (cmd == "play") {
        if (!vm) { response = formatResponse("no view model"); }
        else {
            QMetaObject::invokeMethod(vm, "play", Qt::QueuedConnection);
            response = formatResponse("success");
        }
    } else if (cmd == "fullscreen") {
        // 切换全屏（Qt 窗口侧，经 viewModel.toggleFullscreen——mpv
        // fullscreen 属性无客户端观察器，直接发 mpv 命令不生效）
        if (!vm) { response = formatResponse("no view model"); }
        else {
            QMetaObject::invokeMethod(vm, "toggleFullscreen", Qt::QueuedConnection);
            response = formatResponse("success");
        }
    } else if (cmd == "pause") {
        // 无参数 → 确保暂停（viewModel::pause 非 toggle）；yes/no → setPaused
        if (!vm) { response = formatResponse("no view model"); }
        else if (args.empty()) {
            QMetaObject::invokeMethod(vm, "pause", Qt::QueuedConnection);
            response = formatResponse("success");
        } else if (args[0] == "yes") {
            QMetaObject::invokeMethod(vm, "setPaused", Qt::QueuedConnection,
                                      Q_ARG(bool, true));
            response = formatResponse("success");
        } else if (args[0] == "no") {
            QMetaObject::invokeMethod(vm, "setPaused", Qt::QueuedConnection,
                                      Q_ARG(bool, false));
            response = formatResponse("success");
        } else {
            response = formatResponse("invalid value (yes|no)");
        }
    } else if (cmd == "stop") {
        if (!vm) { response = formatResponse("no view model"); }
        else {
            QMetaObject::invokeMethod(vm, "stop", Qt::QueuedConnection);
            response = formatResponse("success");
        }
    } else if (cmd == "quit") {
        if (quit_cb_) quit_cb_();
        response = formatResponse("success");
    } else if (cmd == "resize") {
        // 逐级 resize 压测（非全屏切换）：Queued 路由主线程改窗口尺寸。
        if (args.size() != 2) {
            response = formatResponse("usage: resize <width> <height>");
        } else if (!win_) {
            response = formatResponse("no window");
        } else {
            int w = atoi(args[0].c_str());
            int h = atoi(args[1].c_str());
            if (w <= 0 || h <= 0) {
                response = formatResponse("invalid size");
            } else {
                QMetaObject::invokeMethod(win_, [win = win_, w, h] {
                    win->resize(w, h);
                }, Qt::QueuedConnection);
                response = formatResponse("success");
            }
        }
    } else if (cmd == "seek") {
        if (args.empty()) {
            response = formatResponse("missing position");
        } else if (!vm) {
            response = formatResponse("no view model");
        } else {
            // ± 前缀 → 相对 seek（seekRelative）；第三参数（mpv flags 位）可为
            // absolute/relative 显式覆盖，其余模式（percent/时间串等）明确拒绝。
            // 秒 → 毫秒换算。解析失败（strtod 后 end 指针检查非数字 / inf/nan）
            // 报错。
            const std::string &pos = args[0];
            bool rel = (pos[0] == '+' || pos[0] == '-');
            bool modeOk = true;
            if (args.size() >= 2) {
                if (args[1] == "relative")
                    rel = true;
                else if (args[1] == "absolute")
                    rel = false;
                else
                    modeOk = false;
            }
            if (!modeOk) {
                response = formatResponse(
                    "unsupported seek mode (use absolute/relative seconds or command passthrough)");
            } else {
                char *end = nullptr;
                double secs = strtod(pos.c_str(), &end);
                if (end == pos.c_str() || *end != '\0' || !std::isfinite(secs)) {
                    response = formatResponse("invalid position (use seconds, e.g. 10 or -5)");
                } else {
                    int64_t ms = (int64_t)(secs * 1000.0);
                    if (rel)
                        QMetaObject::invokeMethod(vm, "seekRelative",
                                                  Qt::QueuedConnection,
                                                  Q_ARG(int64_t, ms));
                    else
                        QMetaObject::invokeMethod(vm, "seekAbsolute",
                                                  Qt::QueuedConnection,
                                                  Q_ARG(int64_t, ms));
                    response = formatResponse("success");
                }
            }
        }
    } else if (cmd == "clear-playlist") {
        // 真正清空播放列表（mpv 的 playlist-clear 会保留当前项，见 VM 里说明）。
        if (!vm) response = formatResponse("no view model");
        else {
            QMetaObject::invokeMethod(vm, "playlistClear", Qt::QueuedConnection);
            response = formatResponse("success");
        }
    } else if (cmd == "remove-current") {
        // 从播放列表移除当前条目（不碰磁盘）。
        if (!vm) response = formatResponse("no view model");
        else {
            QMetaObject::invokeMethod(vm, "removeCurrentFromPlaylist", Qt::QueuedConnection);
            response = formatResponse("success");
        }
    } else if (cmd == "delete-current") {
        // 删除当前文件（移入回收站）+ 前进到下一个条目。
        if (!vm) response = formatResponse("no view model");
        else {
            QMetaObject::invokeMethod(vm, "deleteCurrentFile", Qt::QueuedConnection);
            response = formatResponse("success");
        }
    } else if (cmd == "set_property") {
        if (args.size() < 2)
            response = formatResponse("missing arguments");
        else if (!vm)
            response = formatResponse("no view model");
        else
            response = handleSetProperty(args[0], args[1]);
    } else if (cmd == "get_property") {
        if (args.empty())
            response = formatResponse("missing property name");
        else
            response = handleGetProperty(args[0]);
    } else if (cmd == "command") {
        // 通用 mpv 命令透传：{"command":["command","loadfile","<path>","replace"]}
        if (args.empty()) {
            response = formatResponse("missing command");
        } else {
            std::vector<const char *> argv;
            for (const auto &a : args)
                argv.push_back(a.c_str());
            argv.push_back(nullptr);
            int e = mpv_command(h, argv.data());
            response = formatResponse(e < 0 ? mpv_error_string(e) : "success");
        }
    } else if (cmd == "set-vsr") {
        if (args.size() < 2)
            response = formatResponse("missing arguments");
        else if (!vm)
            response = formatResponse("no view model");
        else
            response = handleSetVsr(args[0], args[1]);
    } else if (cmd == "get-vsr") {
        if (args.empty())
            response = formatResponse("missing parameter");
        else
            response = handleGetVsr(args[0]);
    } else if (cmd == "set-fruc") {
        // 插帧档位（viewModel 单一来源，与 UI 同路径——vf-command
        // 直改 filter 会绕过 frucFps_，重播后状态丢失/被重放覆盖）。
        // 合法档位随编译后端（FrucBackend.h）：VFG -1|2|3|4（倍率）/
        // RIFE -1|40|48|60（目标帧率）。
        if (args.empty()) {
            response = formatResponse("missing value");
        } else if (!vm) {
            response = formatResponse("no view model");
        } else {
            char *end = nullptr;
            long v = strtol(args[0].c_str(), &end, 10);
            if (end == args[0].c_str() || *end != '\0' ||
                !fruc::isValidPreset((int)v)) {
                response = formatResponse("invalid value (" +
                                          fruc::presetValuesHelp() + ")");
            } else {
                QMetaObject::invokeMethod(vm, "setFrucFps", Qt::QueuedConnection,
                                          Q_ARG(int, (int)v));
                response = formatResponse("success");
            }
        }
    } else if (cmd == "set-fruc-quality") {
        // 插帧质量档（VFG VideoFrameGeneration 的 mode；viewModel 单一来源，
        // 与 UI 同路径）。合法值随编译后端（FrucBackend.h kModeSelectable）：
        // VFG low|medium|high；RIFE 分支无此档——与 CLI --fruc-quality 一致
        //（接受但忽略 + 警告）。
        if (args.empty()) {
            response = formatResponse("missing value");
        } else if (!vm) {
            response = formatResponse("no view model");
        } else if (!fruc::kModeSelectable) {
            MLOG_WARN("set-fruc-quality %s ignored: not selectable with the RIFE backend",
                      args[0].c_str());
            response = formatResponse("success");
        } else {
            int v;
            if (!fruc::parseMode(args[0], &v)) {
                response = formatResponse("invalid value (" +
                                          fruc::modeValuesHelp() + ")");
            } else {
                QMetaObject::invokeMethod(vm, "setFrucQuality", Qt::QueuedConnection,
                                          Q_ARG(int, v));
                response = formatResponse("success");
            }
        }
    } else if (cmd == "set-fruc-max-fps") {
        // 输出帧率上限（VFG 倍率语义的补丁；viewModel 单一来源，与 UI 同
        // 路径）。VFG 48|60|120；RIFE 分支无此概念——与 CLI --fruc-max-fps
        // 一致（接受但忽略 + 警告）。
        if (args.empty()) {
            response = formatResponse("missing value");
        } else if (!vm) {
            response = formatResponse("no view model");
        } else if (!fruc::kMaxFpsSelectable) {
            MLOG_WARN("set-fruc-max-fps %s ignored: VFG-only (this is the RIFE backend)",
                      args[0].c_str());
            response = formatResponse("success");
        } else {
            char *end = nullptr;
            long v = strtol(args[0].c_str(), &end, 10);
            if (end == args[0].c_str() || *end != '\0' ||
                !fruc::isValidMaxFps((int)v)) {
                response = formatResponse("invalid value (" +
                                          fruc::maxFpsValuesHelp() + ")");
            } else {
                QMetaObject::invokeMethod(vm, "setFrucMaxFps", Qt::QueuedConnection,
                                          Q_ARG(int, (int)v));
                response = formatResponse("success");
            }
        }
    } else if (cmd == "set-deinterlace") {
        // 反交错三态（mpv --deinterlace 的 off/auto/force；viewModel 单一
        // 来源，与右键菜单同路径）。映射到的 mpv 值是 no/auto/yes。
        if (args.empty()) {
            response = formatResponse("missing value (off|auto|force)");
        } else if (!vm) {
            response = formatResponse("no view model");
        } else {
            int m = PlayerViewModel::parseDeintMode(args[0]);
            if (m < 0) {
                response = formatResponse("invalid value (off|auto|force)");
            } else {
                QMetaObject::invokeMethod(vm, "setDeintMode", Qt::QueuedConnection,
                                          Q_ARG(int, m));
                response = formatResponse("success");
            }
        }
    } else if (cmd == "get-deinterlace") {
        if (!vm) {
            response = formatResponse("no view model");
        } else {
            static const char *names[3] = {"off", "auto", "force"};
            int m = vm->deintMode();
            if (m < 0 || m > 2) m = 0;
            response = formatResponse("success", std::string("\"") + names[m] + "\"");
        }
    } else if (cmd == "set-rotate") {
        // 旋转三态（0=Off / 90=顺时针 / 270=逆时针 90°），viewModel 单一
        // 来源，与右键菜单同路径（vf-command rot angle N 运行期生效）。
        if (args.empty()) {
            response = formatResponse("missing value (0|90|270)");
        } else if (!vm) {
            response = formatResponse("no view model");
        } else {
            char *end = nullptr;
            long v = strtol(args[0].c_str(), &end, 10);
            if (end == args[0].c_str() || *end != '\0' ||
                (v != 0 && v != 90 && v != 270)) {
                response = formatResponse("invalid value (0|90|270)");
            } else {
                QMetaObject::invokeMethod(vm, "setRotMode", Qt::QueuedConnection,
                                          Q_ARG(int, (int)v));
                response = formatResponse("success");
            }
        }
    } else if (cmd == "get-rotate") {
        if (!vm) {
            response = formatResponse("no view model");
        } else {
            response = formatResponse("success", std::to_string(vm->rotMode()));
        }
    } else if (cmd == "dump-vsr") {
        // VSR-specific screenshot: one frame, input (original) + output (Nx).
        // Paths auto-generated as [videoname]_[frameno]_[original|Nx].png
        // unless provided: dump-vsr <input_path> <output_path>.
        std::string in_path, out_path;
        if (args.size() >= 2) {
            in_path = args[0];
            out_path = args[1];
        } else {
            std::string base = dumpBaseName(h);
            int64_t frameno = 0;
            mpv_get_property(h, "estimated-frame-number",
                             MPV_FORMAT_INT64, &frameno);
            in_path  = base + "_" + std::to_string(frameno) + "_original.png";
            out_path = base + "_" + std::to_string(frameno) + "_" +
                       dumpScaleLabel(h) + ".png";
        }
        std::string both = in_path + "|" + out_path;
        const char *argv[] = {"vf-command", "vsr", "dump-both",
                              both.c_str(), nullptr};
        int e = mpv_command(h, argv);
        if (e < 0)
            response = formatResponse(mpv_error_string(e));
        else
            response = formatResponse("success");
    } else if (cmd == "vf-command") {
        // 测试/诊断钩子：直通 mpv 的 vf-command（filter, cmd, [arg]）。
        // 用途举例：`vf-command vsr stereo "tab full"` 强制造 3D 每眼映射，
        // 在没有眼镜/多重屏时也能测 auto 倍率与 VSR 成本。
        const char *argv[5] = {"vf-command", nullptr, nullptr, nullptr, nullptr};
        if (!h) {
            response = formatResponse("no mpv handle");
        } else if (args.size() < 2) {
            response = formatResponse("usage: vf-command <filter> <cmd> [arg]");
        } else {
            argv[1] = args[0].c_str();
            argv[2] = args[1].c_str();
            if (args.size() >= 3) argv[3] = args[2].c_str();
            int e = mpv_command(h, argv);
            response = e < 0 ? formatResponse(mpv_error_string(e))
                             : formatResponse("success");
        }
    } else if (cmd == "toggle-osd") {
        // 测试钩子：切管线状态 OSD（= Tab 键的效果；不注入按键）。
        // 2D 走 mpv osd-msg1，3D 走 QML 覆盖层（见 PlayerViewModel::osdPoll）。
        if (!vm) {
            response = formatResponse("no view model");
        } else {
            QMetaObject::invokeMethod(vm, "toggleOsd", Qt::QueuedConnection);
            response = formatResponse("success");
        }
    } else if (cmd == "get-source-format") {
        // 片源格式状态（排障/验证自动判定用）：layout 0=左右 1=上下；half 0/1；
        // eyeMode 0=off 1=左 2=右；hint = 自动判定结果（-1 未判定）。
        if (!vm || !stereo_) {
            response = formatResponse("no view model");
        } else {
            char buf[96];
            snprintf(buf, sizeof buf, "layout=%d half=%d eye=%d hint=%d",
                     stereo_->layout(), stereo_->half() ? 1 : 0,
                     vm->property("eyeMode").toInt(),
                     vm->property("sourceLayoutHint").toInt());
            response = formatResponse("success", "\"" + std::string(buf) + "\"");
        }
    } else if (cmd == "get-video-size") {
        // 滤镜链输出帧尺寸（video-out-params/w×h）—— 验证 vf_eye 裁切/拉伸、
        // VSR 倍率等改动时不用截图，直接读尺寸。
        if (!mpv_) {
            response = formatResponse("no mpv");
        } else {
            int64_t w = 0, h = 0;
            mpv_get_property(mpv_->handle(), "video-out-params/w", MPV_FORMAT_INT64, &w);
            mpv_get_property(mpv_->handle(), "video-out-params/h", MPV_FORMAT_INT64, &h);
            char buf[64];
            snprintf(buf, sizeof buf, "%lldx%lld", (long long)w, (long long)h);
            response = formatResponse("success", "\"" + std::string(buf) + "\"");
        }
    } else if (cmd == "set-eye") {
        // 测试钩子：单眼 2D 输出（vf_eye）。
        // 用法：set-eye off|left|right [sbs|tab] [full|half]
        if (!vm) {
            response = formatResponse("no view model");
        } else if (args.empty()) {
            response = formatResponse("usage: set-eye off|left|right [sbs|tab] [full|half]");
        } else {
            int mode = -1;
            if (args[0] == "off") mode = 0;
            else if (args[0] == "left") mode = 1;
            else if (args[0] == "right") mode = 2;
            if (mode < 0) {
                response = formatResponse("usage: set-eye off|left|right [sbs|tab] [full|half]");
            } else {
                // 省略布局/尺寸时保持当前值（-1），便于只切眼别
                int layout = -1, half = -1;
                if (args.size() > 1)
                    layout = (args[1] == "tab") ? 1 : (args[1] == "sbs" ? 0 : -1);
                if (args.size() > 2)
                    half = (args[2] == "half") ? 1 : (args[2] == "full" ? 0 : -1);
                QMetaObject::invokeMethod(vm, "setEyeState", Qt::QueuedConnection,
                                          Q_ARG(int, mode), Q_ARG(int, layout),
                                          Q_ARG(int, half));
                response = formatResponse("success");
            }
        }
    } else if (cmd == "get-eye") {
        if (!vm) {
            response = formatResponse("no view model");
        } else {
            const int m = vm->property("eyeMode").toInt();
            static const char *kNames[] = {"off", "left", "right"};
            response = formatResponse("success",
                                      "\"" + std::string(kNames[m < 0 || m > 2 ? 0 : m]) + "\"");
        }
    } else if (cmd == "set-stereo-map") {
        // 测试钩子：设置 3D 每眼映射（等价于真 3D 状态下客户端推给 vf_vsr 的值）。
        // 走 viewModel 同一入口，因此会被 syncVfOptions 重放（不会被链重建抹掉）——
        // 没接眼镜时也能测 3D 下的 VSR auto 倍率与成本。
        // 用法：set-stereo-map off | set-stereo-map <sbs|tab> <full|half>
        if (!vm) {
            response = formatResponse("no view model");
        } else if (args.empty()) {
            response = formatResponse("usage: set-stereo-map off|<sbs|tab> <full|half>");
        } else if (args[0] == "off") {
            QMetaObject::invokeMethod(vm, "setStereoMap", Qt::QueuedConnection,
                                      Q_ARG(bool, false), Q_ARG(int, 0), Q_ARG(int, 0));
            response = formatResponse("success");
        } else if (args.size() >= 2 &&
                   (args[0] == "sbs" || args[0] == "tab") &&
                   (args[1] == "full" || args[1] == "half")) {
            QMetaObject::invokeMethod(vm, "setStereoMap", Qt::QueuedConnection,
                                      Q_ARG(bool, true),
                                      Q_ARG(int, args[0] == "tab" ? 1 : 0),
                                      Q_ARG(int, args[1] == "half" ? 1 : 0));
            response = formatResponse("success");
        } else {
            response = formatResponse("usage: set-stereo-map off|<sbs|tab> <full|half>");
        }
    } else if (cmd == "stereo-start") {
        // 3D OSD 原型：stereo-start <mode 1|2> <screen index>（测试用；
        // 正常入口是右键菜单 3D → 选项 → 选择显示器）。
        if (!stereo_) {
            response = formatResponse("no stereo controller");
        } else if (args.size() < 2) {
            response = formatResponse("usage: stereo-start <1|2> <screen-index>");
        } else {
            char *e1 = nullptr, *e2 = nullptr;
            long mode = strtol(args[0].c_str(), &e1, 10);
            long idx  = strtol(args[1].c_str(), &e2, 10);
            if (!e1 || *e1 || !e2 || *e2 || mode != 1 || idx < 0) {
                response = formatResponse("invalid mode/screen (mode 1, index>=0)");
            } else {
                QMetaObject::invokeMethod(stereo_, "start", Qt::QueuedConnection,
                                          Q_ARG(int, (int)mode),
                                          Q_ARG(int, (int)idx));
                response = formatResponse("success");
            }
        }
    } else if (cmd == "stereo-stop") {
        if (!stereo_) {
            response = formatResponse("no stereo controller");
        } else {
            QMetaObject::invokeMethod(stereo_, "stop", Qt::QueuedConnection);
            response = formatResponse("success");
        }
    } else if (cmd == "stereo-help") {
        if (!stereo_) {
            response = formatResponse("no stereo controller");
        } else if (args.empty()) {
            response = formatResponse("missing value (0|1)");
        } else {
            bool v = args[0] == "1" || args[0] == "yes" || args[0] == "true";
            QMetaObject::invokeMethod(stereo_, "setHelpVisible", Qt::QueuedConnection,
                                      Q_ARG(bool, v));
            response = formatResponse("success");
        }
    } else if (cmd == "open-menu") {
        // 测试用：在窗口内 (x,y) 打开右键菜单（正常入口 = 右键）。
        // 用于截图核对菜单排版/分组（尤其二级面板的分组标题与勾选态）。
        auto *qv = qobject_cast<QQuickView *>(win_);
        if (!qv || !qv->rootObject()) {
            response = formatResponse("no window");
        } else if (args.size() < 2) {
            response = formatResponse("usage: open-menu <x> <y> [submenu-index]");
        } else {
            const int x = atoi(args[0].c_str());
            const int y = atoi(args[1].c_str());
            const int sub = args.size() >= 3 ? atoi(args[2].c_str()) : -1;
            QQuickItem *rootItem = qv->rootObject();
            QMetaObject::invokeMethod(rootItem, [rootItem, x, y, sub]() {
                QMetaObject::invokeMethod(rootItem, "openMenuAt",
                                          Q_ARG(QVariant, QVariant(x)),
                                          Q_ARG(QVariant, QVariant(y)),
                                          Q_ARG(QVariant, QVariant(sub)));
            });
            response = formatResponse("success");
        }
    } else if (cmd == "stereo-picker") {
        // 测试用：打开 3D 选择器（正常入口 = 右键菜单 → 3D → 3D片源/2D转3D）
        auto *qv = qobject_cast<QQuickView *>(win_);
        if (!qv || !qv->rootObject()) {
            response = formatResponse("no window");
        } else if (args.empty()) {
            response = formatResponse("missing mode (1|2)");
        } else {
            int mode = atoi(args[0].c_str());
            QQuickItem *rootItem = qv->rootObject();
            QMetaObject::invokeMethod(rootItem, [rootItem, mode]() {
                rootItem->setProperty("stereoPickerMode", mode);
                QMetaObject::invokeMethod(rootItem, "openStereoPicker", Qt::QueuedConnection);
            }, Qt::QueuedConnection);
            response = formatResponse("success");
        }
    } else if (cmd == "grab-window") {
        // 测试用：抓主窗口（锁屏/无眼镜时检查 UI）
        if (!win_) {
            response = formatResponse("no window");
        } else if (args.empty()) {
            response = formatResponse("missing path");
        } else {
            QQuickWindow *win = win_;
            QString path = QString::fromStdString(args[0]);
            QMetaObject::invokeMethod(win, [win, path]() {
                QImage img = win->grabWindow();
                if (!img.isNull())
                    img.save(path);
            }, Qt::QueuedConnection);
            response = formatResponse("success");
        }
    } else if (cmd == "nudge-volume") {
        // 模拟快捷键调音量（触发音量提示 OSD；与 set_property volume 区分）
        if (!vm) {
            response = formatResponse("no view model");
        } else if (args.empty()) {
            response = formatResponse("usage: nudge-volume <delta e.g. 0.05>");
        } else {
            char *end = nullptr;
            double d = strtod(args[0].c_str(), &end);
            if (end == args[0].c_str() || *end != '\0' || !std::isfinite(d))
                response = formatResponse("invalid delta");
            else {
                QMetaObject::invokeMethod(vm, "nudgeVolume", Qt::QueuedConnection,
                                          Q_ARG(double, d));
                response = formatResponse("success");
            }
        }
    } else if (cmd == "get-stereo") {
        // 合成映射数据（不截图判断满不满屏；与日志 `stereo map:` 同源）
        if (!stereo_) {
            response = formatResponse("no stereo controller");
        } else {
            const std::string m = stereo_->mapJson().toStdString();
            response = m.empty() ? formatResponse("no map yet")
                                 : formatResponse("success", "\"" + jsonEscape(m) + "\"");
        }
    } else if (cmd == "stereo-cycle") {
        if (!stereo_) {
            response = formatResponse("no stereo controller");
        } else if (args.empty()) {
            response = formatResponse("missing value (layout|swap|fit)");
        } else if (args[0] == "layout") {
            QMetaObject::invokeMethod(stereo_, "cycleLayout", Qt::QueuedConnection);
            response = formatResponse("success");
        } else if (args[0] == "swap") {
            QMetaObject::invokeMethod(stereo_, "swapEyes", Qt::QueuedConnection);
            response = formatResponse("success");
        } else if (args[0] == "half") {
            QMetaObject::invokeMethod(stereo_, "toggleHalf", Qt::QueuedConnection);
            response = formatResponse("success");
        } else {
            response = formatResponse("invalid value (layout|swap|half)");
        }
    } else {
        response = formatResponse("unknown command");
    }

    response += "\n";
    send(fd, response.c_str(), response.size(), MSG_NOSIGNAL);
}

// ═════════════════════════════════════════════════════════════════════
//  set/get property — mpv native properties
// ═════════════════════════════════════════════════════════════════════

std::string RpcServer::handleSetProperty(const std::string &name, const std::string &value) {
    if (!vm_) return formatResponse("no view model");
    PlayerViewModel *vm = vm_;
    if (name == "pause") {
        if (value == "yes") { QMetaObject::invokeMethod(vm, "setPaused", Qt::QueuedConnection, Q_ARG(bool, true)); return formatResponse("success"); }
        if (value == "no")  { QMetaObject::invokeMethod(vm, "setPaused", Qt::QueuedConnection, Q_ARG(bool, false)); return formatResponse("success"); }
        return formatResponse("invalid value (yes|no)");
    }
    if (name == "speed") {
        char *end = nullptr;
        double v = strtod(value.c_str(), &end);
        if (end == value.c_str() || *end != '\0' || !std::isfinite(v))
            return formatResponse("invalid value");
        QMetaObject::invokeMethod(vm, "setSpeed", Qt::QueuedConnection, Q_ARG(double, v));
        return formatResponse("success");
    }
    if (name == "volume") {
        char *end = nullptr;
        double v = strtod(value.c_str(), &end);
        if (end == value.c_str() || *end != '\0' || !std::isfinite(v))
            return formatResponse("invalid value");
        // viewModel setVolume 是 0..1 语义（QML 绑定），RPC 保持 mpv 0-100 → 边界换算
        QMetaObject::invokeMethod(vm, "setVolume", Qt::QueuedConnection,
                                  Q_ARG(double, v / 100.0));
        return formatResponse("success");
    }
    if (name == "mute") {
        if (value == "yes") { QMetaObject::invokeMethod(vm, "setMuted", Qt::QueuedConnection, Q_ARG(bool, true)); return formatResponse("success"); }
        if (value == "no")  { QMetaObject::invokeMethod(vm, "setMuted", Qt::QueuedConnection, Q_ARG(bool, false)); return formatResponse("success"); }
        return formatResponse("invalid value (yes|no)");
    }
    return formatResponse("unknown property");
}

std::string RpcServer::handleGetProperty(const std::string &name) {
    // dropped-frames: mpv's frame-drop-count (VO drop counter)
    std::string prop = (name == "dropped-frames") ? "frame-drop-count" : name;

    mpv_handle *h = mpv_->handle();
    char *val = nullptr;
    int e = mpv_get_property(h, prop.c_str(), MPV_FORMAT_STRING, &val);
    if (e < 0) {
        // fall back to flag/number formats via string conversion
        if (e == MPV_ERROR_PROPERTY_NOT_FOUND)
            return formatResponse("unknown property");
        return formatResponse(mpv_error_string(e));
    }
    std::string data = "\"" + jsonEscape(val ? val : "") + "\"";
    mpv_free(val);
    return formatResponse("success", data);
}

// ── set-vsr / get-vsr — VSR 参数热更新（viewModel 单一来源）───────────────
// set 路径：字符串在 RPC 线程解析（静态 parse* 方法），经 QueuedConnection
// 投递主线程 setScale/setQuality/setDenoiseQuality（pushVf 热更新，不重建
// filter 链）。get 路径：只读 atomic 的字符串化方法，RPC 线程可直接调用。

std::string RpcServer::handleSetVsr(const std::string &param, const std::string &value) {
    if (!vm_) return formatResponse("no view model");
    PlayerViewModel *vm = vm_;
    if (param == "scale") {
        double s;
        if (!PlayerViewModel::parseScale(value, &s))
            return formatResponse("invalid scale (off|auto|2|3|4|1.5|4/3)");
        QMetaObject::invokeMethod(vm, "setScale", Qt::QueuedConnection, Q_ARG(double, s));
        return formatResponse("success");
    }
    if (param == "quality") {
        int q;
        if (!PlayerViewModel::parseQuality(value, &q))
            return formatResponse("invalid quality (low|medium|high|ultra)");
        QMetaObject::invokeMethod(vm, "setQuality", Qt::QueuedConnection, Q_ARG(int, q));
        return formatResponse("success");
    }
    if (param == "denoise") {
        int d;
        if (!PlayerViewModel::parseDenoise(value, &d))
            return formatResponse("invalid denoise (off|low|medium|high|ultra)");
        QMetaObject::invokeMethod(vm, "setDenoiseQuality", Qt::QueuedConnection, Q_ARG(int, d));
        return formatResponse("success");
    }
    return formatResponse("unknown parameter (scale|quality|denoise)");
}

std::string RpcServer::handleGetVsr(const std::string &param) {
    PlayerViewModel *vm = vm_;
    if (!vm) return formatResponse("no view model");
    // 线程安全：字符串化方法只读 atomic
    if (param == "scale")   return formatResponse("success", "\"" + jsonEscape(vm->scaleString()) + "\"");
    if (param == "quality") return formatResponse("success", "\"" + jsonEscape(vm->qualityString()) + "\"");
    if (param == "denoise") return formatResponse("success", "\"" + jsonEscape(vm->denoiseString()) + "\"");
    return formatResponse("unknown parameter (scale|quality|denoise)");
}

/// [videoname] part of the default dump path: basename without extension.
std::string RpcServer::dumpBaseName(mpv_handle *h) {
    std::string name = "frame";
    char *p = nullptr;
    if (mpv_get_property(h, "path", MPV_FORMAT_STRING, &p) >= 0 && p) {
        std::string full = p;
        mpv_free(p);
        size_t slash = full.find_last_of('/');
        std::string base = (slash != std::string::npos)
                               ? full.substr(slash + 1) : full;
        size_t dot = base.find_last_of('.');
        name = (dot != std::string::npos) ? base.substr(0, dot) : base;
    }
    return name;
}

/// [Nx] part: filter output width / decoded width (VSR scale).
std::string RpcServer::dumpScaleLabel(mpv_handle *h) {
    int64_t out_w = 0, dec_w = 0;
    mpv_get_property(h, "video-out-params/w", MPV_FORMAT_INT64, &out_w);
    mpv_get_property(h, "video-dec-params/w", MPV_FORMAT_INT64, &dec_w);
    if (out_w > 0 && dec_w > 0)
        return std::to_string((out_w + dec_w - 1) / dec_w) + "x";
    return "1x";
}

// ═════════════════════════════════════════════════════════════════════
//  JSON helpers
// ═════════════════════════════════════════════════════════════════════

std::string RpcServer::jsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c; break;
        }
    }
    return out;
}

std::string RpcServer::formatResponse(const std::string &error,
                                      const std::string &data) {
    if (data.empty())
        return "{\"error\":\"" + jsonEscape(error) + "\"}";
    return "{\"error\":\"" + jsonEscape(error) + "\",\"data\":" + data + "}";
}

// ═════════════════════════════════════════════════════════════════════
//  JSON parser — strstr-based, mpv command subset only
// ═════════════════════════════════════════════════════════════════════

std::string RpcServer::parseCommand(const std::string &json_text,
                                    std::string &out_name,
                                    std::vector<std::string> &out_args) {
    out_name.clear();
    out_args.clear();

    const char *p = strstr(json_text.c_str(), "\"command\"");
    if (!p)
        return "no command field";
    p = strchr(p, '[');
    if (!p)
        return "no command array";
    p++;

    auto skip_space = [](const char *s) {
        while (*s && (*s == ' ' || *s == '\t' || *s == '\n')) s++;
        return s;
    };

    p = skip_space(p);
    if (*p != '"')
        return "command name not a string";
    p++;
    const char *name_end = strchr(p, '"');
    if (!name_end)
        return "unterminated command name";
    out_name.assign(p, name_end - p);
    p = name_end + 1;

    while (*p && *p != ']') {
        p = skip_space(p);
        if (*p == ',' || *p == ' ') {
            p++;
            continue;
        }
        if (*p == ']' || *p == '\0')
            break;

        if (*p == '"') {
            p++;
            const char *arg_end = strchr(p, '"');
            if (!arg_end)
                break;
            out_args.emplace_back(p, arg_end - p);
            p = arg_end + 1;
        } else if (*p == 't' || *p == 'f') {
            if (strncmp(p, "true", 4) == 0) {
                out_args.emplace_back("true");
                p += 4;
            } else if (strncmp(p, "false", 5) == 0) {
                out_args.emplace_back("false");
                p += 5;
            } else {
                break;
            }
        } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
            const char *num_start = p;
            if (*p == '-') p++;
            while (*p >= '0' && *p <= '9') p++;
            if (*p == '.') {
                p++;
                while (*p >= '0' && *p <= '9') p++;
            }
            out_args.emplace_back(num_start, p - num_start);
        } else {
            while (*p && *p != ',' && *p != ']') p++;
        }
    }
    return {};
}
