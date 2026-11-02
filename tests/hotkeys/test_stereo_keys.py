#!/usr/bin/env python3
"""3D 会话热键测试（需要 ≥2 块屏幕；把副屏当眼镜屏）

为什么单独一个脚本：3D 会把主窗口 hide → 全屏搬到目标屏，并创建/显示桌面
UI 窗口——这是个"会动你屏幕"的测试，不该混进日常跑的那一份。

覆盖（都是真 3D 会话里、真按键投递）：
  S1 进入 3D：presentation 落到目标屏、满屏；合成映射变为 active
  S2 3D 键位：H/J/K/L/Tab/A/Space/←→/↑↓/Ctrl+←→ 全部按 scope=3d 派发，
     且逐项验证效果（帮助开关、布局/眼序/全半、播放暂停、seek、音量、切集）
  S2b 眼睛映射（视觉证据）：用"左红右蓝"的 SBS 片源抓取 presentation 窗口，
     确认左眼拿到左半、右眼拿到右半，K 换眼后两半互换（纯状态断言测不到这个）
  S3 两窗口派发：P 只开桌面 UI 窗口的播放列表（presentation 不受影响）；
     菜单开在桌面窗口；Esc 在 3D 下被吞掉、但弹窗打开时让位（关弹窗）
  S4 退出：窗口回到原屏原尺寸，键位回到 scope=2d

用法：
  ./tests/hotkeys/test_stereo_keys.py [-v] [--keep] [--screen <index|name>]
      --screen 不指定时自动挑"主窗口不在的那块屏"（= 副屏）
退出码：0 = 通过；2 = 跳过（只有一块屏 / 没有构建产物）。
"""
from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness
from harness import (Player, check, ensure_fixture, expect_eq, section, wait_until)

SOCK = Path("/tmp/vsr-stereo-keys.sock")
SBS_FIXTURE = Path("/tmp/vsr-sbs-lr.mp4")   # 左半纯红 / 右半纯蓝（SBS）
LOG = Path("/tmp/vsr-stereo-keys.log")
FIXTURE = Path("/tmp/vsr-hotkey-fixture.mp4")

keep = "--keep" in sys.argv


def arg_value(name: str) -> str | None:
    if name in sys.argv:
        i = sys.argv.index(name)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return None


SCREEN_ARG = arg_value("--screen")


def ensure_sbs_fixture() -> None:
    """左红右蓝的 SBS 片源（每半 320x360）：抓窗口后能逐半判定"眼睛拿到哪半"。"""
    if SBS_FIXTURE.exists() and SBS_FIXTURE.stat().st_size > 5000:
        return
    print(f"生成 SBS 测试片 {SBS_FIXTURE} …")
    subprocess.run(
        ["ffmpeg", "-v", "error",
         "-f", "lavfi", "-i", "color=c=red:s=320x360:d=30",
         "-f", "lavfi", "-i", "color=c=blue:s=320x360:d=30",
         "-filter_complex", "hstack=inputs=2",
         "-c:v", "libx264", "-pix_fmt", "yuv420p", "-y", str(SBS_FIXTURE)],
        check=True)


def mean_color(png: Path, box: tuple[int, int, int, int]) -> tuple[int, int, int]:
    """窗口抓图的某个矩形区域的平均色（ffmpeg crop+scale=1:1/area，不依赖 PIL）。"""
    x, y, w, h = box
    out = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(png), "-vf",
         f"crop={w}:{h}:{x}:{y},scale=1:1:flags=area",
         "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, check=True).stdout
    return (out[0], out[1], out[2])


def grab_window(p: Player, png: Path, min_size: int = 200_000) -> None:
    """抓 presentation 窗口并**等文件写完**：grab-window 是投递到 GUI 线程的，
    文件会边写边存在，读得太早会把写了一半的 PNG 交给 ffmpeg（exit 69）。"""
    png.unlink(missing_ok=True)
    p.rpc.call("grab-window", str(png))
    wait_until(lambda: png.exists() and png.stat().st_size > min_size,
               f"抓到 {png.name}", timeout=15)
    prev = -1
    for _ in range(80):
        sz = png.stat().st_size
        if sz == prev:
            return
        prev = sz
        time.sleep(0.05)
    raise AssertionError(f"{png.name} 一直在写（大小不稳定）")


