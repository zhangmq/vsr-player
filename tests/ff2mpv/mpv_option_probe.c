// mpv_option_probe.c — libmpv 选项接受度探针
//
// 目的：ff2mpv 集成调研——mpv CLI 依赖的一组选项里，哪些能在 libmpv
// (mpv_set_option_string) 里设置？特别是 cplayer 专有选项（--playlist）。
//
// 构建/运行（仓库根）：
//   gcc -o build/scratch/mpv_option_probe tests/ff2mpv/mpv_option_probe.c \
//       -Ithird_party/mpv/include -Lbuild/mpv/_build -l:libmpv.so \
//       -Wl,-rpath,"$PWD/build/mpv/_build"
//   ./build/scratch/mpv_option_probe
// 输出: 每行 "name=value -> rc=<n> (<error string>)"
#include <mpv/client.h>
#include <stdio.h>

static const struct { const char *n, *v; } kOpts[] = {
    // ff2mpv wrapper (mpv CLI) 里用到的
    {"force-window",                 "yes"},
    {"no-config",                    "yes"},   // CLI 糖：no- 前缀
    {"config",                       "no"},
    {"vo",                           "gpu-next"},
    {"gpu-api",                      "vulkan"},
    {"hwdec",                        "auto-safe"},
    {"script-opts",                  "ytdl_hook-ytdl_path=yt-dlp"},
    {"demuxer-lavf-analyzeduration", "2"},
    {"ytdl-raw-options",             "cookies=/tmp/nope.txt,no-playlist="},
    {"input-terminal",               "no"},
    {"terminal",                     "no"},
    // 本应用自己用的
    {"vo",                           "libmpv"},
    {"hwdec",                        "nvdec"},
    {"vf",                           "hwup,vsr:scale=auto:denoise=off:quality=high,vfg"},
    {"msg-level",                    "all=v"},
    {"osd-level",                    "0"},
    // cplayer / CLI 专有，重点验证
    {"playlist",                     "-"},
    {"pause",                        "yes"},
    {"start",                        "+5.000"},
    {"script",                       "/tmp/nope.lua"},
    {"list-options",                 "yes"},
};

int main(void) {
    for (unsigned i = 0; i < sizeof(kOpts) / sizeof(kOpts[0]); i++) {
        mpv_handle *m = mpv_create();
        if (!m) { printf("mpv_create failed\n"); return 1; }
        int rc = mpv_set_option_string(m, kOpts[i].n, kOpts[i].v);
        printf("%-30s = %-52s -> rc=%-4d %s\n", kOpts[i].n, kOpts[i].v,
               rc, rc < 0 ? mpv_error_string(rc) : "OK");
        mpv_terminate_destroy(m);
    }

    // 第二组：playlist 选项是否只是"接受但无效"？设完后 initialize，
    // 看 mpv 是否真的把 stdin 当播放列表（这里只检验 initialize 是否报错）。
    mpv_handle *m = mpv_create();
    int rc = mpv_set_option_string(m, "playlist", "-");
    printf("\n[initialize 检验] set playlist=- rc=%d, initialize...\n", rc);
    if (rc >= 0) {
        rc = mpv_initialize(m);
        printf("[initialize 检验] mpv_initialize rc=%d %s\n", rc,
               rc < 0 ? mpv_error_string(rc) : "OK");
    }
    mpv_terminate_destroy(m);
    return 0;
}
