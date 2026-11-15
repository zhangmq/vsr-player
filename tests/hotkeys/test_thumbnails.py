#!/usr/bin/env python3
"""进度条缩略图（seekbar thumbnail）运行期测试。

覆盖 PreviewThumbnailer（进程内第二个 libmpv + 软件渲染）与 QML 预览面板：
  T1 就绪状态 / 基本取帧（尺寸、非黑、时延）
  T2 时间桶缓存（同一 2s 桶不重解，serial 不变、latency=0）
  T3 宽度配置（set-preview-width → 输出尺寸跟着变）
  T4 章节（chapter-list → get-chapters / 面板文案数据源）
  T5 QML 悬浮面板（debug-hover 注入；抓窗口对比"有/无面板"）
  T6 3D 单眼裁剪（set-stereo-map sbs full/half → 缩略图高度翻倍/还原）
  T7 纯音频 / 无视频轨 → 不启用（preview 明确报不可用，不空等）
  T8 3D 会话里面板落在桌面 control 窗口（有副屏时）
  T9 右键菜单「界面 → 缩略图」开关（关闭后只显示时间气泡）
  T10 网络流（HTTP 点播，支持 Range）也能出缩略图
  T11 右键菜单「界面 → 隐藏无效菜单项」开关（灰显占位 vs 隐藏）

Wayland 下无法程序化移动指针（QCursor::setPos 无效），悬浮用 RPC
`debug-hover <秒>` 注入（见 PlayerViewModel::setPreviewDebugHoverMs）。

用法：./tests/hotkeys/test_thumbnails.py [-v] [--keep]
退出码：0 = 通过。
"""
from __future__ import annotations

import functools
import http.server
import os
import re
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harness import (BIN, Player, Rpc, check, expect_eq, section, settle,
                     summary, wait_until)

SOCK = Path("/tmp/vsr-thumbnails.sock")
LOG = Path("/tmp/vsr-thumbnails.log")
FIXTURE = Path("/tmp/vsr-thumb-fixture.mkv")
AUDIO = Path("/tmp/vsr-thumb-audio.mp3")
OUT = Path("/tmp/vsr-thumb-out")

keep = "--keep" in sys.argv

# 面板逻辑宽（= PlayerViewModel::kThumbLogicalWidth，改常量时同步这一处）
WANT_LOGICAL_W = 192


# ── 素材 ────────────────────────────────────────────────────────────

def ensure_fixtures() -> None:
    """带章节的 30 s 测试片 + 纯音频。章节用于 T4（面板文案数据源）。"""
    if not (FIXTURE.exists() and FIXTURE.stat().st_size > 100_000):
        meta = Path("/tmp/vsr-thumb-chapters.txt")
        meta.write_text(
            ";FFMETADATA1\n"
            "[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=15000\ntitle=Part A\n"
            "[CHAPTER]\nTIMEBASE=1/1000\nSTART=15000\nEND=30000\ntitle=Part B\n",
            encoding="utf-8")
        subprocess.run(
            ["ffmpeg", "-v", "error", "-f", "lavfi",
             "-i", "testsrc2=size=640x360:rate=30:duration=30",
             "-f", "lavfi", "-i", "sine=frequency=440:duration=30",
             "-i", str(meta), "-map", "0:v", "-map", "1:a", "-map_metadata", "2",
             "-c:v", "libx264", "-pix_fmt", "yuv420p", "-g", "30",
             "-c:a", "aac", "-shortest", "-y", str(FIXTURE)],
            check=True)
    if not (AUDIO.exists() and AUDIO.stat().st_size > 10_000):
        subprocess.run(
            ["ffmpeg", "-v", "error", "-f", "lavfi",
             "-i", "sine=frequency=440:duration=5",
             "-c:a", "libmp3lame", "-y", str(AUDIO)],
            check=True)


def image_stats(path: Path) -> dict:
    """用 ffmpeg 解成灰度算 w/h/max/mean（不引入 PIL 依赖）。"""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "stream=width,height",
         "-of", "csv=p=0", str(path)],
        capture_output=True, text=True, check=True).stdout.strip()
    w, h = (int(x) for x in out.split(","))
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(path), "-f", "rawvideo",
         "-pix_fmt", "gray", "-"], capture_output=True, check=True).stdout
    return {"w": w, "h": h, "max": max(raw) if raw else 0}