def image_size(png: Path) -> tuple[int, int]:
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height", "-of", "csv=p=0", str(png)],
        capture_output=True, text=True, check=True).stdout.strip()
    w, h = out.split(",")
    return int(w), int(h)


def is_red(c: tuple[int, int, int]) -> bool:
    return c[0] > 150 and c[1] < 90 and c[2] < 90


def is_blue(c: tuple[int, int, int]) -> bool:
    return c[2] > 150 and c[0] < 90 and c[1] < 90


def ensure_toggle(p: Player, field: str, want: int) -> None:
    """stereo-cycle 只有 toggle；按当前状态循环到想要的值。"""
    rpc = p.rpc
    for _ in range(4):
        if rpc.stereo()[field] == want:
            return
        rpc.call("stereo-cycle", field)
        time.sleep(0.35)
    check(False, f"{field} 收敛到 {want}", rpc.stereo()[field])


def pick_target(p: Player, screens: list[dict]) -> dict | None:
    """挑目标屏：--screen 指定 index/name，否则选"主窗口不在的那块"。"""
    if SCREEN_ARG is not None:
        for s in screens:
            if str(s["index"]) == SCREEN_ARG or s["name"] == SCREEN_ARG:
                return s
        return None
    cur = p.rpc.stereo()["win"].split("@")[-1]
    for s in screens:
        if s["name"] != cur:
            return s
    return None


def press_expect(p: Player, seq: str, action_id: str, scope: str = "3d",
                 mode: str = "3d", timeout: float = 5.0) -> str:
    """按一次键，断言派发到指定动作且走指定键位上下文，返回日志行。"""
    p.rpc.clear_key_log()
    p.rpc.key(seq)
    wait_until(lambda: action_id in (p.last_dispatch() or ""),
               f"{seq} → {action_id}", timeout=timeout)
    line = p.last_dispatch()
    check(f"scope={scope}" in line, f"{seq} 走 {scope} 键位表", line)
    check(f"mode={mode}" in line, f"{seq} 处于 {mode} 态", line)
    return line


def s1_enter_3d(p: Player, screens: list[dict], target: dict) -> None:
    section("S1 进入 3D（副屏当眼镜屏）")
    st = p.rpc.stereo()
    expect_eq(st["active"], 0, "进入前是 2D")
    orig_win = st["win"]
    p.orig_win = orig_win

    p.rpc.call("stereo-start", "1", str(target["index"]))
    st = wait_until(lambda: (lambda s: s if s["active"] == 1 else None)(p.rpc.stereo()),
                    "3D 会话启动", timeout=25)
    check(True, "3D 会话已启动")
    expect_eq(st["mode"], 1, "模式 = 3D 片源")
    expect_eq(st["screen"], target["name"], "目标屏 = 副屏")

    # presentation 窗口应满屏落在目标屏（hide→setScreen→showFullScreen 有延迟，
    # 轮询到尺寸稳定为止，别在 active 刚置位的瞬间读）
    want = (target["logicalWidth"], target["logicalHeight"])
    got = wait_until(lambda: (lambda sz: sz if sz == want else None)(p.win_size()),
                     "presentation 满屏", timeout=10)
    check(got == want, "presentation 满屏", f"{got} vs {want}")
    st = p.rpc.stereo()
    check(st["win"].endswith("@" + target["name"]), "presentation 在目标屏上", st["win"])

    # 合成映射要变成 active（每帧写快照，可能比窗口晚一两帧）
    m = wait_until(lambda: (lambda s: s if s["map_active"] == 1 else None)(p.rpc.stereo()),
                   "合成映射生效", timeout=10)
    check(m["map_out"] != "", "映射里有输出尺寸", m["map_out"])
    print(f"    映射：{m['map']}")


