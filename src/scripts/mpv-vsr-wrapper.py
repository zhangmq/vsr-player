#!/usr/bin/env python3
"""mpv wrapper: cookie export + yt-dlp playlist extraction + mpv launch.

Called by ff2mpv-rust. Exports Chrome cookies on each invocation.

Default: extract playlist via yt-dlp (supports YouTube/Bilibili/Niconico).
Use --no-playlist to skip extraction and pass URL directly to mpv.
"""

import os
import sys
import re
import subprocess
import shlex
import time
from datetime import datetime

XDG_CACHE = os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache"))
COOKIE_FILE = os.path.join(XDG_CACHE, "yt-dlp", "cookies.txt")
LOG_FILE = os.path.join(XDG_CACHE, "yt-dlp", "wrapper.log")


def log(msg: str) -> None:
    os.makedirs(os.path.dirname(LOG_FILE), exist_ok=True)
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    with open(LOG_FILE, "a") as f:
        f.write(f"[{ts}] {msg}\n")


_notify_id = None  # reused across steps for a single flowing notification


# desktop notification spec says -t 0 = never expire, but some servers
# treat 0 as default. use a large explicit value for persistent.
PERSIST = 3600000  # 1 hour in ms


def notify(summary: str, body: str, duration: int = 0) -> str:
    """Send or update desktop notification. Returns the notification ID.

    Set duration=0 for persistent (updated by next call), >0 for auto-dismiss.
    """
    global _notify_id
    if duration <= 0:
        duration = PERSIST
    args = ["notify-send", "-u", "critical", summary, body,
            "-t", str(duration), "-p"]
    if _notify_id:
        args.extend(["-r", _notify_id])
    result = subprocess.run(
        args, capture_output=True, text=True,
    )
    nid = result.stdout.strip()
    if nid:
        _notify_id = nid
    return nid


def export_cookies() -> tuple[int, int]:
    """Export Chrome cookies to file. Returns (exit_code, num_entries)."""
    os.makedirs(os.path.dirname(COOKIE_FILE), exist_ok=True)
    log("exporting cookies from Chrome")
    notify("正在导出 Cookie", "从 Chrome 获取登录状态")

    t0 = time.time()
    # 删除旧 cookie 文件，防止残留过期 cookie 污染 Chrome 新导出的
    try:
        os.remove(COOKIE_FILE)
    except FileNotFoundError:
        pass
    try:
        result = subprocess.run(
            [
                "yt-dlp", "--cookies-from-browser", "chrome",
                "--cookies", COOKIE_FILE,
                "--skip-download", "https://httpbin.org/ip",
            ],
            capture_output=True, text=True,
            timeout=30,
        )
    except subprocess.TimeoutExpired:
        elapsed = time.time() - t0
        log(f"cookie export timed out after {elapsed:.0f}s")
        notify("Cookie 导出超时", "密钥环可能未解锁，使用已有 cookie", 5000)
        count = 0
        try:
            with open(COOKIE_FILE) as f:
                for line in f:
                    if not line.startswith("#") and line.strip():
                        count += 1
        except FileNotFoundError:
            pass
        return -1, count

    elapsed = time.time() - t0

    count = 0
    try:
        with open(COOKIE_FILE) as f:
            for line in f:
                if not line.startswith("#") and line.strip():
                    count += 1
    except FileNotFoundError:
        pass

    log(f"cookie export done: exit={result.returncode}, entries={count}, took={elapsed:.1f}s")
    if result.stderr.strip():
        log(f"cookie stderr: {result.stderr.strip()[:300]}")

    if result.returncode == 0 and count >= 10:
        notify("Cookie 已就绪", f"{count} 条，耗时 {elapsed:.0f}s")
    else:
        notify("Cookie 导出异常", f"exit={result.returncode}, {count} 条", 5000)

    return result.returncode, count


BIN = os.path.expanduser("~/.local/bin/mpv-vsr")

def mpv_base_args() -> list:
    return [
        "stdbuf", "-eL",
        # --vf=vsr:<option>=<value>:<option>=<value>...
        #   scale:   off | auto | 2 | 3 | 4            (default: auto)
        #   denoise: off | low | medium | high | ultra  (default: off)
        #   quality: low | medium | high | ultra        (default: high)
        BIN, "--force-window=yes",
        "--no-config",
        "--vo=gpu-next",
        "--gpu-api=vulkan",
        "--hwdec=auto-safe",
        "--script-opts=ytdl_hook-ytdl_path=yt-dlp",
        "--demuxer-lavf-analyzeduration=2",
        "--vf=vsr:scale=auto:denoise=off:quality=high",
        f"--ytdl-raw-options=cookies={COOKIE_FILE},no-playlist=",
    ]


RE_YTDL_ERROR = re.compile(r"\[ytdl_hook\] ERROR", re.IGNORECASE)
RE_COOKIE_WARN = re.compile(r"cookies.*(?:invalid|no longer valid|expired)", re.IGNORECASE)


MAX_CONSECUTIVE_ERRORS = 3


