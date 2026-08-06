#!/bin/bash
# fs_stress.sh — 全屏切换 × VSR 引擎重建压力复现（RPC 驱动，无需手动操作）
#
# 用途：复现 NVIDIA Xid 109（全屏 modeset 与 VSR 重建并发 → GPU hang）。
# 用法：./tests/fs_stress.sh [rounds] [video] [vsync] [benchmark] [scale]
#   默认 rounds=12，video=input（目录）；vsync=on 加 --vsync（对照）；
#   benchmark=yes 加 --benchmark（untimed 无 A/V 同步，渲染循环自由跑对照）；
#   scale=自定义倍率（默认 auto，如 2 强制 2x——4K 视频 + 高倍率压狠）
# 检测：stderr 的 "Device loss" + 进程存活；失败时输出日志路径。
# 成功（无 device loss）时自动 kill 播放器。

set -u
ROUNDS=${1:-12}
VIDEO=${2:-input}
VSYNC=${3:-off}
BENCH=${4:-no}
SCALE=${5:-auto}
SOCK=/tmp/vsr-player.sock
LOG=/tmp/fs_stress.log
BIN=./build/src/client/vsr-player
VSYNC_ARG=""
[ "$VSYNC" = "on" ] && VSYNC_ARG="--vsync"
BENCH_ARG=""
[ "$BENCH" = "yes" ] && BENCH_ARG="--benchmark"
SCALE_ARG=""
[ "$SCALE" != "auto" ] && SCALE_ARG="--scale $SCALE"

rpc() { printf '%s\n' "$1" | socat - UNIX-CONNECT:$SOCK >/dev/null 2>&1; }

pkill -f "$BIN" 2>/dev/null
sleep 1
# journalctl --since 在本机实测失效（@epoch 与本地格式均静默忽略 →
# 统计全部历史 Xid，恒等于历史总数造成误报）。改用前后计数对比：
# 运行前记 XID_BASE，检测 Xid 数量是否超过基线（新增）。
export LC_ALL=C
XID_BASE=$(journalctl -k --no-pager 2>/dev/null | grep -c "Xid" || true)
# stdbuf -oL：stderr 行缓冲——崩溃日志实时落盘（重定向到文件默认块
# 缓冲，崩溃内容会滞留在 libc 缓冲区导致检测不到）
stdbuf -oL -eL "$BIN" --lang en "$VIDEO" $SCALE_ARG $VSYNC_ARG $BENCH_ARG >"$LOG" 2>&1 &
PID=$!
echo "pid=$PID log=$LOG  rounds=$ROUNDS"
sleep 10   # 等播放稳定（解码 + VSR 初始化 + 首帧渲染）

# GPU 后台采样（诊断）：每 100ms 记录利用率+显存+时间戳——Xid 挂死时
# 对照利用率判断 GPU 是"空闲等待"还是"kernel 挂起"；显存曲线验证
# "多实例累积/销毁不释放 → 显存耗尽 → Load 卡死"假设（延迟销毁方案
# 首次压测 24 轮后挂死，日志显示挂死点在 init 的 Load，用户观察到
# 显存耗尽）。
GPU_LOG=/tmp/fs_stress.gpu
: > "$GPU_LOG"
(
    while true; do
        read -r U M <<< "$(nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv,noheader,nounits 2>/dev/null | tr ',' ' ')"
        echo "$(date +%s.%N) util=$U mem=$M" >> "$GPU_LOG" 2>/dev/null
        sleep 0.1
    done
) &
GPUMON=$!
trap 'kill $GPUMON 2>/dev/null' EXIT

# 日志停滞检测：记录上次日志大小/时间，多轮无增长 = 卡死（GPU hang）
last_size=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
stall=0
fail=0
for i in $(seq 1 "$ROUNDS"); do
    # auto + 全屏切换：显示区域变化 → auto 重算倍率 → VSR 引擎重建，
    # 与全屏 modeset 并发——手动复现场景的精确模拟（set-vsr 固定倍率
    # 会破坏 auto，勿用）。停留 1.5s：render target 需尺寸稳定 30 帧
    # 才重建（RT_STABLE_FRAMES 节流），太短则重建被吞、测不到切换。
    rpc '{"command":["fullscreen"]}'
    sleep 1.5
    rpc '{"command":["fullscreen"]}'
    sleep 1.5

    if ! kill -0 "$PID" 2>/dev/null; then
        echo "ROUND $i: PROCESS EXITED"; fail=1; break
    fi
    if grep -q "Device loss" "$LOG"; then
        echo "ROUND $i: DEVICE LOSS"; fail=1; break
    fi
    # Xid 检测：驱动报错立即 SIGABRT——core 离 hang 点最近（stall 检测
    # 会晚 6s+，core 反映的是 Xid 之后的状态，可能已失真）
    XID_NOW=$(journalctl -k --no-pager 2>/dev/null | grep -c "Xid" || true)
    if [ "${XID_NOW:-0}" -gt "$XID_BASE" ]; then
        echo "ROUND $i: XID DETECTED ($((XID_NOW - XID_BASE)) new) — SIGABRT for core"
        kill -ABRT "$PID" 2>/dev/null
        sleep 3
        if command -v gdb >/dev/null 2>&1; then
            coredumpctl dump -o /tmp/fs_stress.core 2>/dev/null
            gdb -batch -ex "thread apply all bt" \
                "$(realpath "$BIN")" /tmp/fs_stress.core \
                > "$LOG.stack" 2>&1 || true
            echo "thread stacks: $LOG.stack"
        else
            echo "no core dump captured"
        fi
        fail=1; break
    fi
    # 卡死检测：日志无增长（GPU hang 后渲染循环停止，日志冻结）
    cur_size=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
    if [ "$cur_size" -eq "$last_size" ]; then
        stall=$((stall + 1))
        if [ "$stall" -ge 2 ]; then
            echo "ROUND $i: LOG STALLED (GPU hang, render frozen)"
            # 冻结时 SIGABRT 触发 core dump（ptrace 受限时唯一可行的
            # 线程栈途径），用 coredumpctl 提取并 gdb 分析。
            # 注意：vsr-player 无 SIGABRT handler，core 含全部线程栈。
            echo "ROUND $i: LOG STALLED (GPU hang, render frozen)"
            kill -ABRT "$PID" 2>/dev/null
            sleep 3
            COREDUMP=$(coredumpctl -1 --no-pager 2>/dev/null | grep -oE "[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}" | head -1)
            if [ -n "$COREDUMP" ] && command -v gdb >/dev/null 2>&1; then
                coredumpctl dump -o /tmp/fs_stress.core 2>/dev/null
                gdb -batch -ex "thread apply all bt" \
                    "$(realpath "$BIN")" /tmp/fs_stress.core \
                    > "$LOG.stack" 2>&1 || true
                echo "thread stacks: $LOG.stack"
            else
                echo "no core dump captured"
            fi
            fail=1; break
        fi
    else
        stall=0
    fi
    last_size=$cur_size
    echo "round $i ok"
done

# 内核日志 Xid 检测（最可靠：驱动级证据）
XID_CNT=$(( $(journalctl -k --no-pager 2>/dev/null | grep -c "Xid" || true) - XID_BASE ))

if [ "$fail" -eq 0 ] && [ "$XID_CNT" -le 0 ]; then
    echo "PASS: $ROUNDS rounds, no device loss (Xid=$XID_CNT)"
    kill "$PID" 2>/dev/null
else
    echo "FAIL at round $i (Xid=$XID_CNT) — log: $LOG (tail):"
    tail -15 "$LOG"
fi