def preview(rpc, tag: str, sec: float) -> dict:
    OUT.mkdir(exist_ok=True)
    path = OUT / f"{tag}.png"
    path.unlink(missing_ok=True)
    t0 = time.time()
    res = rpc.call("preview", str(path), str(sec), timeout=20)
    ms = (time.time() - t0) * 1000
    data = res.get("data") or {}
    return {"ok": not res.get("error"), "err": res.get("error", ""),
            "path": path, "ms": ms, "data": data if isinstance(data, dict) else {}}


# ── 用例 ────────────────────────────────────────────────────────────

def t1_basic(p: Player) -> None:
    section("T1 基本取帧")
    rpc = p.rpc
    st = wait_until(lambda: (lambda d: d if d.get("loaded") else None)(
        rpc.call("get-preview-state").get("data")), "预览实例载入文件", timeout=20)
    check(st.get("ready") is True, "preview ready", st)
    # 尺寸口径（用户 2026-11-15）：**面板按逻辑像素固定 192**，预览实例的渲染宽
    # = 192 × DPR（dpr=2 的屏 → 384 px 纹理，否则放大两倍显示会糊）；不再跟窗口尺寸走。
    expect_eq(st.get("logicalWidth"), WANT_LOGICAL_W, "面板逻辑宽固定")
    dpr = st.get("dpr")
    check(isinstance(dpr, (int, float)) and dpr > 0, "上报了 DPR", dpr)
    want_px = max(1, round(WANT_LOGICAL_W * (dpr or 1)))
    expect_eq(st.get("width"), want_px, "渲染宽 = 逻辑宽 × DPR")
    # 窗口尺寸变化不应改变渲染宽（这条正是取消比例的验收点）
    win_w, win_h = p.win_w(), p.win_h()
    p.rpc.call("resize", str(max(640, win_w - 400)), str(max(400, win_h - 200)))
    time.sleep(0.8)
    st2 = p.rpc.call("get-preview-state").get("data") or {}
    expect_eq(st2.get("width"), want_px, "改窗口尺寸后渲染宽不变")
    expect_eq(st2.get("logicalWidth"), WANT_LOGICAL_W, "改窗口尺寸后逻辑宽不变")
    p.rpc.call("resize", str(win_w), str(win_h))     # 还原，别影响后续抓图
    wait_until(lambda: p.win_w() == win_w, "窗口尺寸还原", timeout=10)
    rpc.call("set-preview-width", "320")
    wait_until(lambda: rpc.call("get-preview-width").get("data") == 320,
               "切到 320 覆盖宽度", timeout=10)

    r = preview(rpc, "t1a", 5)
    check(r["ok"], "preview 5s 成功", r["err"])
    s = image_stats(r["path"])
    expect_eq((s["w"], s["h"]), (320, 180), "缩略图 320x180")
    check(s["max"] > 20, "画面非全黑", s["max"])
    check(r["ms"] < 2000, "首次取帧 < 2s", f"{r['ms']:.0f}ms")
    lat = r["data"].get("latencyMs", -1)
    check(0 <= lat < 1500, "引擎自报时延合理", lat)


def t2_bucket_cache(p: Player) -> None:
    section("T2 时间桶缓存")
    rpc = p.rpc
    a = preview(rpc, "t2a", 5.0)
    check(a["ok"], "5.0s 成功")
    b = preview(rpc, "t2b", 5.6)          # 同一 2s 桶
    check(b["ok"], "5.6s（同桶）成功", b["err"])
    expect_eq(b["data"].get("serial"), a["data"].get("serial"), "同桶不重解（serial 不变）")
    expect_eq(b["data"].get("latencyMs"), 0, "同桶命中缓存（latency=0）")
    c = preview(rpc, "t2c", 7.0)          # 下一桶
    check(c["ok"], "7.0s（新桶）成功")
    check(c["data"].get("serial") != a["data"].get("serial"), "新桶产生新图")


def t3_width(p: Player) -> None:
    section("T3 宽度配置")
    rpc = p.rpc
    rpc.call("set-preview-width", "480")
    wait_until(lambda: rpc.call("get-preview-width").get("data") == 480,
               "宽度设置生效", timeout=10)
    r = preview(rpc, "t3", 9)
    check(r["ok"], "480 宽取帧成功", r["err"])
    s = image_stats(r["path"])
    expect_eq((s["w"], s["h"]), (480, 270), "480x270")
    rpc.call("set-preview-width", "320")
    wait_until(lambda: rpc.call("get-preview-width").get("data") == 320, "宽度还原")


