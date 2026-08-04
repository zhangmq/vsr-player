#include "Options.h"
#include <cstdio>
#include <cstring>

bool Options::parse(int argc, char *argv[], Options *out) {
    Options o;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            // `--` 之后的所有参数透传给 mpv（键值对，支持 = 与空格分隔）
            for (int j = i + 1; j < argc; j++) {
                const char *a = argv[j];
                if (a[0] != '-' || a[1] != '-') {
                    // 非 --xxx 形式：最后一个非选项是视频文件
                    if (j == argc - 1 && o.video_file.empty()) { o.video_file = a; break; }
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
        if (strcmp(argv[i], "--benchmark") == 0) {
            o.benchmark = true;
            o.vsync = false;  // benchmark 强制关闭 vsync
        } else if (strcmp(argv[i], "--no-hwaccel") == 0) {
            o.hwaccel = false;
        } else if (strcmp(argv[i], "--no-vsync") == 0) {
            o.vsync = false;
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
        } else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            o.lang = argv[++i];
        } else if (strncmp(argv[i], "--lang=", 7) == 0) {
            o.lang = argv[i] + 7;
        } else if (strcmp(argv[i], "--screenshot-dir") == 0 && i + 1 < argc) {
            o.screenshot_dir = argv[++i];
        } else if (strncmp(argv[i], "--screenshot-dir=", 17) == 0) {
            o.screenshot_dir = argv[i] + 17;
        } else if (argv[i][0] != '-') {
            if (!o.video_file.empty()) {
                fprintf(stderr, "error: multiple video files\n");
                return false;
            }
            o.video_file = argv[i];
        } else {
            fprintf(stderr, "error: unknown option: %s\n", argv[i]);
            return false;
        }
    }
    if (o.video_file.empty()) {
        fprintf(stderr,
            "Usage: %s [--benchmark] [--no-hwaccel] [--no-vsync] "
            "[--no-rpc] [--rpc-socket <path>] "
            "[--scale off|auto|2|3|4] [--denoise off|low|medium|high|ultra] "
            "[--quality low|medium|high|ultra] "
            "[--lang <locale>] [--screenshot-dir <dir>] [-- mpv-opt ...] <video>\n",
            argv[0]);
        return false;
    }
    *out = o;
    return true;
}
