#pragma once
#include <vector>
#include <string>

/// Parsed command-line options. Pure data — no logic beyond parsing.
struct Options {
    bool benchmark = false;
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
    // 插帧：正常模式 "off"|"30"|"40"|"60"（目标 fps）；benchmark 模式
    // "2"|"3"|"4"（倍率硬跑，测设备能力）。同一选项，语义随模式。
    std::string fruc;
    // 插帧模型：""|"lite"（默认）|"full"（RIFE 4.25 full，启动时固定选择）
    std::string rife_model;
    std::vector<std::pair<std::string, std::string>> passthrough;  // (key, value) from `--`
    std::string video_file;
    std::string lang;           // UI language override (e.g. "zh_CN", "en"); empty = system locale
    std::string screenshot_dir; // screenshot output dir; empty = mpv default

    /// Parse argc/argv. Returns true on success; on failure prints
    /// usage to stderr and returns false.
    static bool parse(int argc, char *argv[], Options *out);
};