def t4_chapters(p: Player) -> None:
    section("T4 章节")
    rpc = p.rpc
    ch = wait_until(lambda: (lambda d: d if isinstance(d, list) and len(d) >= 2 else None)(
        rpc.call("get-chapters").get("data")), "章节载入", timeout=20)
    expect_eq(len(ch), 2, "章节数 2")
    expect_eq(ch[0].get("title"), "Part A", "第一章标题")
    expect_eq(ch[1].get("title"), "Part B", "第二章标题")
    check(abs(float(ch[1].get("time", -1)) - 15.0) < 0.5, "第二章时间 ≈15s", ch[1])


def t5_ui(p: Player) -> None:
    section("T5 QML 悬浮面板")
    rpc = p.rpc
    OUT.mkdir(exist_ok=True)
    plain = OUT / "ui_plain.png"
    hover = OUT / "ui_hover.png"
    plain.unlink(missing_ok=True)
    hover.unlink(missing_ok=True)

    # 无悬停基准
    rpc.call("debug-hover", "off")
    time.sleep(0.4)
    rpc.call("grab-window", str(plain), "player")
    wait_until(lambda: plain.exists() and plain.stat().st_size > 0, "基准截图", timeout=10)
    time.sleep(0.6)

    rpc.call("debug-hover", "10")
    wait_until(lambda: (lambda d: d if d.get("serial", 0) > 0 else None)(
        rpc.call("get-preview-state").get("data")), "面板请求触发", timeout=15)
    time.sleep(0.8)
    rpc.call("grab-window", str(hover), "player")
    wait_until(lambda: hover.exists() and hover.stat().st_size > 0, "悬停截图", timeout=10)
    time.sleep(0.8)

    check(plain.read_bytes() != hover.read_bytes(), "悬停后画面出现面板（截图不同）")
    # 面板形态：缩略图铺满（无外框/无内边距）+ 底部黑底文字条 → 与基准截图像素不同；
    # 这里只做"出现了东西"的粗断言，版式靠人眼看 grab-window 截图为证。
    rpc.call("debug-hover", "off")


def t6_3d_crop(p: Player) -> None:
    section("T6 3D 单眼裁剪")
    rpc = p.rpc
    # 640x360 源当 SBS 全幅：单眼 320x360 → 缩略图 320x360（高按眼宽比）
    # get-stereo 的映射段由 Video 每帧写快照——暂停时不会更新，所以这里
    # 直接以"预览输出尺寸"为准（也正是被测行为）。
    def crop_size(tag: str, want: tuple[int, int], what: str) -> None:
        deadline = time.time() + 12
        got = None
        while time.time() < deadline:
            r = preview(rpc, tag, 11)
            if r["ok"]:
                s = image_stats(r["path"])
                got = (s["w"], s["h"])
                if got == want:
                    break
            time.sleep(0.15)
        expect_eq(got, want, what)

    rpc.call("set-stereo-map", "sbs", "full")
    crop_size("t6a", (320, 360), "SBS 全幅 → 单眼 320x360")

    rpc.call("set-stereo-map", "tab", "full")
    crop_size("t6b", (320, 90), "TAB 全幅 → 单眼 320x90")

    rpc.call("set-stereo-map", "sbs", "half")
    crop_size("t6c", (320, 180), "SBS 半幅 → 还原宽高比 320x180")

    rpc.call("set-stereo-map", "off", "full")
    crop_size("t6d", (320, 180), "复位 → 320x180")


def t9_toggle(p: Player) -> None:
    section("T9 「界面」菜单开关缩略图")
    rpc = p.rpc

    rpc.call("set-preview-thumbnails", "1")
    wait_until(lambda: rpc.call("get-preview-state").get("data", {}).get("ready") is True,
               "缩略图就绪", timeout=20)
    r = preview(rpc, "t9a", 20)
    check(r["ok"], "开启时有缩略图", r["err"])

    # 菜单路径：界面 → 缩略图（toggle 后菜单保持打开）
    p.open_menu()
    st = {}
    for _ in range(40):
        st = settle(p)
        row = st.get("currentRow", "")
        if "Interface" in row or "界面" in row:
            break
        rpc.key("Down")
        time.sleep(0.05)
    row = st.get("currentRow", "")
    check("Interface" in row or "界面" in row, "导航到「界面」行", row)
    rpc.key("Right")
    st = settle(p)
    expect_eq(st.get("subOpen"), 1, "二级面板展开")

    rpc.key("Enter")
    st = settle(p)
    expect_eq(st.get("ctxVisible"), 1, "toggle 后菜单保持打开")
    off = wait_until(lambda: (lambda d: d if d and d.get("thumbs") is False else None)(
        rpc.call("get-preview-state").get("data")), "缩略图关闭", timeout=10)
    check(off.get("ready") is False, "关闭后预览实例卸载", off)
    r2 = rpc.call("preview", str(OUT / "t9_off.png"), "21", timeout=10)
    check("not available" in (r2.get("error") or ""), "关闭后明确报不可用", r2)

    rpc.key("Enter")
    wait_until(lambda: rpc.call("get-preview-state").get("data", {}).get("ready") is True,
               "重新开启并载入", timeout=20)
    rpc.key("Escape")
    time.sleep(0.2)
    rpc.key("Escape")
    time.sleep(0.3)
    r3 = preview(rpc, "t9b", 22)
    check(r3["ok"], "重新开启后有缩略图", r3["err"])


