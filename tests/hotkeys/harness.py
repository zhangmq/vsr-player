"""热键测试公共件：断言记账 + 播放器进程 + 收敛式轮询（不 sleep 猜时间）。

两个测试脚本共用（避免各写一份断言/启动逻辑）：
  - test_hotkeys.py      2D 窗口级键、弹窗让位、菜单状态机、单眼态分支
  - test_stereo_keys.py  真 3D 会话（把副屏当眼镜屏）：键位/帮助/两窗口派发/退出恢复
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rpc import Rpc, wait_until, wait_value  # noqa: F401  (re-export)

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "build" / "src" / "client" / "vsr-player"

failures: list[str] = []
checks = 0
verbose = False


def configure(argv: list[str]) -> None:
    global verbose
    verbose = "-v" in argv


def check(ok, what: str, detail: str = "") -> bool:
    global checks
    checks += 1
    if ok:
        if verbose:
            print(f"  ✓ {what}")
    else:
        failures.append(f"{what}{(': ' + str(detail)) if detail else ''}")
        print(f"  ✗ {what}{(': ' + str(detail)) if detail else ''}")
    return ok


def expect_eq(actual, want, what: str) -> bool:
    return check(actual == want, what, f"实际 {actual!r} != 期望 {want!r}")


def section(title: str) -> None:
    print(f"\n── {title} ──")


def summary() -> int:
    print()
    if failures:
        print(f"✗ {len(failures)}/{checks} 项不通过")
        for f in failures:
            print(f"  - {f}")
        return 1
    print(f"✓ 全部 {checks} 项通过")
    return 0


def ensure_fixture(path: Path, seconds: int = 60) -> None:
    if path.exists() and path.stat().st_size > 100_000:
        return
    print(f"生成测试片 {path} …")
    subprocess.run(
        ["ffmpeg", "-v", "error", "-f", "lavfi",
         "-i", f"testsrc2=size=640x360:rate=30:duration={seconds}",
         "-f", "lavfi", "-i", f"sine=frequency=440:duration={seconds}",
         "-c:v", "libx264", "-pix_fmt", "yuv420p", "-g", "30",
         "-c:a", "aac", "-shortest", "-y", str(path)],
        check=True)


class Player:
    """起一个独立播放器实例（自己的 socket/日志），退出时确保清理。"""

    def __init__(self, sock: Path, log: Path, media: list[str], *,
                 keep: bool = False, verbose_log: bool = True):
        self.sock = sock
        self.logpath = log
        self.media = list(media)
        self.keep = keep
        self.verbose_log = verbose_log
        self.proc: subprocess.Popen | None = None
        self.rpc = Rpc(sock)

    def __enter__(self):
        self.sock.unlink(missing_ok=True)
        self.logpath.unlink(missing_ok=True)
        env = dict(os.environ)
        if self.verbose_log:
            env["VSR_LOG_VERBOSE"] = "1"
        self.logf = self.logpath.open("wb")
        self.proc = subprocess.Popen(
            [str(BIN), "--lang", "en", "--scale", "off", "--fruc", "off",
             "--rpc-socket", str(self.sock), *self.media,
             "--", "--loop-file=inf"],
            stdout=self.logf, stderr=subprocess.STDOUT, env=env)
        wait_until(lambda: self.sock.exists(), "RPC 套接字出现", timeout=20)
        wait_until(lambda: self.rpc.data("get-menu-state"), "QML 就绪", timeout=20)
        wait_until(lambda: "media=1" in self.rpc.data("get-menu-state"),
                   "媒体载入", timeout=30)
        # 把鼠标挪到窗口右下角（远离右键菜单/底栏热区）：菜单/弹窗是"悬停即
        # 改当前行"的，用户鼠标若恰好压在菜单上，"刚打开时没有当前行"这类
        # 断言会随机失败（2026-11-03 实测）。
        self.warp()
        return self

    def warp(self, x: int | None = None, y: int | None = None) -> None:
        """把鼠标挪到窗口内 (x,y)（窗口相对；缺省 = 右下角安全位置）。"""
        if x is None or y is None:
            x = max(10, self.win_w() - 20)
            y = max(10, self.win_h() // 2)
        self.rpc.call("warp-pointer", str(x), str(y))

    def win_size(self) -> tuple[int, int]:
        m = re.search(r"win=(\d+)x(\d+)@", self.rpc.data("get-stereo"))
        return (int(m.group(1)), int(m.group(2))) if m else (1280, 720)

    def win_w(self) -> int:
        return self.win_size()[0]

    def win_h(self) -> int:
        return self.win_size()[1]

    def __exit__(self, *exc):
        if self.proc and self.proc.poll() is None:
            try:
                self.rpc.call("quit", timeout=3)
            except Exception:
                pass
            try:
                self.proc.wait(timeout=6)
            except subprocess.TimeoutExpired:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
        self.logf.close()
        if not self.keep:
            self.sock.unlink(missing_ok=True)

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def log_text(self) -> str:
        return self.logpath.read_text(encoding="utf-8", errors="replace")

    def menu(self) -> dict:
        return self.rpc.menu_state()

    def last_dispatch(self) -> str:
        log = self.rpc.key_log()
        return log[-1] if log else ""

    # ── 菜单操作 ────────────────────────────────────────────────
    def open_menu(self) -> None:
        self.rpc.call("open-menu", "500", "300")
        wait_until(lambda: self.menu()["ctxVisible"] == 1, "菜单打开")
        # 等 popup 完成布局/焦点接管：刚打开的那一瞬间按 Esc/方向键会投给
        # 旧焦点项（人按不到这个速度，但脚本会）。
        settle(self)

    def close_menu(self) -> None:
        if self.menu()["ctxVisible"]:
            self.rpc.key("Escape")
            wait_until(lambda: self.menu()["ctxVisible"] == 0, "菜单关闭")


def settle(p: Player) -> dict:
    """等菜单状态稳定：RPC 命令是异步投递的，处理前的采样会读到陈旧值。
    两次采样间隔 > 一帧，键已被处理；连续两次完全一致才返回。"""
    prev = None
    deadline = time.monotonic() + 4.0
    keys = ("ctxVisible", "cur", "subOpen", "ctl", "msSub", "msAudio", "playlist")
    while time.monotonic() < deadline:
        cur = p.menu()
        if prev is not None and all(cur[k] == prev[k] for k in keys):
            return cur
        prev = cur
        time.sleep(0.12)
    return prev or {}


def nudge(p: Player, seq: str, direction: int, attempts: int = 4,
          timeout: float = 4.0, field: str = "time-pos") -> tuple[float, float]:
    """按一次方向键，返回 (before, after)；没动就重按（最多 attempts 次）。

    连续 seek 有门控：上一次 seek 还没出帧时本次步进**按设计丢弃**，所以每次
    尝试都用**当时**的位置当基准，不能拿几秒前的旧值算预期。
    """
    rpc = p.rpc
    for _ in range(attempts):
        before = rpc.prop(field)
        threshold = before + direction * 3.0
        rpc.key(seq)
        try:
            after = wait_until(
                lambda: (lambda t: t if ((t < threshold) if direction < 0
                                         else (t > threshold)) else None)(rpc.prop(field)),
                f"{seq} 生效", timeout=timeout)
            return before, after
        except AssertionError:
            continue
    raise AssertionError(f"{seq} 按了 {attempts} 次都没有移动 3 个步长")
