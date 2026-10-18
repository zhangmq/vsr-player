// mpv_terminal_probe.c — libmpv 下 mpv 的"终端状态行"是否可用（ff2mpv wrapper 监控通道调研）
//
// 背景：用户 mpv wrapper 靠 stdout 上的 `AV: 00:00:00 / … A-V: 0.000` 行判断
// "开始播放"。libmpv 默认 terminal=no（`mpv --show-profile=libmpv`），本探针
// 验证 `terminal=yes + input-terminal=no` 时，libmpv 应用能否拿到同样的行、
// 且它写到 **stdout**（wrapper 监控的就是 stdout）。
//
// 构建/运行（仓库根；cwd 必须是仓库根，里面引用 input/goose_480p.webm）：
//   gcc -o build/scratch/mpv_terminal_probe tests/ff2mpv/mpv_terminal_probe.c \
//       -Ithird_party/mpv/include -Lbuild/mpv/_build -l:libmpv.so \
//       -Wl,-rpath,"$PWD/build/mpv/_build"
//   ./build/scratch/mpv_terminal_probe 2>/dev/null     # 只看 stdout
#include <mpv/client.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    mpv_handle *m = mpv_create();
    if (!m) return 1;
    mpv_set_option_string(m, "terminal", "yes");
    mpv_set_option_string(m, "input-terminal", "no");
    mpv_set_option_string(m, "config", "no");
    mpv_set_option_string(m, "vo", "null");
    mpv_set_option_string(m, "ao", "null");
    mpv_set_option_string(m, "msg-level", "all=status");
    if (mpv_initialize(m) < 0) { printf("initialize failed\n"); return 1; }

    const char *cmd[] = {"loadfile", "input/goose_480p.webm", NULL};
    mpv_command(m, cmd);

    // 播放 ~4 秒（不阻塞在事件上，只为让状态行刷出来）
    for (int i = 0; i < 40; i++) {
        usleep(100000);
        mpv_wait_event(m, 0);
    }
    const char *stop[] = {"quit", NULL};
    mpv_command(m, stop);
    mpv_terminate_destroy(m);
    return 0;
}
