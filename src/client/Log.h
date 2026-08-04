#pragma once

/// client 侧分级日志（mpv 日志为单向输出、不可注入，故独立轻量体系）。
///
/// 级别语义与 mpv 对齐（默认输出 >= INFO）：
///   Err  —— 致命/断言类（不可恢复）
///   Warn —— 可恢复问题、降级
///   Info —— 状态变更节点（启动流程、模式切换等）
///
/// benchmark 模式全静默（vsr_log_set_quiet(true)，与 mpv msg-level
/// all=no 对齐——"benchmark 无日志"是测量口径的一部分）。
void vsr_log_set_quiet(bool q);
void vsr_log(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

// 级别常量（与 mpv msglvl 对齐：越大越啰嗦）
#define VSR_LOG_ERR 0
#define VSR_LOG_WARN 1
#define VSR_LOG_INFO 2

#define MLOG_ERR(...)  vsr_log(VSR_LOG_ERR, __VA_ARGS__)
#define MLOG_WARN(...) vsr_log(VSR_LOG_WARN, __VA_ARGS__)
#define MLOG_INFO(...) vsr_log(VSR_LOG_INFO, __VA_ARGS__)
