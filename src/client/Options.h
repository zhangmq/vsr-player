#pragma once
#include <vector>
#include <string>

/// Parsed command-line options. Pure data — no logic beyond parsing.
struct Options {
    bool benchmark = false;
    bool help = false;          // --help：打印用法后由 main 直接退出（不启引擎）
    bool hwaccel = true;
    // 默认 off（非阻塞提交）：wayland 下 FIFO 排队 + 无 present_sync
    // 反馈 → 启动播放帧 drop（见 vo_libmpv 无 get_vsync）。显式 --vsync
    // 开启 FIFO（X11 防撕裂场景）。
    bool vsync = false;
    bool rpc = true;            // JSON IPC server on
    std::string rpc_socket = "/tmp/vsr-player.sock";
    std::string scale;          // "off" | "auto" | "2" | "3" | "4" (empty = unset)
    std::string denoise;        // "off" | "low" | "medium" | "high" | "ultra"
    std::string quality;        // "low" | "medium" | "high" | "ultra"
    // 插帧 --fruc（合法值随编译后端，见 FrucBackend.h）：VFG 分支
    // "off"|"2"|"3"|"4"（倍率）；RIFE 分支 "off"|"40"|"48"|"60"（目标 fps）
    // 与 "2"|"3"|"4"（benchmark 倍率硬跑）。
    std::string fruc;
    // 插帧/超分链序 --fruc-order："pre"|"post"（默认 post = 插帧后置）。
    // 仅 VFG 分支可切换；RIFE 分支接受但忽略（initFrucOrder 提示）。
    std::string fruc_order;
    // 插帧质量档 --fruc-quality："low"|"medium"|"high"（默认 medium）。
    // 仅 VFG 分支可选（值 = VFG 的 mode）；RIFE 分支接受但忽略
    //（与 --fruc-order 同模式：此处只搬字符串，语义/兜底在
    // PlayerViewModel::initFrucQuality；合法值见 FrucBackend.h）。
    std::string fruc_quality;
    // 输出帧率上限 --fruc-max-fps："48"|"60"|"120"（默认 120）。仅 VFG
    // 分支可选（倍率插帧后帧率超过上限 → 直通）；RIFE 分支接受但忽略。
    // 同 --fruc-order 模式：此处只搬字符串，语义/兜底在
    // PlayerViewModel::initFrucMaxFps；合法值见 FrucBackend.h。
    std::string fruc_max_fps;
    std::vector<std::pair<std::string, std::string>> passthrough;  // (key, value) from `--`
    // ── 输入列表（mpv 语义）────────────────────────────────────────────
    // 位置参数与 `--playlist <file|->` 按**出现顺序**登记；多个位置参数 =
    // 多条播放列表项（不再报 "multiple video files"）。`--playlist` 的展开
    //（读文件/stdin）在 main 里做——本层保持纯字符串解析（不做 I/O）。
    struct Input {
        bool is_playlist = false;   // true: value = 播放列表路径（"-" = stdin）
        std::string value;
    };
    std::vector<Input> inputs;
    // 第一个非 playlist 输入（兼容旧调用点：无 --playlist 时的 `<video>`）；
    // 只给了 --playlist 时为空。
    std::string video_file;
    std::string lang;           // UI language override (e.g. "zh_CN", "en"); empty = system locale
    std::string screenshot_dir; // screenshot output dir; empty = mpv default

    /// Parse argc/argv. Returns true on success; on failure prints
    /// usage to stderr and returns false.
    static bool parse(int argc, char *argv[], Options *out);
};
