#!/usr/bin/env python3
"""T1 静态键位检查（不需要显示器、不需要跑播放器）

背景：快捷键这批 bug 清一色是"同一件事写了多份、漏了一处"——
QML Shortcut 漏加一条、菜单里另抄一份偏移键映射、某个 id 没有实现。
编译、启动、截图都看不出来，只有用户按到那个组合才发现。

做法：编译真实的 src/client/Keymap.cpp（Qt 自己的 QKeySequence 解析，
不在这里用正则重写一遍规范化规则），把表 dump 成 JSON，然后断言：

  1. 启动自检通过（重复按键 / 空 id / 无法解析的序列）
  2. 每个 handler=qml 的动作在 main.qml 的 runAction 里真有分支
  3. main.qml 里出现的每个 `id === "..."` 都在表里（拼写漂移）
  4. main.qml 不再有 Shortcut / sequence:（键位不能回到 QML）
  5. ContextMenu.qml 不再有按键映射（偏移键只有一份）
  6. handler=cpp 的动作在 KeyRouter.cpp 里真有实现
  7. 关键键位与用户决定的语义仍在（z / Shift+Z / Ctrl+0 / Ctrl++ / Ctrl+Alt+0 …）
  8. 弹窗内可用的动作集合就是那 6 个延迟动作
  9. 3D 帮助面板引用的每个动作 id 都能解析出键名

用法：./tests/hotkeys/check_keys.py [-v]
退出码：0 = 通过。
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLIENT = ROOT / "src" / "client"

verbose = "-v" in sys.argv

failures: list[str] = []
checks = 0


def check(ok: bool, what: str, detail: str = "") -> bool:
    global checks
    checks += 1
    if ok:
        if verbose:
            print(f"  ✓ {what}")
    else:
        failures.append(f"{what}{(': ' + detail) if detail else ''}")
        print(f"  ✗ {what}{(': ' + detail) if detail else ''}")
    return ok


def pkg_config(*args: str) -> list[str]:
    return subprocess.run(["pkg-config", *args], capture_output=True, text=True,
                          check=True).stdout.split()


def build_and_dump() -> list[dict]:
    out = Path(tempfile.mkdtemp(prefix="vsr-keys-"))
    exe = out / "dump-keys"
    cmd = [
        "g++", "-std=c++20", "-fPIC",
        f"-I{CLIENT}",
        *pkg_config("--cflags", "Qt6Gui", "Qt6Core"),
        str(CLIENT / "Keymap.cpp"),
        str(ROOT / "tests" / "hotkeys" / "dump_keys.cpp"),
        "-o", str(exe),
        *pkg_config("--libs", "Qt6Gui", "Qt6Core"),
    ]
    cc = subprocess.run(cmd, capture_output=True, text=True)
    if cc.returncode != 0:
        failures.append("编译 dump_keys.cpp 失败:\n" + cc.stderr[-2000:])
        print(failures[-1])
        sys.exit(1)
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    selfcheck_out = run.stderr.strip()
    ok = check(run.returncode == 0, "键位表启动自检通过", selfcheck_out)
    if ok and selfcheck_out:
        print(selfcheck_out)
    try:
        return json.loads(run.stdout)
    except json.JSONDecodeError:
        failures.append("dump 输出不是 JSON:\n" + run.stdout[:500])
        print(failures[-1])
        sys.exit(1)


def main() -> int:
    rows = build_and_dump()
    main_qml = (CLIENT / "main.qml").read_text(encoding="utf-8")
    menu_qml = (CLIENT / "ui" / "ContextMenu.qml").read_text(encoding="utf-8")
    router_cpp = (CLIENT / "KeyRouter.cpp").read_text(encoding="utf-8")
    help_qml = (CLIENT / "StereoHotkeyHelp.qml").read_text(encoding="utf-8")

    ids = {r["id"] for r in rows}
    print(f"键位表：{len(rows)} 行 / {len(ids)} 个动作")

    check(len(rows) >= 40, "表非空且规模合理", f"{len(rows)} 行")

    # ── 1. QML 侧动作必须真的实现 ────────────────────────────────
    qml_ids = {r["id"] for r in rows if r["handler"] == "qml"}
    for aid in sorted(qml_ids):
        check(f'id === "{aid}"' in main_qml,
              f'QML 动作 {aid} 在 main.qml 有分支')

    # 反向：main.qml 里比较过的 id 必须在表里
    referenced = set(re.findall(r'id === "([^"]+)"', main_qml))
    for aid in sorted(referenced):
        check(aid in ids, f'main.qml 引用的 "{aid}" 在键位表里')

    # ── 2. 键位定义不能回到 QML ──────────────────────────────────
    check("Shortcut {" not in main_qml, "main.qml 没有 QML Shortcut")
    check("sequence:" not in main_qml, "main.qml 没有裸 sequence:")

    # ── 3. 菜单里不能再有第二份按键映射 ─────────────────────────
    check("Qt.Key_" not in menu_qml, "ContextMenu.qml 不再按 Qt.Key_* 判键")
    for legacy in ('"ctrl+0"', '"shift+z"', '"audio-up"', '"audio-down"',
                   '"ctrl+alt+0"'):
        check(legacy not in menu_qml, f"ContextMenu.qml 无遗留映射 {legacy}")

    # ── 4. C++ 动作必须真的实现 ──────────────────────────────────
    cpp_ids = {r["id"] for r in rows
               if r["handler"] == "cpp" and r["owner"] in ("shortcut", "filter")}
    for aid in sorted(cpp_ids):
        check(f'id == "{aid}"' in router_cpp,
              f'C++ 动作 {aid} 在 KeyRouter 有实现')

    # ── 5. 用户定下的键位/语义（回归护栏）────────────────────────
    pairs = {(r["id"], s) for r in rows for s in r["seqs"]}
    required = [
        ("sub-delay-down", "z"), ("sub-delay-up", "Shift+Z"),
        ("sub-delay-reset", "Ctrl+0"),
        ("audio-delay-up", "Ctrl++"), ("audio-delay-up", "Ctrl+="),
        ("audio-delay-down", "Ctrl+-"), ("audio-delay-reset", "Ctrl+Alt+0"),
        ("open-menu", "Menu"), ("open-menu", "Shift+F10"), ("open-menu", "F10"),
        ("seek-back", "Left"), ("seek-fwd", "Right"),
        ("seek-back-10", "Shift+Left"), ("seek-fwd-10", "Shift+Right"),
        ("volume-up", "Up"), ("volume-down", "Down"),
        ("toggle-osd", "Tab"), ("play-pause", "Space"),
        ("remove-from-playlist", "Delete"), ("delete-file", "Shift+Delete"),
        ("quit", "Ctrl+Q"),
    ]
    for pair in required:
        check(pair in pairs, f"键位 {pair[1]} → {pair[0]} 仍在")

    # 字幕/音频归零必须分开（用户 2026-11-02 明确要求）
    sub_reset = [r for r in rows if r["id"] == "sub-delay-reset"][0]
    aud_reset = [r for r in rows if r["id"] == "audio-delay-reset"][0]
    check(sub_reset["seqs"] != aud_reset["seqs"],
          "字幕/音频归零不是同一个键")

    # ── 6. 弹窗内可用的动作 = 那 6 个延迟动作 ───────────────────
    popup_ok = {r["id"] for r in rows if r["popup_ok"]}
    expect = {"sub-delay-down", "sub-delay-up", "sub-delay-reset",
              "audio-delay-up", "audio-delay-down", "audio-delay-reset"}
    check(popup_ok == expect, "弹窗内可用的动作集合", f"{sorted(popup_ok)}")

    # 弹窗内可用的动作必须是 C++ 实现（菜单只派发 C++ 动作）
    for r in rows:
        if r["popup_ok"]:
            check(r["handler"] == "cpp", f'{r["id"]} 在 C++ 侧实现')

    # ── 7. 3D 帮助面板的键名都能解析 ────────────────────────────
    help_ids = set(re.findall(r'keys\.helpKey\("([^"]+)"\)', help_qml))
    check(len(help_ids) >= 8, "帮助面板使用键位表", f"{len(help_ids)} 个动作")
    by_id = {r["id"]: r for r in rows}
    for aid in sorted(help_ids):
        ok = aid in by_id and bool(by_id[aid]["help_key"])
        check(ok, f'帮助面板的动作 "{aid}" 有键名')

    # 帮助键名必须是**单段**（`QKeySequence(key, mods)` 那个"两段和弦"构造
    # 函数会把 "Shift+Delete" 打印成 "Del, Shift+"，2026-11-03 踩过）
    for r in rows:
        hk = r["help_key"]
        check(", " not in hk, f'帮助键名是单段按键（{r["id"]}）', hk)

    # ── 8. 菜单导航齐备 ─────────────────────────────────────────
    menu_ids = {r["id"] for r in rows if r["owner"] == "menu"}
    for nav in ("up", "down", "left", "right", "home", "end", "enter", "escape"):
        check(f"menu-{nav}" in menu_ids, f"菜单导航 menu-{nav} 存在")

    print()
    if failures:
        print(f"✗ {len(failures)}/{checks} 项不通过")
        return 1
    print(f"✓ 全部 {checks} 项通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