class _Chaos:
    """测试用的"网络抖动"开关：首响应延迟 + 可切换的 403 封锁。"""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.delay_s = 0.0      # 第一次响应前 sleep（模拟慢解析/慢首包）
        self.block = False      # True = 所有媒体请求返回 403
        self.hits = 0           # 媒体请求计数（不含 /control）
        self.delayed = False


def _start_range_server(directory: Path,
                        chaos: "_Chaos | None" = None
                        ) -> tuple[http.server.ThreadingHTTPServer, int]:
    """本地 HTTP 点播服务（**支持 Range**）。

    python -m http.server 不返回 206/Accept-Ranges，mpv 会把它当"不可 seek 的
    线性流"→ 预览按直播被禁用；这里用一个小 handler 提供 Range，模拟真实点播。
    `chaos` 非空时可注入延迟/403（T12/T13 用；T10 不传，行为不变）。
    """

    class Handler(http.server.SimpleHTTPRequestHandler):
        def send_head(self):
            if chaos is not None:
                if self.path.startswith("/control"):
                    with chaos.lock:
                        chaos.block = "block=1" in self.path
                    self.send_response(200)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return None
                with chaos.lock:
                    chaos.hits += 1
                    blocked = chaos.block
                    slow = chaos.delay_s > 0 and not chaos.delayed
                    if slow:
                        chaos.delayed = True
                if slow:
                    time.sleep(chaos.delay_s)
                if blocked:
                    body = b"blocked"
                    self.send_response(403)
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return None
            path = self.translate_path(self.path)
            if not Path(path).is_file():
                self.send_error(404)
                return None
            size = Path(path).stat().st_size
            rng = self.headers.get("Range")
            start, end = 0, size - 1
            code = 200
            if rng:
                m = re.match(r"bytes=(\d*)-(\d*)", rng)
                if m and (m.group(1) or m.group(2)):
                    if m.group(1):
                        start = int(m.group(1))
                    if m.group(2):
                        end = min(int(m.group(2)), size - 1)
                    elif m.group(1):
                        end = size - 1
                    if not m.group(1) and m.group(2):
                        start = max(0, size - int(m.group(2)))
                    code = 206
            f = open(path, "rb")
            f.seek(start)
            self.send_response(code)
            self.send_header("Content-Type", "video/x-matroska")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(end - start + 1))
            if code == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            return f

        def log_message(self, *a):
            pass

    handler = functools.partial(Handler, directory=str(directory))
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def t12_deferred_hover() -> None:
    section("T12 悬停落在实例就绪前 → 就绪后补发（不需要再动指针）")
    chaos = _Chaos()
    chaos.delay_s = 2.5            # 主实例首包慢 → 悬停时预览必然还没就绪
    srv, port = _start_range_server(FIXTURE.parent, chaos)
    sock = SOCK.with_name("vsr-thumb-defer.sock")
    log = LOG.with_name("vsr-thumb-defer.log")
    sock.unlink(missing_ok=True)
    log.unlink(missing_ok=True)
    logf = log.open("wb")
    proc = subprocess.Popen(
        [str(BIN), f"http://127.0.0.1:{port}/{FIXTURE.name}",
         "--rpc-socket", str(sock), "--", "--msg-level=all=v"],
        stdout=logf, stderr=subprocess.STDOUT,
        env=dict(os.environ, VSR_LOG_VERBOSE="1"))
    try:
        wait_until(lambda: sock.exists(), "RPC 套接字", timeout=20)
        rpc = Rpc(sock)
        rpc.call("set-preview-thumbnails", "1")
        # 只发这一次悬停，之后不再碰指针（模拟"打开就悬停、然后不动了"）
        rpc.call("debug-hover", "60")
        st = rpc.call("get-preview-state").get("data") or {}
        check(st.get("serial", 0) == 0, "悬停瞬间还没有图", st)
        st = wait_until(lambda: (lambda d: d if d.get("serial", 0) > 0 else None)(
            rpc.call("get-preview-state").get("data")), "不移动指针也能出图（补发）",
            timeout=40)
        check(st.get("ready") is True, "补发后 ready", st)
        check("replay deferred hover request" in log.read_text(errors="ignore"),
              "日志确认走了补发路径")
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=6)
        except Exception:
            proc.kill()
        logf.close()
        srv.shutdown()


