#!/usr/bin/env python3
"""T2+T3 运行期热键测试（需要图形会话；脚本自己启动/关闭播放器）

为什么要这一层：改版前的三个用户报障（菜单打开时 ↑↓ 还在调音量、
`Right/Left` 没反应、二级面板"上下"没有可见效果）都是**真实按键投递**
下的问题，静态检查与状态机钩子都看不出来。这里用 RPC `key <序列>` 注入
真实按键，走完整链路：应用级过滤器（KeyRouter）→ 窗口 → 焦点项
（弹窗的 Keys.onPressed 也在这条链上）。

覆盖：
  T3-a 窗口级键：音量 / 快进快退 / 字幕延迟 / 音频延迟 / 播放暂停 / 播放列表
  T3-b 打开菜单的三种键：Menu、Shift+F10、F10
  T3-c 弹窗让位（用户报过的 bug 类）：菜单开着时 ↓ 不许调音量、P 不许开面板
  T3-d 单眼态（set-eye）走 3D 键位分支且只派发一次
  T2   菜单状态机：Home/End/↑↓ 跳隐藏行、→/← 开合面板、Enter 进控件态、
       控件态 ←/→ ±50ms、Esc 两级退出

真 3D 会话（副屏当眼镜屏）另见 test_stereo_keys.py。

用法：./tests/hotkeys/test_hotkeys.py [-v] [--keep] [--no-eye]
退出码：0 = 通过。
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness
from harness import (Player, check, ensure_fixture, expect_eq, nudge, section,
                     settle, wait_until, wait_value)

SOCK = Path("/tmp/vsr-hotkeys.sock")
LOG = Path("/tmp/vsr-hotkeys.log")
FIXTURE = Path("/tmp/vsr-hotkey-fixture.mp4")

keep = "--keep" in sys.argv
no_eye = "--no-eye" in sys.argv


def t3_window_keys(p: Player) -> None:
    section("T3-a 窗口级按键（真实投递）")
    rpc = p.rpc

    # 音量：↓/↑ 各 5%
    rpc.call("set_property", "volume", "100")
    wait_until(lambda: abs(rpc.prop("volume") - 100) < 0.6, "音量置 100")
    rpc.key("Down")
    wait_until(lambda: rpc.prop("volume") < 96, "↓ 降低音量")
    rpc.key("Up")
    wait_until(lambda: rpc.prop("volume") > 99, "↑ 恢复音量")
    check(True, "↑/↓ 调音量")
    check("id=volume-up" in p.last_dispatch(), "↑ 派发到 volume-up",
          p.last_dispatch())

    # 快进：→ 5 秒（精确 seek 有耗时，按区间断言；播放中做，避免"暂停态
    # 是否出帧/是否发 PLAYBACK_RESTART"这些与热键无关的变量干扰）
    rpc.call("set_property", "pause", "no")
    wait_until(lambda: rpc.call("get_property", "pause")["data"] == "no", "恢复播放")
    rpc.call("seek", "2", "absolute")
    wait_until(lambda: 1.5 <= rpc.prop("time-pos") <= 3.5, "定位到 2s")
    time.sleep(0.3)
    before, after = nudge(p, "Right", +1)
    check(4.0 <= after - before <= 7.5, "→ 前进约 5 秒", f"{before:.1f}→{after:.1f}")
    before2, back = nudge(p, "Left", -1)
    check(4.0 <= before2 - back <= 7.5, "← 后退约 5 秒", f"{before2:.1f}→{back:.1f}")

    # 字幕延迟：z / Shift+Z / Ctrl+0（分开放置，不要一起归零）
    rpc.key("Ctrl+0")
    wait_until(lambda: p.menu()["subDelay"] == 0, "字幕延迟归零")
    rpc.key("z")
    wait_until(lambda: p.menu()["subDelay"] == -50, "z → -50ms", timeout=4)
    rpc.key("z")
    wait_until(lambda: p.menu()["subDelay"] == -100, "z 再按 → -100ms", timeout=4)
    rpc.key("Shift+Z")
    wait_until(lambda: p.menu()["subDelay"] == -50, "Shift+Z → -50ms", timeout=4)
    rpc.key("Ctrl+0")
    wait_until(lambda: p.menu()["subDelay"] == 0, "Ctrl+0 → 0", timeout=4)
    check(True, "字幕延迟 z / Shift+Z / Ctrl+0")

    # 音频延迟：Ctrl++ / Ctrl+= / Ctrl+- / Ctrl+Alt+0
    rpc.key("Ctrl++")
    wait_until(lambda: p.menu()["audioDelay"] == 50, "Ctrl++ → +50ms", timeout=4)
    rpc.key("Ctrl+-")
    wait_until(lambda: p.menu()["audioDelay"] == 0, "Ctrl+- → 0", timeout=4)
    rpc.key("Ctrl+=")
    wait_until(lambda: p.menu()["audioDelay"] == 50, "Ctrl+= 别名 → +50ms", timeout=4)
    rpc.key("Ctrl+Alt+0")
    wait_until(lambda: p.menu()["audioDelay"] == 0, "Ctrl+Alt+0 → 0", timeout=4)
    check(True, "音频延迟 Ctrl++ / Ctrl+= / Ctrl+- / Ctrl+Alt+0")

    # 字幕/音频互不影响
    rpc.key("z")
    wait_until(lambda: p.menu()["subDelay"] == -50, "z 只改字幕", timeout=4)
    expect_eq(p.menu()["audioDelay"], 0, "调字幕不动音频延迟")
    rpc.key("Ctrl+0")
    wait_until(lambda: p.menu()["subDelay"] == 0, "字幕归零", timeout=4)

    # 播放/暂停
    was_paused = rpc.call("get_property", "pause").get("data")
    rpc.key("Space")
    wait_until(lambda: rpc.call("get_property", "pause")["data"] != was_paused,
               "Space 切播放态")
    rpc.key("Space")
    wait_until(lambda: rpc.call("get_property", "pause")["data"] == was_paused,
               "Space 切回")
    check(True, "Space 播放/暂停")

    # 播放列表：P 开、Esc 关
    rpc.key("P")
    wait_until(lambda: p.menu()["playlist"] == 1, "P 打开播放列表")
    rpc.key("Escape")
    wait_until(lambda: p.menu()["playlist"] == 0, "Esc 关闭播放列表")
    check(True, "P / Esc 播放列表面板")


def t3_menu_openers(p: Player) -> None:
    section("T3-b 打开右键菜单的三种键")
    for seq in ("Menu", "Shift+F10", "F10"):
        p.close_menu()
        p.rpc.key(seq)
        ok = False
        try:
            wait_until(lambda: p.menu()["ctxVisible"] == 1, f"{seq} 打开菜单", timeout=4)
            ok = True
        except AssertionError:
            ok = False
        check(ok, f"{seq} 打开右键菜单")
    p.close_menu()


def t3_popup_gating(p: Player) -> None:
    section("T3-c 弹窗让位（用户报过的 bug 类）")
    rpc = p.rpc
    rpc.call("set_property", "volume", "80")
    wait_until(lambda: abs(rpc.prop("volume") - 80) < 0.6, "音量置 80")

    p.open_menu()
    rpc.clear_key_log()
    rpc.key("Down")
    wait_until(lambda: "menu-down" in (p.last_dispatch() or ""), "↓ 走菜单导航", timeout=4)
    time.sleep(0.4)   # 给"错误分支"（调音量）留出发生窗口
    expect_eq(round(rpc.prop("volume")), 80, "菜单开着时 ↓ 不改音量")
    check("popup=1" in p.last_dispatch(), "↓ 记录为弹窗内派发", p.last_dispatch())

    # P 也不能开播放列表（窗口级动作全部让位）
    rpc.key("P")
    time.sleep(0.4)
    expect_eq(p.menu()["playlist"], 0, "菜单开着时 P 不开播放列表")

    # 但延迟动作照旧可用（popupOk）
    rpc.key("Ctrl+0")
    wait_until(lambda: p.menu()["subDelay"] == 0, "菜单内 Ctrl+0 归零", timeout=4)
    rpc.key("z")
    wait_until(lambda: p.menu()["subDelay"] == -50, "菜单内 z → -50ms", timeout=4)
    check("popup=1" in p.last_dispatch(), "延迟键记录为弹窗内派发",
          p.last_dispatch())
    rpc.key("Ctrl+0")
    wait_until(lambda: p.menu()["subDelay"] == 0, "菜单内归零", timeout=4)
    p.close_menu()
    expect_eq(p.menu()["popups"], 0, "关菜单后弹窗计数归零")


def t2_menu_state_machine(p: Player) -> None:
    section("T2 菜单状态机")
    rpc = p.rpc
    p.open_menu()
    st = settle(p)
    expect_eq(st["cur"], -1, "刚打开时没有当前行")

    rpc.key("Home")
    st = settle(p)
    check(st["cur"] == 0, "Home 到第一行", st["cur"])
    rpc.key("Down")
    st = settle(p)
    check(st["cur"] > 0, "↓ 下移", st["cur"])
    down_one = st["cur"]
    rpc.key("Up")
    st = settle(p)
    check(st["cur"] < down_one, "↑ 上移", st["cur"])

    rpc.key("End")
    st_end = settle(p)
    check(st_end["cur"] > down_one, "End 到末行", st_end["cur"])
    rpc.key("Up")
    st = settle(p)
    check(st["cur"] < st_end["cur"], "↑ 从末行上移", f'{st_end["cur"]}→{st["cur"]}')

    # ── 有二级面板的行：→ 展开 / ← 收起（不执行）────────────────
    rpc.key("Home")
    settle(p)
    opened = False
    row_with_sub = ""
    for _ in range(40):
        rpc.key("Down")
        st = settle(p)
        rpc.key("Right")
        st = settle(p)
        if st["subOpen"]:
            opened = True
            row_with_sub = st["currentRow"]
            break
        rpc.key("Left")
        settle(p)
    check(opened, "→ 展开二级面板", row_with_sub)
    rpc.key("Left")
    st = settle(p)
    expect_eq(st["subOpen"], 0, "← 收起二级面板")
    expect_eq(st["ctxVisible"], 1, "开合面板不关菜单")

    # ── 延迟行：Enter 进控件态 → ←/→ ±50ms → Enter 退出 ─────────
    rpc.key("Home")
    settle(p)
    landed = False
    for _ in range(40):
        st = settle(p)
        row = st["currentRow"].lower()
        if "delay" in row and "subtitle" in row:
            landed = True
            break
        rpc.key("Down")
    check(landed, "导航到 Subtitle delay 行", p.menu()["currentRow"])

    rpc.key("Ctrl+0")
    wait_until(lambda: p.menu()["msSub"] == 0, "先归零", timeout=4)
    rpc.key("Enter")
    st = settle(p)
    expect_eq(st["ctl"], 1, "Enter 进控件态")
    rpc.key("Right")
    wait_until(lambda: p.menu()["msSub"] == 50, "控件态 → +50ms", timeout=4)
    rpc.key("Right")
    wait_until(lambda: p.menu()["msSub"] == 100, "控件态 → +100ms", timeout=4)
    rpc.key("Left")
    wait_until(lambda: p.menu()["msSub"] == 50, "控件态 ← -50ms", timeout=4)
    rpc.key("Enter")
    st = settle(p)
    expect_eq(st["ctl"], 0, "Enter 退出控件态")
    rpc.key("Ctrl+0")
    wait_until(lambda: p.menu()["msSub"] == 0, "Ctrl+0 归零", timeout=4)
    check(True, "延迟行：Enter 控件态 + ←/→ ±50ms")

    # 方向键只导航不执行：在延迟行按 ←/→（非控件态）不调值
    rpc.key("Right")
    settle(p)
    rpc.key("Left")
    settle(p)
    expect_eq(p.menu()["msSub"], 0, "非控件态 ←/→ 不改延迟")

    # ── Esc 分级退出 ────────────────────────────────────────────
    rpc.key("Enter")
    st = settle(p)
    expect_eq(st["ctl"], 1, "再次进控件态")
    rpc.key("Escape")
    st = settle(p)
    expect_eq(st["ctl"], 0, "Esc 先退控件态")
    expect_eq(st["ctxVisible"], 1, "第一级 Esc 不关菜单")

    p.open_menu() if not p.menu()["ctxVisible"] else None
    opened = False
    if p.menu()["ctxVisible"]:
        rpc.key("Home")
        settle(p)
        for _ in range(40):
            rpc.key("Down")
            settle(p)
            rpc.key("Right")
            st = settle(p)
            if st["subOpen"]:
                opened = True
                break
            rpc.key("Left")
            settle(p)
    if opened:
        rpc.key("Escape")
        st = settle(p)
        expect_eq(st["subOpen"], 0, "Esc 收起面板")
        expect_eq(st["ctxVisible"], 1, "第二级 Esc 仍不关菜单")
    rpc.key("Escape")
    st = settle(p)
    expect_eq(st["ctxVisible"], 0, "最后一级 Esc 关菜单")
    expect_eq(st["popups"], 0, "关菜单后弹窗计数归零")

    # ── 两条入口等价：RPC menu-key 与真实按键 ───────────────────
    p.open_menu()
    rpc.call("menu-key", "down")
    st = settle(p)
    check(st["cur"] >= 0, "menu-key 入口生效", st["cur"])
    rpc.key("Home")
    st = settle(p)
    expect_eq(st["cur"], 0, "真实按键入口生效")
    check(True, "menu-key 与真实按键等价")
    p.close_menu()


def t3_stereo_branch(p: Player) -> None:
    """单眼 2D 输出走"3D 键位分支"（改版前过滤器是 stereo.active || eyeMode）：
    这里验证 mode3d 的匹配确实生效，且没有和 2D 键位重复触发。"""
    section("T3-d 单眼态的 3D 键位分支")
    rpc = p.rpc
    rpc.call("set-eye", "left")
    wait_until(lambda: p.menu()["eye"] != 0, "进入单眼态", timeout=5)
    try:
        rpc.clear_key_log()
        rpc.key("Tab")
        wait_until(lambda: "id=toggle-osd" in (p.rpc.key_log() or [""])[-1],
                   "Tab 派发", timeout=4)
        check("scope=3d" in p.last_dispatch(), "单眼态按 3D 键位表匹配",
              p.last_dispatch())
        # 同一次按键不能被派发两次（2D 与 3D 两张表重叠的经典回归）
        time.sleep(0.3)
        hits = [ln for ln in rpc.key_log() if "id=toggle-osd" in ln]
        check(len(hits) == 1, "同一次按键只派发一次", hits)

        rpc.clear_key_log()
        rpc.key("Space")
        wait_until(lambda: "id=play-pause" in (rpc.key_log() or [""])[-1],
                   "Space 派发", timeout=4)
        check("scope=3d" in p.last_dispatch(), "Space 走 3D 分支", p.last_dispatch())
    finally:
        rpc.call("set-eye", "off")
        wait_until(lambda: p.menu()["eye"] == 0, "退出单眼态", timeout=5)


def t_sanity(p: Player) -> None:
    section("一致性")
    table = p.rpc.call("get-keys")["data"]
    check(len(table) >= 40, "get-keys 返回完整表", len(table))
    log = p.log_text()
    check("self-check OK" in log, "启动自检通过")
    check("QML error" not in log, "无 QML 错误")
    check("Ambiguous shortcut" not in log, "无重复快捷键告警")
    check("keymap:" in log, "键位表已加载")


def main() -> int:
    harness.configure(sys.argv)
    if not harness.BIN.exists():
        print(f"✗ 找不到 {harness.BIN}（先 ninja -C build）")
        return 2
    ensure_fixture(FIXTURE)
    print(f"播放器：{harness.BIN}")
    print(f"测试片：{FIXTURE}")
    with Player(SOCK, LOG, [str(FIXTURE)], keep=keep) as p:
        phases = [("一致性", t_sanity), ("T3-a 窗口级按键", t3_window_keys),
                  ("T3-b 打开菜单", t3_menu_openers), ("T3-c 弹窗让位", t3_popup_gating)]
        if not no_eye:
            phases.append(("T3-d 单眼态分支", t3_stereo_branch))
        phases.append(("T2 菜单状态机", t2_menu_state_machine))
        for name, fn in phases:
            if not p.alive():
                check(False, f"播放器在「{name}」前已退出（rc={p.proc.returncode}）")
                break
            try:
                fn(p)
            except AssertionError as e:
                check(False, f"{name} 中断：{e}")
                print(f"    key-log: {p.rpc.key_log()[-4:]}")
                print(f"    menu: {p.menu()['raw'][:200]}")
        if harness.failures:
            print("\n最近日志：")
            print("\n".join(p.log_text().splitlines()[-20:]))
    return harness.summary()


if __name__ == "__main__":
    sys.exit(main())
