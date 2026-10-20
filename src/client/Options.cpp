#include "Options.h"
#include "FrucBackend.h"
#include <cstdio>
#include <cstring>

// ── Usage ────────────────────────────────────────────────────────────
// --fruc 合法值列表来自编译期分支（FrucBackend.h 单一事实源）：
// VFG = off|2|3|4（倍率）；RIFE = off|40|48|60|2|3|4（目标帧率 + benchmark
// 倍率）。--fruc-order 仅 VFG 可切换（RIFE 链序固定），故只在 VFG 分支列出；
// --fruc-quality 同理仅 VFG 可选（RIFE 无质量档）；--fruc-max-fps 同理
//（RIFE 的 --fruc 本身就是目标输出帧率，没有"倍率顶过头"的问题）。
// 值本身的合法性不在本层校验（与 --fruc 同模式：字符串搬运，语义/兜底在
// PlayerViewModel::initFruc/initFrucOrder/initFrucQuality/initFrucMaxFps）。
static void printUsage(FILE *out, const char *prog) {
    const std::string frucVals = fruc::cliValuesHelp();
    std::string orderOpt;
    if (fruc::kOrderSelectable)
        orderOpt = "[--fruc-order pre|post] ";
    std::string qualityOpt;
    if (fruc::kModeSelectable)
        qualityOpt = "[--fruc-quality " + fruc::modeValuesHelp() + "] ";
    std::string maxFpsOpt;
    if (fruc::kMaxFpsSelectable)
        maxFpsOpt = "[--fruc-max-fps " + fruc::maxFpsValuesHelp() + "] ";
    fprintf(out,
        "Usage: %s [--benchmark] [--no-hwaccel] [--vsync] "
        "[--no-rpc] [--rpc-socket <path>] "
        "[--scale off|auto|2|3|4] [--denoise off|low|medium|high|ultra] "
        "[--quality low|medium|high|ultra] "
        "[--fruc %s] %s%s%s"
        "[--lang <locale>] [--screenshot-dir <dir>] [--playlist <file|->] "
        "[-- mpv-opt ...] <video ...>\n",
        prog, frucVals.c_str(), orderOpt.c_str(), qualityOpt.c_str(),
        maxFpsOpt.c_str());
}

bool Options::parse(int argc, char *argv[], Options *out) {
    Options o;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            // `--` 之后的所有参数透传给 mpv（键值对，支持 = 与空格分隔）；
            // 非选项参数与前面一样是播放列表条目（mpv 语义：位置参数即条目）
            for (int j = i + 1; j < argc; j++) {
                const char *a = argv[j];
                if (a[0] != '-' || a[1] != '-') {
                    o.inputs.push_back({false, a});
                    if (o.video_file.empty()) o.video_file = a;
                    continue;
                }
                a += 2;
                const char *eq = strchr(a, '=');
                if (eq) {
                    o.passthrough.emplace_back(std::string(a, eq - a), std::string(eq + 1));
                } else {
                    if (j + 1 < argc && argv[j + 1][0] != '-')
                        o.passthrough.emplace_back(std::string(a), std::string(argv[++j]));
                    else
                        o.passthrough.emplace_back(std::string(a), std::string("yes"));
                }
            }
            break;
        }
        if (strcmp(argv[i], "--help") == 0) {
            o.help = true;
            continue;
        }
        if (strcmp(argv[i], "--benchmark") == 0) {
            o.benchmark = true;
            o.vsync = false;  // benchmark 强制关闭 vsync
        } else if (strcmp(argv[i], "--no-hwaccel") == 0) {
            o.hwaccel = false;
        } else if (strcmp(argv[i], "--vsync") == 0) {
            o.vsync = true;  // 显式开启 FIFO present（默认 no）
        } else if (strcmp(argv[i], "--no-vsync") == 0) {
            o.vsync = false;  // 兼容 no-op（默认已是 no）
        } else if (strcmp(argv[i], "--no-rpc") == 0) {
            o.rpc = false;
        } else if (strcmp(argv[i], "--rpc-socket") == 0 && i + 1 < argc) {
            o.rpc_socket = argv[++i];
        } else if (strncmp(argv[i], "--rpc-socket=", 13) == 0) {
            o.rpc_socket = argv[i] + 13;
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            o.scale = argv[++i];
        } else if (strncmp(argv[i], "--scale=", 8) == 0) {
            o.scale = argv[i] + 8;
        } else if (strcmp(argv[i], "--denoise") == 0 && i + 1 < argc) {
            o.denoise = argv[++i];
        } else if (strncmp(argv[i], "--denoise=", 10) == 0) {
            o.denoise = argv[i] + 10;
        } else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
            o.quality = argv[++i];
        } else if (strncmp(argv[i], "--quality=", 10) == 0) {
            o.quality = argv[i] + 10;
        } else if (strcmp(argv[i], "--fruc") == 0 && i + 1 < argc) {
            o.fruc = argv[++i];
        } else if (strncmp(argv[i], "--fruc=", 7) == 0) {
            o.fruc = argv[i] + 7;
        } else if (strcmp(argv[i], "--fruc-order") == 0 && i + 1 < argc) {
            o.fruc_order = argv[++i];
        } else if (strncmp(argv[i], "--fruc-order=", 13) == 0) {
            o.fruc_order = argv[i] + 13;
        } else if (strcmp(argv[i], "--fruc-quality") == 0 && i + 1 < argc) {
            o.fruc_quality = argv[++i];
        } else if (strncmp(argv[i], "--fruc-quality=", 15) == 0) {
            o.fruc_quality = argv[i] + 15;
        } else if (strcmp(argv[i], "--fruc-max-fps") == 0 && i + 1 < argc) {
            o.fruc_max_fps = argv[++i];
        } else if (strncmp(argv[i], "--fruc-max-fps=", 15) == 0) {
            o.fruc_max_fps = argv[i] + 15;
        } else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            o.lang = argv[++i];
        } else if (strncmp(argv[i], "--lang=", 7) == 0) {
            o.lang = argv[i] + 7;
        } else if (strcmp(argv[i], "--screenshot-dir") == 0 && i + 1 < argc) {
            o.screenshot_dir = argv[++i];
        } else if (strncmp(argv[i], "--screenshot-dir=", 17) == 0) {
            o.screenshot_dir = argv[i] + 17;
        } else if (strcmp(argv[i], "--playlist") == 0 && i + 1 < argc) {
            // 播放列表文件（"-" = stdin）。展开在读入时做（main.cpp）。
            o.inputs.push_back({true, argv[++i]});
        } else if (strncmp(argv[i], "--playlist=", 11) == 0) {
            o.inputs.push_back({true, argv[i] + 11});
        } else if (argv[i][0] != '-') {
            // 位置参数 = 播放列表条目（mpv 语义：`vsr-player a b` 两条，
            // 不再报 "multiple video files"）
            o.inputs.push_back({false, argv[i]});
            if (o.video_file.empty()) o.video_file = argv[i];
        } else {
            fprintf(stderr, "error: unknown option: %s\n", argv[i]);
            return false;
        }
    }
    // --help：只列用法（其余参数忽略），main 据此直接退出（不改工作区/不启引擎）
    if (o.help) {
        printUsage(stdout, argv[0]);
        *out = o;
        return true;
    }
    if (o.inputs.empty()) {
        if (o.benchmark) {
            printUsage(stderr, argv[0]);
            return false;
        }
        // 无参数正常启动：恢复上次播放列表（main.cpp restorePlaylist 分支）。
        // benchmark 必须显式给输入——测量无列表可恢复。
    }
    *out = o;
    return true;
}