def t13_failed_load_retry() -> None:
    section("T13 装载失败：3 s 冷却内不重试，过冷却后由悬停触发重试")
    chaos = _Chaos()
    srv, port = _start_range_server(FIXTURE.parent, chaos)
    def ctl(q: str) -> None:
        urllib.request.urlopen(f"http://127.0.0.1:{port}/control?{q}").read()
    try:
        url = f"http://127.0.0.1:{port}/{FIXTURE.name}"
        with Player(SOCK.with_name("vsr-thumb-retry.sock"),
                    LOG.with_name("vsr-thumb-retry.log"), [url]) as p:
            rpc = p.rpc
            rpc.call("set_property", "pause", "yes")
            rpc.call("set-preview-thumbnails", "1")
            time.sleep(0.5)
            ctl("block=1")          # 预览的装载会 403（主实例已缓冲，不受影响）
            t_fail = time.time()
            rpc.call("debug-hover", "300")
            wait_until(lambda: (rpc.call("get-preview-state").get("data") or {}).get("error"),
                       "预览装载失败", timeout=25)
            check(True, "装载失败反映到 error")

            hits = chaos.hits
            time.sleep(0.5)
            rpc.call("debug-hover", "301")     # 冷却期内：应被忽略
            time.sleep(1.2)
            expect_eq(chaos.hits, hits, "冷却期内不发重试请求")
            check((rpc.call("get-preview-state").get("data") or {}).get("ready") is False,
                  "冷却期内仍未就绪")

            ctl("block=0")
            left = 3.3 - (time.time() - t_fail)
            if left > 0:
                time.sleep(left)
            rpc.call("debug-hover", "305")     # 过冷却 → 悬停触发重试
            st = wait_until(lambda: (lambda d: d if d.get("ready") and d.get("serial", 0) > 0
                                    else None)(rpc.call("get-preview-state").get("data")),
                            "重试后出图", timeout=30)
            check(st.get("ready") is True, "重试成功", st)
            expect_eq(st.get("error"), None, "成功后错误状态清空")
    finally:
        srv.shutdown()


def t10_network() -> None:
    section("T10 网络流（非直播）缩略图")
    srv, port = _start_range_server(FIXTURE.parent)
    try:
        url = f"http://127.0.0.1:{port}/{FIXTURE.name}"
        with Player(SOCK.with_name("vsr-thumb-net.sock"),
                    LOG.with_name("vsr-thumb-net.log"), [url]) as p:
            # 懒加载：网络源在**第一次请求前**不载入（用户 2026-11-15 定）
            st0 = p.rpc.call("get-preview-state").get("data") or {}
            check(st0.get("loaded") is False, "网络源首次请求前不载入（懒加载）",
                  st0.get("loaded"))
            p.rpc.call("set-preview-width", "320")
            # 第一次请求 = 触发懒加载（此时实例可能还没就绪，不吃返回值）
            p.rpc.call("preview", str(OUT / "t10-warm.png"), "6")
            st = wait_until(
                lambda: (lambda d: d if d and d.get("ready") else None)(
                    p.rpc.call("get-preview-state").get("data")),
                "网络流预览就绪", timeout=30)
            check(st.get("ready") is True, "网络流（非直播）启用预览", st)
            r = preview(p.rpc, "t10", 6)
            check(r["ok"], "网络流取帧成功", r["err"])
            if r["ok"]:
                s = image_stats(r["path"])
                expect_eq((s["w"], s["h"]), (320, 180), "网络流缩略图 320x180")
    finally:
        srv.shutdown()