def s2_3d_keys(p: Player) -> None:
    section("S2 3D 键位（真实投递）")
    rpc = p.rpc

    # H：3D 快捷键帮助面板
    h0 = rpc.stereo()["help"]
    press_expect(p, "H", "toggle-help")
    wait_until(lambda: rpc.stereo()["help"] != h0, "帮助面板翻转")
    h1 = rpc.stereo()["help"]
    press_expect(p, "H", "toggle-help")
    wait_until(lambda: rpc.stereo()["help"] == h0, "帮助面板恢复")
    check(True, f"H 切帮助面板（{h0}→{h1}→{h0}）")

    # J：SBS / TAB
    l0 = rpc.stereo()["layout"]
    press_expect(p, "J", "stereo-layout")
    l1 = wait_until(lambda: (lambda v: v if v != l0 else None)(rpc.stereo()["layout"]),
                    "布局翻转")
    press_expect(p, "J", "stereo-layout")
    wait_until(lambda: rpc.stereo()["layout"] == l0, "布局恢复")
    check(True, f"J 切布局（{l0}→{l1}→{l0}）")

    # K：左右眼
    s0 = rpc.stereo()["swap"]
    press_expect(p, "K", "stereo-swap")
    wait_until(lambda: rpc.stereo()["swap"] != s0, "眼序翻转")
    press_expect(p, "K", "stereo-swap")
    wait_until(lambda: rpc.stereo()["swap"] == s0, "眼序恢复")
    check(True, "K 切左右眼")

    # L：全/半
    f0 = rpc.stereo()["half"]
    press_expect(p, "L", "stereo-half")
    wait_until(lambda: rpc.stereo()["half"] != f0, "全半翻转")
    press_expect(p, "L", "stereo-half")
    wait_until(lambda: rpc.stereo()["half"] == f0, "全半恢复")
    check(True, "L 切全/半")

    # Tab：管线状态 OSD
    press_expect(p, "Tab", "toggle-osd")

    # A：画面比例（scope=both，2D/3D 都生效）
    press_expect(p, "A", "aspect-next", scope="both")

    # Space：播放/暂停
    was = rpc.call("get_property", "pause").get("data")
    press_expect(p, "Space", "play-pause")
    wait_until(lambda: rpc.call("get_property", "pause")["data"] != was, "Space 切播放态")
    press_expect(p, "Space", "play-pause")
    wait_until(lambda: rpc.call("get_property", "pause")["data"] == was, "Space 切回")
    check(True, "Space 播放/暂停")

    # ←/→：±5 s（带重按，连续 seek 有门控）
    rpc.call("set_property", "pause", "no")
    wait_until(lambda: rpc.call("get_property", "pause")["data"] == "no", "恢复播放")
    rpc.call("seek", "2", "absolute")
    wait_until(lambda: 1.5 <= rpc.prop("time-pos") <= 3.5, "定位到 2s")
    press_expect(p, "Right", "seek-fwd")
    before, after = harness.nudge(p, "Right", +1)
    check(after > before + 3.0, "→ 前进", f"{before:.1f}→{after:.1f}")
    press_expect(p, "Left", "seek-back")
    b2, back = harness.nudge(p, "Left", -1)
    check(back < b2 - 3.0, "← 后退", f"{b2:.1f}→{back:.1f}")

    # ↑/↓：音量 ±5%
    rpc.call("set_property", "volume", "100")
    wait_until(lambda: abs(rpc.prop("volume") - 100) < 0.6, "音量置 100")
    press_expect(p, "Down", "volume-down")
    wait_until(lambda: rpc.prop("volume") < 96, "↓ 降音量")
    press_expect(p, "Up", "volume-up")
    wait_until(lambda: rpc.prop("volume") > 99, "↑ 恢复音量")
    check(True, "↑/↓ 调音量")

    # Ctrl+→ / Ctrl+←：切集（两条目）
    pos0 = int(float(rpc.data("get_property", "playlist-pos")))
    press_expect(p, "Ctrl+Right", "playlist-next")
    pos1 = wait_until(lambda: (lambda v: v if v != pos0 else None)(
        int(float(rpc.data("get_property", "playlist-pos")))), "切到下一个")
    press_expect(p, "Ctrl+Left", "playlist-prev")
    wait_until(lambda: int(float(rpc.data("get_property", "playlist-pos"))) == pos0,
               "切回上一个")
    check(True, f"Ctrl+→/← 切集（{pos0}→{pos1}→{pos0}）")


