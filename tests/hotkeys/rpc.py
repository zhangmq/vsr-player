"""RPC 客户端（JSON IPC over Unix socket）—— 测试脚本共用。

协议见 src/client/RpcServer.h：一条 JSON 一行，命令走 QueuedConnection
（"success" = 已接受，不代表已执行），所以断言一律轮询到收敛，不 sleep。
"""
from __future__ import annotations

import json
import re
import socket
import time
from pathlib import Path


class Rpc:
    def __init__(self, path: str | Path):
        self.path = str(path)

    def call(self, cmd: str, *args, timeout: float = 10.0) -> dict:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        try:
            s.connect(self.path)
            s.sendall((json.dumps({"command": [cmd, *map(str, args)]}) + "\n").encode())
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = s.recv(1 << 16)
                if not chunk:
                    break
                buf += chunk
        finally:
            s.close()
        if not buf:
            raise RuntimeError(f"rpc {cmd}: 空响应（播放器退出了？）")
        return json.loads(buf.decode())

    # ── 便捷封装 ────────────────────────────────────────────────
    def data(self, cmd: str, *args) -> str:
        r = self.call(cmd, *args)
        if r.get("error") not in ("", "success"):
            raise RuntimeError(f"rpc {cmd}: {r['error']}")
        return str(r.get("data", ""))

    def num(self, cmd: str, *args) -> float:
        d = self.data(cmd, *args)
        try:
            return float(d)
        except ValueError:
            raise RuntimeError(f"rpc {cmd}: 非数值 {d!r}")

    def prop(self, name: str) -> float:
        return self.num("get_property", name)

    def key(self, seq: str) -> None:
        self.call("key", seq)

    def key_log(self) -> list[str]:
        return [ln for ln in self.data("key-log").splitlines() if ln.strip()]

    def clear_key_log(self) -> None:
        self.call("clear-key-log")

    def menu_state(self) -> dict:
        """get-menu-state 的字符串快照 → 常用字段字典。"""
        text = self.data("get-menu-state")
        out: dict = {"raw": text}

        def grab(pattern, cast=str, default=None):
            m = re.search(pattern, text)
            return cast(m.group(1)) if m else default

        out["ctxVisible"] = grab(r"ctxVisible=(\d)", int, 0)
        out["anyPopup"] = grab(r"anyPopup=(\d)", int, 0)
        out["popups"] = grab(r"popups=(\d+)", int, 0)
        out["playlist"] = grab(r"playlist=(\d)", int, 0)
        out["subOpen"] = grab(r"subOpen=(\d)", int, 0)
        out["ctl"] = grab(r"ctl=(\d)", int, 0)
        # 注意：C++ 前缀里还有一个 cur=<edition 索引>，导航用的那个后面跟 "("
        out["cur"] = grab(r"cur=(-?\d+)\(", int, -1)
        out["currentRow"] = grab(r"cur=-?\d+\(([^)]*)\)", str, "")
        out["eye"] = grab(r"eye=(\d)", int, 0)
        out["subDelay"] = grab(r"subDelay=(-?\d+)ms", int, 0)
        out["audioDelay"] = grab(r"audioDelay=(-?\d+)ms", int, 0)
        m = re.search(r"ms=(-?\d+)/(-?\d+)", text)
        out["msSub"], out["msAudio"] = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        return out


def wait_value(fn, what: str, timeout: float = 6.0, interval: float = 0.05):
    """轮询到 fn() 返回非 None（0 / "" / False 都算"到"）；返回该值。"""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = fn()
        except (RuntimeError, OSError):
            last = None
        if last is not None:
            return last
        time.sleep(interval)
    raise AssertionError(f"超时 {timeout}s：{what}（最后观测：{last!r}）")


def wait_until(fn, what: str, timeout: float = 6.0, interval: float = 0.05):
    """轮询到 fn() 返回真值；返回该值。超时抛 AssertionError。"""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = fn()
        except (RuntimeError, OSError):
            last = None
        if last:
            return last
        time.sleep(interval)
    raise AssertionError(f"超时 {timeout}s：{what}（最后观测：{last!r}）")
