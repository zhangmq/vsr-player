#include "Log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace {

double g_t0 = 0.0;      // 相对启动时刻（首个日志触发基准）
bool g_quiet = false;   // benchmark 全静默（与 mpv msg-level all=no 对齐）

} // namespace

void vsr_log_set_quiet(bool q) { g_quiet = q; }

void vsr_log(int level, const char *fmt, ...)
{
    if (g_quiet)
        return;
    using namespace std::chrono;
    double t = duration<double>(steady_clock::now().time_since_epoch()).count();
    if (g_t0 == 0.0)
        g_t0 = t;
    const char *lvl = level >= VSR_LOG_INFO ? "info" :
                      level >= VSR_LOG_WARN ? "warn" : "err";
    fprintf(stderr, "[vsr %7.0fms][%s] ", (t - g_t0) * 1000.0, lvl);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