def s2b_eye_mapping(p: Player) -> None:
    section("S2b 眼睛映射（视觉证据）")
    rpc = p.rpc
    rpc.call("command", "loadfile", str(SBS_FIXTURE), "replace")
    wait_until(lambda: "vsr-sbs-lr.mp4" in rpc.data("get_property", "path"),
               "SBS 片源载入", timeout=15)
    time.sleep(1.0)

    rpc.call("stereo-help", "0")      # 帮助面板会盖住画面中央
    ensure_toggle(p, "layout", 0)     # SBS
    ensure_toggle(p, "half", 0)       # 全幅
    ensure_toggle(p, "swap", 0)       # 正常眼序
    time.sleep(0.6)

    png = Path("/tmp/vsr-stereo-sbs.png")
    grab_window(p, png)
    w, h = image_size(png)
    eye = w // 2
    box_l = (eye // 2 - 200, h // 2 - 200, 400, 400)
    box_r = (eye + eye // 2 - 200, h // 2 - 200, 400, 400)
    cl, cr = mean_color(png, box_l), mean_color(png, box_r)
    check(is_red(cl), "左眼 = 左半（红）", cl)
    check(is_blue(cr), "右眼 = 右半（蓝）", cr)
    print(f"    抓图 {w}x{h}：左眼 {cl} / 右眼 {cr}")

    # K 换眼：两半互换
    press_expect(p, "K", "stereo-swap")
    wait_until(lambda: rpc.stereo()["swap"] == 1, "换眼生效")
    time.sleep(0.5)
    png2 = Path("/tmp/vsr-stereo-sbs-swap.png")
    grab_window(p, png2)
    cl2, cr2 = mean_color(png2, box_l), mean_color(png2, box_r)
    check(is_blue(cl2), "换眼后左眼 = 右半（蓝）", cl2)
    check(is_red(cr2), "换眼后右眼 = 左半（红）", cr2)
    press_expect(p, "K", "stereo-swap")   # 恢复
    wait_until(lambda: rpc.stereo()["swap"] == 0, "眼序恢复")

    # 换回主测试片，后续相位继续用
    rpc.call("command", "loadfile", str(FIXTURE), "replace")
    wait_until(lambda: "vsr-hotkey-fixture" in rpc.data("get_property", "path"),
               "主测试片载入", timeout=15)
    time.sleep(0.8)


def s3_two_windows(p: Player) -> None:
    section("S3 两窗口派发（桌面 UI vs presentation）")
    rpc = p.rpc
    expect_eq(rpc.control_state()["control"], 1, "桌面 UI 窗口可见")

    # P 只在桌面 UI 窗口打开播放列表
    rpc.key("P")
    wait_until(lambda: rpc.control_state()["playlist"] == 1, "P 打开桌面播放列表")
    expect_eq(p.menu()["playlist"], 0, "presentation 不开播放列表")
    rpc.key("P")
    wait_until(lambda: rpc.control_state()["playlist"] == 0, "P 关掉桌面播放列表")
    check(True, "P 只作用于桌面 UI 窗口")

    # 菜单开在桌面 UI 窗口
    rpc.key("Menu")
    wait_until(lambda: rpc.control_state()["ctxVisible"] == 1, "Menu 打开桌面菜单")
    expect_eq(p.menu()["ctxVisible"], 0, "presentation 不开菜单")
    cur0 = rpc.control_state()["cur"]
    rpc.key("Down")
    wait_until(lambda: rpc.control_state()["cur"] != cur0, "菜单内 ↓ 生效")
    check(True, "3D 下菜单导航在桌面窗口")

    # 弹窗打开时 Esc 让位（关弹窗，不退出 3D）
    rpc.key("Escape")
    wait_until(lambda: rpc.control_state()["ctxVisible"] == 0, "Esc 关菜单")
    expect_eq(rpc.stereo()["active"], 1, "关菜单不退出 3D")
    check(True, "3D 下弹窗优先吃 Esc")

    # 没有弹窗时 Esc 被吞掉（不退全屏、不退出 3D、窗口尺寸不变）
    win_before = p.win_size()
    line = press_expect(p, "Escape", "escape-swallow")
    time.sleep(0.4)
    expect_eq(rpc.stereo()["active"], 1, "Esc 不退出 3D")
    expect_eq(p.win_size(), win_before, "Esc 不改窗口尺寸")
    check(True, "3D 下 Esc 被吞掉")


def s4_exit(p: Player) -> None:
    section("S4 退出 3D")
    rpc = p.rpc
    rpc.call("stereo-stop")
    st = wait_until(lambda: (lambda s: s if s["active"] == 0 else None)(rpc.stereo()),
                    "3D 退出", timeout=25)
    expect_eq(st["active"], 0, "已回到 2D")
    check(p.win_size() == (1280, 720), "窗口回到默认尺寸", p.win_size())
    check(st["win"].endswith(p.orig_win.split("@")[-1]), "窗口回到原屏", st["win"])

    # 键位回到 2D 上下文
    press_expect(p, "Right", "seek-fwd", scope="2d", mode="2d")


def main() -> int:
    harness.configure(sys.argv)
    if not harness.BIN.exists():
        print(f"✗ 找不到 {harness.BIN}（先 ninja -C build）")
        return 2
    ensure_fixture(FIXTURE)
    ensure_sbs_fixture()
    print(f"播放器：{harness.BIN}")
    print(f"测试片：{FIXTURE}")
    # 两个条目（同一文件两遍）：为了测 Ctrl+←/→ 切集
    with Player(SOCK, LOG, [str(FIXTURE), str(FIXTURE)], keep=keep) as p:
        screens = p.rpc.screens()
        print("屏幕：")
        for s in screens:
            print(f"  [{s['index']}] {s['name']} {s['model']} "
                  f"物理 {s['width']}x{s['height']} 逻辑 {s['logicalWidth']}x{s['logicalHeight']}")
        if len(screens) < 2:
            print("跳过：只有一块屏幕，3D 会话无处安放")
            return 2
        target = pick_target(p, screens)
        if target is None:
            print(f"跳过：--screen {SCREEN_ARG} 没匹配到（或找不到副屏）")
            return 2
        print(f"目标屏（眼镜屏）：[{target['index']}] {target['name']}")

        phases = [("S1 进入 3D", lambda: s1_enter_3d(p, screens, target)),
                  ("S2 3D 键位", lambda: s2_3d_keys(p)),
                  ("S2b 眼睛映射", lambda: s2b_eye_mapping(p)),
                  ("S3 两窗口", lambda: s3_two_windows(p)),
                  ("S4 退出", lambda: s4_exit(p))]
        for name, fn in phases:
            if not p.alive():
                check(False, f"播放器在「{name}」前已退出（rc={p.proc.returncode}）")
                break
            try:
                fn()
            except AssertionError as e:
                check(False, f"{name} 中断：{e}")
                print(f"    key-log: {p.rpc.key_log()[-4:]}")
                print(f"    stereo: {p.rpc.stereo()['raw'][:220]}")
        # 收尾：不管失败在哪，都尽量退出 3D，别把窗口留在副屏全屏
        try:
            if p.rpc.stereo()["active"]:
                p.rpc.call("stereo-stop")
                time.sleep(1.0)
        except Exception:
            pass

        log = p.log_text()
        check("QML error" not in log, "无 QML 错误")
        check("keymap:" in log and "self-check OK" in log, "键位表自检通过")
        if harness.failures:
            print("\n最近日志：")
            print("\n".join(log.splitlines()[-20:]))
    return harness.summary()


if __name__ == "__main__":
    sys.exit(main())
