// mpv_defaults_probe.c — libmpv 默认值探针
//
// 回答"libmpv 起手时这些选项是什么值"（ff2mpv 接入相关：ytdl / terminal /
// keep-open / idle / input-terminal / config）。必须在 mpv_initialize() 之后
// 才能读属性——未初始化时 mpv_get_property_string 一律失败。
//
// 构建/运行（仓库根）：
//   gcc -o build/scratch/mpv_defaults_probe tests/ff2mpv/mpv_defaults_probe.c \
//       -Ithird_party/mpv/include -Lbuild/mpv/_build -l:libmpv.so \
//       -Wl,-rpath,"$PWD/build/mpv/_build"
//   ./build/scratch/mpv_defaults_probe
#include <mpv/client.h>
#include <stdio.h>

static const char *kProps[] = {
    "ytdl", "load-scripts", "terminal", "config", "idle", "keep-open",
    "input-terminal", "input-default-bindings", "input-vo-keyboard",
    "audio", "pause", "resume-playback", "save-position-on-quit",
    "sub-auto", "autoload-files", "vo", "hwdec", "force-window",
    "demuxer-lavf-analyzeduration", "osd-level", "msg-level", "playlist",
};

int main(void) {
    mpv_handle *m = mpv_create();
    if (!m) { printf("mpv_create failed\n"); return 1; }
    int rc = mpv_initialize(m);
    if (rc < 0) {
        printf("mpv_initialize failed: %s\n", mpv_error_string(rc));
        return 1;
    }
    for (unsigned i = 0; i < sizeof(kProps) / sizeof(kProps[0]); i++) {
        char *s = mpv_get_property_string(m, kProps[i]);
        printf("%-30s = %s\n", kProps[i], s ? s : "(get failed)");
        mpv_free(s);
    }
    mpv_terminate_destroy(m);
    return 0;
}