def t11_hide_disabled_toggle(p: Player) -> None:
    section("T11 「界面 → 隐藏无效菜单项」开关")
    rpc = p.rpc

    def visible_rows() -> list[int]:
        st = settle(p)
        return [r["idx"] for r in st.get("rows", []) if r["vis"] and r["h"] >= 30]

    # 默认（hide=0）：禁用项灰显占位
    rpc.call("set-menu-hide-invalid", "0")
    time.sleep(0.3)
    p.open_menu()
    shown_rows = visible_rows()
    p.close_menu()

    # 打开隐藏（hide=1）：禁用项不显示
    rpc.call("set-menu-hide-invalid", "1")
    time.sleep(0.3)
    p.open_menu()
    hidden_rows = visible_rows()
    p.close_menu()
    rpc.call("set-menu-hide-invalid", "0")

    check(len(shown_rows) > len(hidden_rows), "默认模式可见行更多（含禁用项）",
          f"shown={len(shown_rows)} hidden={len(hidden_rows)}")
    check(not set(hidden_rows) - set(shown_rows), "隐藏模式是默认模式的子集")


def t7_audio() -> None:
    section("T7 纯音频不启用")
    with Player(SOCK.with_name("vsr-thumb-audio.sock"),
                LOG.with_name("vsr-thumb-audio.log"), [str(AUDIO)]) as p:
        st = p.rpc.call("get-preview-state").get("data") or {}
        check(st.get("ready") is False, "音频文件 previewReady=false", st)
        r = p.rpc.call("preview", str(OUT / "audio.png"), "1", timeout=10)
        check("not available" in (r.get("error") or ""), "明确报不可用（不空等）", r)


def t8_3d_window(p: Player) -> None:
    section("T8 3D 会话：面板在桌面 control 窗口")
    rpc = p.rpc
    screens = rpc.screens()
    win_screen = rpc.stereo()["win"].split("@")[-1]
    target = next((s for s in screens if not s["primary"] and s["name"] != win_screen), None)
    if target is None:
        print("  （跳过：没有可用的副屏；本机把主屏当眼镜屏会误判，用户 2026-11-03 提醒）")
        return
    rpc.call("stereo-start", "1", str(target["index"]))
    st = wait_until(lambda: (lambda s: s if s["active"] == 1 else None)(rpc.stereo()),
                    "进入 3D", timeout=25)
    check(st["active"] == 1, "3D 激活", st.get("screen"))
    time.sleep(0.5)

    plain = OUT / "ctl_plain.png"
    hover = OUT / "ctl_hover.png"
    for f in (plain, hover):
        f.unlink(missing_ok=True)
    rpc.call("debug-hover", "off")
    time.sleep(0.4)
    rpc.call("grab-window", str(plain), "control")
    wait_until(lambda: plain.exists() and plain.stat().st_size > 0, "control 基准截图", 10)
    time.sleep(0.6)

    rpc.call("debug-hover", "12")
    time.sleep(1.0)
    rpc.call("grab-window", str(hover), "control")
    wait_until(lambda: hover.exists() and hover.stat().st_size > 0, "control 悬停截图", 10)
    time.sleep(0.8)
    check(plain.read_bytes() != hover.read_bytes(), "3D 下 control 窗口出现预览面板")

    rpc.call("debug-hover", "off")
    rpc.call("stereo-stop")
    wait_until(lambda: rpc.stereo()["active"] == 0, "退出 3D", timeout=25)


def main() -> int:
    ensure_fixtures()
    OUT.mkdir(exist_ok=True)
    section("启动（带章节的 30s 测试片）")
    with Player(SOCK, LOG, [str(FIXTURE)]) as p:
        p.rpc.call("set_property", "pause", "yes")
        # 开关是持久化的（右键菜单「界面 → 缩略图」）：上一轮跑成关闭状态时
        # 这一轮要先明确打开，否则 T1 永远等不到实例载入。
        p.rpc.call("set-preview-thumbnails", "1")
        wait_until(lambda: p.rpc.call("get-preview-state").get("data", {}).get("thumbs") is True,
                   "缩略图开关打开", timeout=10)
        t1_basic(p)
        t2_bucket_cache(p)
        t3_width(p)
        t4_chapters(p)
        t5_ui(p)
        t6_3d_crop(p)
        t9_toggle(p)
        t8_3d_window(p)
    t7_audio()
    with Player(SOCK.with_name("vsr-thumb-dim.sock"),
                LOG.with_name("vsr-thumb-dim.log"), [str(FIXTURE)]) as p:
        t11_hide_disabled_toggle(p)
    t10_network()
    t12_deferred_hover()
    t13_failed_load_retry()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