def _monitor_mpv(proc: subprocess.Popen, *, is_playlist: bool = False) -> None:
    """Monitor mpv stdout: push errors to notification, kill on consecutive errors.

    Single video (is_playlist=False): kill on 1st ytdl_hook ERROR.
    Playlist (is_playlist=True):     kill after N consecutive ERRORs, reset
                                      count when a video starts playing (AV:).
    """
    playing = False
    cookie_warned = False
    consecutive_errors = 0
    threshold = MAX_CONSECUTIVE_ERRORS if is_playlist else 1

    log(f"_monitor_mpv: entered (is_playlist={is_playlist}, threshold={threshold})")
    for line in proc.stdout:
        log(f"mpv: {line.strip()[:120]}")
        sys.stderr.write(line)
        sys.stderr.flush()

        if RE_YTDL_ERROR.search(line):
            consecutive_errors += 1
            err_msg = re.sub(r'^.*ERROR:\s*', '', line.strip())
            log(f"ytdl_hook error #{consecutive_errors}/{threshold}: {err_msg}")
            if consecutive_errors >= threshold:
                notify("播放失败",
                       f"连续 {consecutive_errors} 次解析错误，已停止\n{err_msg}", 7000)
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                return
            else:
                notify("解析错误",
                       f"({consecutive_errors}/{threshold}) {err_msg}", 0)

        if not cookie_warned and RE_COOKIE_WARN.search(line):
            cookie_warned = True
            log("cookie warning detected")
            notify("⚠ Cookie 可能已失效", "播放可能受限，请在 Chrome 中刷新登录状态", 0)

        if not playing and line.startswith("AV:"):
            time.sleep(0.5)
            notify("正在播放", "", 1500)
            playing = True
            consecutive_errors = 0  # 成功播放，重置
            log("playback started, notification dismissed")

    proc.wait()
    log(f"mpv exited: {proc.returncode}")


def launch_mpv(cmd: list, *, is_playlist: bool = False) -> None:
    """Launch mpv and monitor until playback."""
    log(f"launching: {shlex.join(cmd)}")
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
    _monitor_mpv(proc, is_playlist=is_playlist)


def extract_playlist(url: str) -> list:
    """Run yt-dlp --flat-playlist, return list of video URLs."""
    log(f"extracting playlist: {url}")
    notify("正在解析播放列表", "提取视频地址中，请稍候")

    t0 = time.time()
    try:
        result = subprocess.run(
            [
                "yt-dlp", f"--cookies={COOKIE_FILE}",
                "--flat-playlist", "--print", "url",
                "--extractor-args", "youtubetab:skip=authcheck",
                url,
            ],
            capture_output=True, text=True,
            timeout=60,
        )
    except subprocess.TimeoutExpired:
        elapsed = time.time() - t0
        log(f"playlist extraction timed out after {elapsed:.0f}s")
        notify("播放列表解析超时", "将直接打开链接尝试播放", 5000)
        return []

    elapsed = time.time() - t0

    combined = result.stdout + "\n" + result.stderr
    log(f"playlist exit={result.returncode}, took={elapsed:.1f}s")
    if result.stderr.strip():
        log(f"playlist stderr: {result.stderr.strip()[:300]}")

    urls = [u for u in result.stdout.split("\n") if re.match(r"^https?://", u)]
    log(f"playlist urls: {len(urls)}")

    return urls


def handle_playlist(url: str) -> None:
    """Extract playlist and launch mpv."""
    url_list = extract_playlist(url)
    count = len(url_list)

    if count > 1:
        notify("播放列表就绪", f"共 {count} 个视频，正在启动播放器")
        log(f"launching mpv --playlist=- with {count} urls")
        cmd = mpv_base_args() + ["--playlist=-"]
        log(f"launching: {shlex.join(cmd)}")
        proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True,
        )
        proc.stdin.write("\n".join(url_list))
        proc.stdin.close()
        _monitor_mpv(proc, is_playlist=True)
    elif count == 1:
        log("playlist returned 1 url, playing directly")
        notify("正在启动播放器", url)
        launch_mpv(mpv_base_args() + [url])
    else:
        log("playlist returned 0 urls, falling back to direct mpv")
        notify("播放列表解析失败", "将直接打开链接尝试播放", 5000)
        launch_mpv(mpv_base_args() + [url])


def handle_single(url: str) -> None:
    """Pass URL directly to mpv without playlist extraction."""
    log("route: single (--no-playlist)")
    notify("正在启动播放器", url)
    launch_mpv(mpv_base_args() + [url])


def main() -> None:
    no_playlist = False
    url = ""

    for arg in sys.argv[1:]:
        if arg == "--no-playlist":
            no_playlist = True
        elif arg != "--":
            url = arg

    if not url:
        log("ERROR: no URL in args")
        sys.exit(1)

    log("=== wrapper invoked ===")
    log(f"raw args: {' '.join(shlex.quote(a) for a in sys.argv[1:])}")
    log(f"url: {url}")
    log(f"mode: {'single' if no_playlist else 'playlist'}")

    notify("收到链接，请等待播放器启动", url)

    export_cookies()

    if no_playlist:
        handle_single(url)
    else:
        handle_playlist(url)


if __name__ == "__main__":
    main()
