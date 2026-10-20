# ff2mpv 接入调研（vsr-player × ff2mpv-rust）

- 日期：2026-10-18（2026-10-20 收尾）
- 分支：`investigate/ff2mpv-support`（基线 `3fee0af` / v0.4.0）→ 已合并 master（#33），随 **v0.4.1** 发布
- 目标：搞清"浏览器里点 ff2mpv → 用 vsr-player 播放"缺什么，给出接入方案与改动点
- 现状一句话：**单条 URL 今天就能播**；用户现有 wrapper（mpv CLI 参数表 + `--playlist=-` + stdout 监控）**一条都喂不进去**

## 0. 结论速览

1. ff2mpv 的调用契约是**一条 argv + 完全分离的 stdio**：`player_command` 后跟 `player_args`、扩展的 options、最后是 URL；子进程 stdout/stderr 被重定向到 `/dev/null`、`process_group(0)` 脱离、**不等待退出**。→ 参数不被接受时表现为"点了没反应"（连报错都看不到）。
2. 用户 wrapper 里的那串选项是 **mpv CLI 语法**，vsr-player 只认自己的选项：实测 `vsr-player --no-config …` → `error: unknown option: --no-config` 直接退出。
3. **单 URL 已经不是缺口**：`vsr-player -- <url>` 就被接受并播放（实测 http 源 3601 帧 / 7.9 s / 457.9 fps），而且**按 ff2mpv 的 native messaging 协议端到端验证过**——把 `~/.config/ff2mpv-rust.json` 临时指向 dev 构建，喂 `{"url": …, "options": []}`：ff2mpv-rust 回 `"ok"` 并拉起 `vsr-player -- http://…`，应用真的载入播放（播放列表被改写成该 URL、`playlistCurrent=0`；测试后配置已还原）。`yt-dlp` 通路也是完整的——libmpv 默认 `ytdl=yes`、`@ytdl_hook.lua` 已编译进 libmpv、`--` 之后的 `--ytdl-raw-options=cookies=…` 直达 yt-dlp 命令行（实测 yt-dlp 报 `no such option: --definitely-not-an-option`）。
4. 真正的缺口只有**三个**（第四个已排除）：
   - **G1 mpv 风格选项被拒**（`--no-config` / `--force-window` / `--script-opts` / `--demuxer-lavf-analyzeduration` …）——**wrapper 侧就能解决**：把这些选项挪到 `--` 之后透传（表 #4 证明所需选项 libmpv 全部接受）；
   - **G2 播放列表不支持**：多个位置参数报 `multiple video files`；`--` 之后多个非选项参数**只有最后一个生效**（其余静默丢弃）；`--playlist=` 不认；
   - **G3 `--playlist` 在 libmpv 里根本不存在**——它是 cplayer（命令行）专有选项，`mpv_set_option_string("playlist", …)` 返回 `-5 option not found`（探针 + mpv 源码双证），所以**必须应用侧实现**；这是唯一必须动 app 的地方；
   - ~~G4 stdout 无状态行~~ **不是缺口**：透传 `--terminal=yes` 后 stdout 就有 `AV: …` 与 `[ytdl_hook] ERROR: …`，与 wrapper 现有正则逐字匹配（实测，见 §3 #11），监控代码零改动。
5. 次要但有影响的三个坑：正常播放路径下 CLI/URL 载入会**覆盖并保存**用户播放列表（实测 conf 从 `catlove_loop_470s.webm` 变成 `http://127.0.0.1:8931/`；**已定：保持现状**，应用本来就是"退出时保留上一次状态（含列表）"的模型）；**`--msg-level` 透传会被应用覆盖**（改不了日志级别）；**多开时后启动的实例抢走 `/tmp/vsr-player.sock`**。
6. 定稿方案（D4）：**app 加 `--playlist=<file|->` + 多位置参数=播放列表（对齐 mpv）；wrapper 改 BIN、把 mpv 选项挪进 `--` 之后并加 `--terminal=yes`**。改动集中在 `Options.cpp` / `main.cpp`，不碰管线与 UI。"未知 `--xxx` 透传"**不做**——`-- <mpv-opt>` 本来就有支持，只是形式不同（见 §3.5）。**当前先不动代码**，改动点见 §6。

## 1. ff2mpv 侧：调用契约（源码 + 本机实测）

**组件**

| 组件 | 位置 / 版本 | 作用 |
|---|---|---|
| 浏览器扩展 | Chrome 扩展 `ephjcajbkgplkjmelpglennepbpmdpjg` 6.0.0 | 取当前 tab/link 的 URL（以及 profiles 的 options）→ `sendNativeMessage("ff2mpv", {url, options})` |
| native host | `/usr/bin/ff2mpv-rust` 1.1.7（pacman `ff2mpv-rust`） | 读 native message → 启动播放器 |
| host manifest | `/usr/lib/mozilla/native-messaging-hosts/ff2mpv.json`（Chromium 系在 `/etc/chromium|/etc/opt/chrome/…`） | `path=/usr/bin/ff2mpv-rust`、`allowed_extensions=[ff2mpv@yossarian.net]` / `allowed_origins=[chrome-extension://ephjcajbkgplkjmelpglennepbpmdpjg/]` |
| 配置 | `~/.config/ff2mpv-rust.json`（本机：`{"player_command": "/home/zmq/.local/bin/mpv-vsr-wrapper.py", "player_args": ["--"]}`） | 播放器命令与固定参数 |

**启动逻辑**（`ff2mpv-rust` 1.1.7 `src/command.rs:76-99`、`src/config.rs:17-24`）

```
argv  = [player_command] + player_args + extension_options + [url]
stdio: stdout → /dev/null, stderr → /dev/null
detach: process_group(0)（unix）
spawn() 后立刻给浏览器回 "ok"，不等待子进程退出
默认值（无配置文件时）: player_command="mpv", player_args=["--no-terminal", "--"]
```

要点：
- 扩展的 **options 是"每个 profile 的文本行"**，逐条追加在 `player_args` 之后（`ff2mpv.js` 的 `getOptions()`），用户 wrapper 正是用它接收 `--no-playlist`。
- 没有 shell，不解析引号；**每一项就是一个 argv**。
- stdout/stderr 被丢弃 → 任何"启动失败"在浏览器侧无声。用户现在遇到的"换成 vsr-player 不行"就是这条：`vsr-player` 打印 `error: unknown option: --no-config` 后退出了。

## 2. 用户 wrapper 依赖的 mpv 能力

`~/.local/bin/mpv-vsr-wrapper.py`（10 KB）：

| 行 | 能力 | 用到的 mpv 行为 |
|---|---|---|
| 118 | `BIN = ~/.local/bin/mpv-vsr` | 可执行文件路径（要换成 vsr-player 就是这一行） |
| 120-136 | 固定参数表 | `--force-window=yes`、`--no-config`、`--vo=gpu-next`、`--gpu-api=vulkan`、`--hwdec=auto-safe`、`--script-opts=ytdl_hook-ytdl_path=yt-dlp`、`--demuxer-lavf-analyzeduration=2`、`--vf=hwup,vsr:scale=auto:denoise=off:quality=high,vfg`、`--ytdl-raw-options=cookies=<file>,no-playlist=` |
| 207-267 | 播放列表 | 先 `yt-dlp --flat-playlist --print url` 自己展开 → **`--playlist=-` 并用 stdin 喂 URL 列表** |
| 146-195 | 播放监控 | 逐行读 **stdout**：`line.startswith("AV:")` = 开始播放；`[ytdl_hook] ERROR` = 解析失败计数（单视频阈值 1、播放列表 3）→ `notify-send` + 终止 |
| 59-115 | Cookie | `yt-dlp --cookies-from-browser chrome --cookies <file>`，路径经 `--ytdl-raw-options=cookies=` 传给播放器 |

mpv 侧这两个行为的实测口径（作为要镜像的参考语义）：

```
$ printf 'http://127.0.0.1:8931/catlove_480p.webm\n' | mpv --no-config --vo=null --ao=null --frames=1 --playlist=-
Playing: http://127.0.0.1:8931/catlove_480p.webm
...
$ mpv --no-config --vo=null --input-terminal=no --length=3 input/goose_480p.webm 2>/dev/null | cat -A
● Video  --vid=1               (av1 854x480 30 fps) [default]
AV: 00:00:00 / 00:05:05 (0%) A-V:  0.000$      ← wrapper 监控的就是这一行
```

wrapper 监控命中之后的**行为**（回答"后续是什么行为"）：

| 事件 | 行为（`_monitor_mpv`） |
|---|---|
| stdout 有 `AV:` 开头的行 | 判为"开始播放"：`sleep(0.5)` → `notify-send -t 1500 "正在播放"`（自动消失，用来替换启动时那条常驻通知）、误差计数清零 |
| `[ytdl_hook] ERROR` | 计数 +1；**未到阈值** → 常驻通知"解析错误 (n/N)"继续监控；**达到阈值**（单视频 1 次 / 播放列表连续 3 次）→ 通知"播放失败：连续 N 次解析错误，已停止"→ `proc.terminate()`（5 s 未退则 `kill`）并结束监控 |
| cookie 告警（`cookies … invalid/expired`） | 一次常驻通知"⚠ Cookie 可能已失效" |
| 其它行 | 原样转发到 wrapper 自己的 stderr + 追加写 `~/.cache/yt-dlp/wrapper.log` |

即：监控的用途 = **给用户桌面通知** + **解析连续失败时自动收掉播放器**（避免留一个空窗口/一直重试）。没有其它逻辑依赖这两类行。

## 3. vsr-player 现状：能力矩阵

（"实测"= 本次在本机 `build/src/client/vsr-player` 上跑出来的；"源码"= 仓库行号）

| # | 需要的能力 | 现状 | 证据 |
|---|---|---|---|
| 1 | 单个 URL 位置参数 | ✅ 支持 | 实测 `--benchmark -- http://127.0.0.1:8931/catlove_480p.webm` → `frames: 3601 / 7.9s / 457.9 fps`（= 服务器上那个 120.061 s@30fps 文件，帧数完全对上）；另见附录"ff2mpv native messaging 端到端" |
| 2 | 内嵌 yt-dlp（含 cookies） | ✅ 完整 | libmpv 默认 `ytdl=yes`、`load-scripts=yes`（`tests/ff2mpv/mpv_defaults_probe`）；`libmpv.so` 内含 `@ytdl_hook.lua`；实测应用日志 `[mpv error] ERROR: Unsupported URL: http://127.0.0.1:8931/`（= ytdl_hook 调了 yt-dlp） |
| 3 | `--ytdl-raw-options` 透传 | ✅ | `vsr-player -- --ytdl-raw-options=definitely-not-an-option=1 <url>` → 应用 stderr `yt-dlp: error: no such option: --definitely-not-an-option`（说明选项真的进了 yt-dlp argv） |
| 4 | `--script-opts` / `--demuxer-lavf-analyzeduration` / `--force-window` 透传 | ✅（作为 mpv 选项） | `tests/ff2mpv/mpv_option_probe`：三者 `rc=0` |
| 5 | 未在 `--` 之后的 mpv 选项 | ❌ 直接退出 | 实测 `error: unknown option: --no-config`；`Options.cpp:113-116` |
| 6 | `--no-config` | ❌ libmpv 无此名 | 探针 `no-config → rc=-5 option not found`；libmpv 默认本就是 `config=no`（`mpv --show-profile=libmpv`） |
| 7 | 多个位置参数（播放列表） | ❌ 报错 | 实测 `error: multiple video files`；`Options.cpp:107-112` |
| 8 | `--` 之后多个非选项参数 | ⚠ 只有最后一个生效 | `Options.cpp:38-42`：`if (j == argc - 1 && video_file.empty())` 否则 `continue`（静默丢弃） |
| 9 | `--playlist=<file>` | ❌ 应用不认 + libmpv 不可达 | 实测 `error: unknown option: --playlist=-`；探针 `playlist → rc=-5 option not found`；mpv 源码 `options/options.c:490`（`{"playlist", CONF_TYPE_STRING, M_OPT_NOCFG\|M_OPT_FILE, .offset = -1}`，`.offset=-1` = 伪选项）+ `options/parse_commandline.c:192-202`（只在 CLI 命令行解析里特判）+ `player/main.c:370`（只有 cplayer 调那个解析器） |
| 10 | stdin 播放列表（`--playlist=-`） | ❌ | 应用不读 stdin（无相关代码）；libmpv profile `input-terminal=no` |
| 11 | stdout 状态行（wrapper 监控） | ✅ **透传 `--terminal=yes` 即可** | libmpv 默认 `terminal=no`（所以裸跑 stdout 为空）；但 `terminal` 是普通可设选项（探针 `rc=0`），且应用**不会**覆盖它（只在 benchmark 设 `terminal=no`）。实测 `vsr-player -- --terminal=yes <file>` → stdout 12 s 内 44 行 `AV: …`；`vsr-player -- --terminal=yes http://127.0.0.1:8931/` → stdout `[ytdl_hook] ERROR: Unsupported URL: …`（与 wrapper 正则 `\[ytdl_hook\] ERROR` 逐字匹配，见 `tests/ff2mpv/mpv_terminal_probe.c`） |
| 12 | 播放列表持久化 | ⚠ 正常播放路径下 CLI/URL 载入会**覆盖并保存** | `PlayerViewModel.cpp:215-216` 播放列表观察器无条件 `saveSettings("playlist", …)`；实测（非 benchmark）conf：`playlist=input/catlove_loop_470s.webm` → 载入 URL 后 `playlist=http://127.0.0.1:8931/`、`playlistCurrent=-1`。⚠ benchmark 不写设置——`saveSettings()` 由 `settingsEnabled_` 守卫（`PlayerViewModel.cpp:652-655`，只有 `loadSettings()` 打开它），所以用 benchmark 做实验时看不到这个覆盖 |
| 13 | `--msg-level` 透传 | ❌ 被应用覆盖 | `MpvController.cpp:53-54` 先应用透传，`:69/:76` 再设 `msg-level`（benchmark `all=no` / 正常 `all=status`）→ 透传的级别无效 |
| 14 | 应用自管选项被透传覆盖 | ⚠ 无保护 | `MpvController.cpp:43-54`：先 `vo=libmpv`/`hwdec=nvdec`/`vf=<应用链>`，**再**应用透传（同选项后者胜）→ wrapper 的 `--vo=gpu-next --vf=…` 会把应用自己的渲染/VFX 链顶掉 |
| 15 | 透传失败的处理 | ⚠ 静默 | `MpvController.cpp:53-54` 不看 `mpv_set_option_string` 返回值（拼错/不存在的选项无声忽略） |
| 16 | 播放结束行为 | ✅ 常驻窗口、列表续播 | `main.cpp:541`：非 benchmark 不处理 END_FILE；libmpv `idle=yes`、`keep-open=no` |
| 17 | 单实例 | ❌ 无（每次点开新窗口，与 mpv 一致） | 无 `QLockFile`/单实例代码 |
| 18 | 多开时 RPC socket | ⚠ 后启动者抢走 | `RpcServer.cpp:39-49`：`unlink(path)` 后 `bind` → 新实例拿到 `/tmp/vsr-player.sock`，旧实例的 RPC 静默失效 |

### 3.5 `-- <mpv-opt>` 透传：确实已有支持，只是"形式不同"

支持是存在的，机制 = **`--` 之后的一切都透传给 mpv**（`Options.cpp:34-54`；`--help` 的用法串里就写作 `[-- mpv-opt ...]`）：

```
vsr-player <自有选项> -- --ytdl-raw-options=cookies=x.txt,no-playlist= \
                        --demuxer-lavf-analyzeduration=2 <文件或 URL>
```

- `--name=value` 与 `--name value` 两种写法都吃；`--flag`（后面不是值）自动变 `flag=yes`。
- `--` 之前是应用自己的选项域：**不认识的 `--xxx` 在这里直接报错退出**（wrapper 这次踩的就是它）；`--` 之后才归 mpv。

与 mpv CLI 的四个差别（都用探针/源码验证过）：

1. **没有 `no-` 前缀糖**：`--no-config` 在 libmpv 里没有这个选项名（探针 `rc=-5 option not found`），要写 `--config=no`（libmpv 默认本就是 `config=no`，所以其实可以不传）。
2. **应用自管选项会被透传顶掉**：透传在 `vo=libmpv` / `hwdec=nvdec` / `vf=<应用链>` 之后应用（`MpvController.cpp:43-54`，同选项后者胜）→ 传 `--vo=gpu-next --vf=…` 会废掉应用自己的渲染/VFX 链。
3. **`--msg-level` 无效**：应用随后自己设 `all=status`（benchmark `all=no`）（`MpvController.cpp:69/:76`）。
4. **透传失败静默**：不检查 `mpv_set_option_string` 返回码（`MpvController.cpp:53-54`），拼错的选项无声无息。

另外 `--` 之后的**多个非选项参数只有最后一个生效**（§3 #8）——这正是"本事 2"要修的。

README 现状：只在"启动"那行举了 `-- --pause=yes` 一个例子，`-- <mpv-opt>` 没有写进命令行选项表；如果继续把它当正式用法，值得补一句。

## 4. 差距清单（按优先级）

- **G1 参数语法**：mpv 风格 `--xxx` 除了 `--` 之后一律拒绝。**不需要改 app**——wrapper 把 mpv 选项放进 `--` 之后即可（表 #4/#11 证明所需选项全部可透传）；改 app（未知选项透传）只是让 ff2mpv 直接指向 vsr-player / 脚本 / `xdg-open` 也能用。
- **G2 播放列表**：多文件、`--playlist=<file>`、`--playlist=-`（stdin）三种形态都没有。
- **G3 `--playlist` 必须应用侧实现**（libmpv 不可达，见上表 #9）——这是**唯一真正必须动 app 的地方**：wrapper 无法把列表交给应用（除非用 RPC，见方案 B）。
- ~~G4 监控通道~~ **不是缺口**：透传 `--terminal=yes` 后 stdout 上的 `AV:` 行与 `[ytdl_hook] ERROR` 行与 wrapper 现有正则逐字匹配（表 #11），wrapper 的监控代码**一行都不用改**。

## 5. 接入方案

### 方案 A（推荐）app 侧补播放列表 + 改 wrapper

- **A1（必须）`--playlist=<file|->`**：应用侧读文件或 stdin（读到 EOF），兼容 m3u（忽略空行与 `#` 注释）；条目 = 播放列表，首条播放、其余 append。给了 `--playlist` 时**不**走 idle 恢复、**不**恢复上次列表。
- **A2（建议）多位置参数 = 播放列表**（对齐 mpv：`mpv a b c` 就是三条列表项），替换现在"多文件报错/`--` 后只取最后一个"的行为。
- ~~A3 未知 `--xxx` 透传~~ **不做（D4）**：`-- <mpv-opt>` 已有支持（§3.5），只是形式不同；wrapper 场景不需要它。
- ~~A4 deny-list~~ **随 A3 取消**。
- ~~A5 临时列表~~ **已定不做**：应用语义就是"退出时保留上一次状态（含播放列表）"，浏览器点播进来的 URL/列表照常覆盖保存。
- ~~A6 监控通道~~ **无需改动**：wrapper 透传 `--terminal=yes` 即可（§3 #11 实测）。
- **A7（未决策，不在 D4 范围）**把透传的 `msg-level` 让位给应用（或加 `--log-level`），否则 ff2mpv 场景没法提日志级别排障。

成本：`Options.{h,cpp}` + `main.cpp`（playlist 展开与载入分支）；若做 A3/A4 再加 `MpvController` 的透传错误码处理。不碰渲染/VFX/UI。A1 的行为变更面很小（只多了一个新选项 + 新载入分支）。

### 方案 B 只做一个 vsr-player 专用 wrapper（不改 app）

把 wrapper 的 `mpv_base_args()` 改成 vsr-player 语法（BIN 指向 vsr-player、删掉 `--vo/--gpu-api/--hwdec/--vf/--no-config`、mpv 选项移入 `--` 之后），播放列表用 **RPC** 交给应用：启动应用 → 等 `/tmp/vsr-player.sock` → `playlist-clear` → 逐条 `loadfile <url> append` → `playlist-play-index 0`。
- 优点：零 app 改动。
- 缺点：① 需要"启动后不自动恢复/播放上次列表"的开关（现在无参数启动会 `restorePlaylist()` 直接播上次列表，`main.cpp:631-637`）；② 竞态（等 socket、命令排队）；③ 仍然覆盖用户播放列表（#12）；④ 多开抢 socket（#18）→ 需要给每个实例传 `--rpc-socket`。

### 方案 C 应用侧出 `--ytdl-playlist` 之类的"原生 ff2mpv 模式"

在应用里直接做 cookie 导出 + yt-dlp 展开 + 通知（把 wrapper 的活搬进 app）。
- 优点：一个进程、体验统一。
- 缺点：把"浏览器专属"逻辑（Chrome cookie 提取、notify-send）塞进播放器，违背单一职责；且 ytdl_hook 已经能做单视频（含 playlist EDL）——重复造轮子。**不建议**。

## 6. 改动点（D4 定稿：A1 + A2；A3/A4 不做；**当前先不动代码**）

1. `src/client/Options.{h,cpp}`
   - `std::string playlist;`（`--playlist=<f>` 与 `--playlist <f>`）。
   - `std::vector<std::string> files;`（所有位置参数，按顺序；A2）；`video_file` 保留 = `files[0]`（兼容现有调用点），不再报 `multiple video files`。
2. `src/client/main.cpp`
   - 解析顺序：`--playlist` 展开 → 与 `files` 合并（mpv 语义：`--playlist` 是"把列表文件的内容追加到列表里"）。
   - 展开：文件走 `QFile`，`-` 走 stdin 读到 EOF；m3u 语法（空行/`#` 注释跳过）。
   - 载入分支：列表非空 → `loadfile <first>`（replace）+ 逐条 `loadfile <x> append` + `playlist-play-index 0`；`idleStart` 与 `restorePlaylist()` 都不走。给了列表时**照常**持久化（= 现状语义，见 §7 D1）。
   - `--playlist` 只允许出现在 `--` 之前（`--` 之后是 mpv 选项透传，语义边界清晰）。
3. ~~`src/client/MpvController.cpp`（透传错误码 / `no-` 回退 / deny-list）~~ **不做（A3/A4 取消）**。
4. wrapper（`~/.local/bin/mpv-vsr-wrapper.py`，约 20 行；等 app 支持 `--playlist` 后再改）
   - `BIN = ~/.local/bin/vsr-player`。
   - `mpv_base_args()` 改成：应用选项在前、mpv 选项在 `--` 之后——
     `BIN, "--", "--terminal=yes", "--script-opts=ytdl_hook-ytdl_path=yt-dlp", "--demuxer-lavf-analyzeduration=2", "--force-window=yes", "--ytdl-raw-options=cookies=<file>,no-playlist="`；
     删除（应用自管/无意义）：`--no-config`（libmpv 默认已是 `config=no`）、`--vo=gpu-next`、`--gpu-api=vulkan`、`--hwdec=auto-safe`、`--vf=hwup,vsr:…`（应用自己构建链，透传会顶掉）。
   - `--terminal=yes` 是**监控能原样工作的关键**（§3 #11）。
   - 播放列表分支：`proc.stdin.write(...)` / `proc.stdin.close()` 保留，命令行改成 `BIN -- --terminal=yes … --playlist=-`。
   - 监控分支（`_monitor_mpv`）**不改**：`AV:` 与 `[ytdl_hook] ERROR` 都能在原通道看到。
   - （可选）把扩展 profiles 传来的未知 `--xxx` 透传到 `--` 之后（现在会被 `url = arg` 吞掉，见附录"wrapper 对 profiles 的处理"）。
5. 若要让 ff2mpv **直接**指向 vsr-player（不经 wrapper，仅单 URL）：`~/.config/ff2mpv-rust.json` 改
   `{"player_command": "<prefix>/bin/vsr-player", "player_args": ["--"]}` —— `--` 之后连扩展 options 一起当 mpv 选项解析、最后一个非选项当 URL，所以单 URL 今天就能用（§3 #1）；但 cookie/列表展开/通知仍要靠 wrapper。

## 7. 已定决策（2026-10-18）

- **D1 播放列表持久化**：按现状 —— 载入即覆盖并保存。理由（用户）：不存在"点链接"这个特殊场景，应用本来就是"退出时保留上一次状态（含播放列表）"的模型。→ 不做临时列表（原 A5 取消）。
- **D2 接入形态**：保留现有 wrapper（cookie 导出 + yt-dlp 展开 + 通知 + 错误监控）。
- **D3 监控通道**：wrapper 继续读 stdout，但参数里必须有 `--terminal=yes`（实测可拿到 `AV:` 与 `[ytdl_hook] ERROR`，wrapper 代码零改动）。
- **D4 实现范围**：只做"本事 1 + 本事 2"——`--playlist=<file|->` **和** 多位置参数 = 播放列表；**不做**"未知 `--xxx` 透传"（A3/A4 取消：`-- <mpv-opt>` 已有支持，见 §3.5，只是形式不同）。**已于 2026-10-18 实现，见 §8。**
- **D5 wrapper 落点**：直接改 `~/.local/bin/mpv-vsr-wrapper.py`（已改；改动前未留副本，事后用"反向替换 + round-trip 校验"重建了改动前版本 → `~/.local/bin/mpv-vsr-wrapper.py.orig`，10233 字节 = 原文件大小，见 §8）。
- 遗留 TODO（本次未做）：`data/vsr-player.desktop.in` 的 `Exec=… %f` 可改成 `%U`（多文件已是播放列表）；透传的 `msg-level` 让位（A7）。

## 8. 实现与验收（2026-10-18 落地）

**代码**（分支 `investigate/ff2mpv-support`）

| 文件 | 改动 |
|---|---|
| `src/client/Options.{h,cpp}` | `Options::Input{is_playlist,value}` + `inputs`（位置参数与 `--playlist` 按出现顺序登记）；`--playlist <f>` / `--playlist=<f>`；多位置参数不再报 `multiple video files`；benchmark 判据改成 `inputs.empty()`；用法串加 `[--playlist <file|->]` |
| `src/client/main.cpp` | `expandInputs()`（m3u/纯文本、跳过空行与 `#`、去 CR、`-` = stdin 读到 EOF；文件打不开 → 报错退出 1）；`idleStart` 判据改 `inputs.empty()`；载入分支：列表非空 → 首条 `loadFile`（replace+播放）+ 其余 `appendFile`；显式给输入但列表为空 → 不载入也不恢复上次列表 |
| `src/client/MpvController.{h,cpp}` | 新增 `appendFile()`（`loadfile <path> append`） |
| `README.md` / `README_zh.md` | CLI 表加 `--playlist`、`--` 两行 + "位置参数即列表条目"说明 |
| `AGENTS.md` | 「启动与会话行为」加命令行输入语义；修正 `Exec=%f`/`%U` 那条过时结论 |

**验收（本机实测）**

| 用例 | 结果 |
|---|---|
| `--benchmark <file>`（单文件回归） | `frames: 3603 / 8.1s / 446.1 fps` |
| `--benchmark a b`（多位置 = 列表） | RPC `playlist` = 2 条，第 1 条 `current/playing`，`playlist-pos=0` |
| `--playlist=<file>`（m3u + CRLF + `#EXTM3U`/`#EXTINF`/注释/空行） | 2 条，顺序正确 |
| `printf 'a\nb\n' \| vsr-player --benchmark --playlist=-` | 2 条 |
| `-- a b`（`--` 之后多个位置参数） | 2 条（旧行为只取最后一个） |
| `file-a -- --terminal=yes file-b`（顺序 + 透传共存） | 列表 `[a, b]`，透传生效 |
| `--playlist=<不存在>` | `playlist: cannot open '…'` + 退出码 1（未开窗口） |
| `--benchmark --playlist=/dev/null`（空列表） | 退出码 1 |
| **wrapper → app（ff2mpv 全链路，dev 构建）** | `PATH` 注入替身 yt-dlp（输出 2 条 http URL）/`notify-send`，跑 `wrapper -- <playlist-url>`：argv = `vsr-player --playlist=- -- --terminal=yes …`；RPC `playlist` = 2 条且首条播放中；wrapper 日志 `playlist urls: 2` → `launching vsr-player --playlist=- with 2 urls` → `playback started, notification dismissed`；stdout 56 行 `AV:`、0 条 ERROR |
| **wrapper 单 URL 路径**（`--no-playlist`） | 1 条、播放中、`AV:` 40 行、`playback started` |

**wrapper 改动**（`~/.local/bin/mpv-vsr-wrapper.py`）

- `BIN` → `~/.local/bin/vsr-player`；`mpv_base_args()` 拆成 `base_args()`（`stdbuf -eL` + BIN）+ `mpv_args()`（`--` + `--terminal=yes --script-opts=ytdl_hook-ytdl_path=yt-dlp --demuxer-lavf-analyzeduration=2 --ytdl-raw-options=cookies=…,no-playlist=`）。
- 删掉 `--no-config`（libmpv 无此名且默认 `config=no`）、`--vo=gpu-next`、`--gpu-api=vulkan`、`--hwdec=auto-safe`、`--vf=…`（应用自管，透传会顶掉）。
- 单 URL：`base_args() + [url] + mpv_args()`；播放列表：`base_args() + ["--playlist=-"] + mpv_args()`（`--playlist` 必须在 `--` **之前**）。
- `_monitor_mpv` **未改**（`AV:` 与 `[ytdl_hook] ERROR` 靠 `--terminal=yes` 原样出现）。
- ⚠ 改动前的版本没留副本（我的疏漏）：事后按"反向替换 + round-trip 一致性校验"重建 → **`~/.local/bin/mpv-vsr-wrapper.py.orig`**（10233 B = 会话开始时 `ls` 报的原大小；不需要就删掉）。要回滚就 `cp` 回去。
- 部署状态：调研当时本机安装版还是 v0.4.0（wrapper 的**播放列表**路径会因未知选项 `--playlist` 失败，单 URL 路径仍可用）；**v0.4.1 起该能力随正式包发布**，装上即全通（含本机安装）。

## 9. 附录：证据命令与输出

**探针**（`tests/ff2mpv/`：`mpv_option_probe.c` 选项接受度 / `mpv_defaults_probe.c` libmpv 默认值 / `mpv_terminal_probe.c` 状态行通道）

```
gcc -o build/scratch/mpv_option_probe tests/ff2mpv/mpv_option_probe.c \
    -Ithird_party/mpv/include -Lbuild/mpv/_build -l:libmpv.so -Wl,-rpath,"$PWD/build/mpv/_build"
./build/scratch/mpv_option_probe
```

```
force-window                   = yes    -> rc=0    OK
no-config                      = yes    -> rc=-5   option not found      ← CLI 糖，libmpv 无
config                         = no     -> rc=0    OK
vo                             = gpu-next -> rc=0  OK
gpu-api                        = vulkan -> rc=0    OK
hwdec                          = auto-safe -> rc=0 OK
script-opts                    = ytdl_hook-ytdl_path=yt-dlp -> rc=0 OK
demuxer-lavf-analyzeduration   = 2      -> rc=0    OK
ytdl-raw-options               = cookies=/tmp/nope.txt,no-playlist= -> rc=0 OK
input-terminal                 = no     -> rc=0    OK
vf                             = hwup,vsr:…,vfg -> rc=0 OK
playlist                       = -      -> rc=-5   option not found      ← 关键：cplayer 专有
script                         = /tmp/nope.lua -> rc=-5 option not found
```

`./build/scratch/mpv_defaults_probe`（`mpv_initialize()` 之后读属性）

```
ytdl = yes        load-scripts = yes     terminal = no      config = no
idle = yes        keep-open = no         input-terminal = no
save-position-on-quit = no               vo = (空)          hwdec = no
```

`./build/mpv/_build/mpv --no-config --show-profile=libmpv`

```
Profile libmpv:
 config=no
 idle=yes
 terminal=no
 input-terminal=no
 osc=no
 input-default-bindings=no
 input-vo-keyboard=no
 input-media-keys=no
 media-controls=no
 script-opt=select-populate_menu_data=no
```

**应用 CLI 与端到端**

```
$ ./build/src/client/vsr-player --no-config --force-window=yes http://x   → error: unknown option: --no-config
$ ./build/src/client/vsr-player a.mp4 b.mp4                               → error: multiple video files
$ ./build/src/client/vsr-player --playlist=-                              → error: unknown option: --playlist=-
$ ./build/src/client/vsr-player --benchmark -- http://127.0.0.1:8931/catlove_480p.webm
  === Benchmark ===  frames: 3601  time: 7.9s  throughput: 457.9 fps      ← ff2mpv 的 argv 形态，单 URL 可用
$ ./build/src/client/vsr-player -- --ytdl-raw-options=definitely-not-an-option=1 http://127.0.0.1:8931/
  [mpv error] yt-dlp: error: no such option: --definitely-not-an-option  ← 透传直达 yt-dlp argv
$ ./build/src/client/vsr-player http://127.0.0.1:8931/                     ← 直连失败 → ytdl_hook 介入
  [mpv error] ERROR: Unsupported URL: http://127.0.0.1:8931/
  [mpv error] youtube-dl failed: unexpected error occurred
```

**ff2mpv native messaging 端到端（本机实测；配置已还原）**

临时把 `~/.config/ff2mpv-rust.json` 指向 dev 构建（`{"player_command": "<repo>/build/src/client/vsr-player", "player_args": ["--"]}`），按 native messaging 协议直接喂一条消息：

```python
import json, struct, subprocess
msg = {"url": "http://127.0.0.1:8931/catlove_480p.webm", "options": []}
data = json.dumps(msg).encode()
subprocess.run(["/usr/bin/ff2mpv-rust"], input=struct.pack("<I", len(data)) + data,
               capture_output=True)
```

```
ff2mpv-rust rc = 0, reply = b'\x04\x00\x00\x00"ok"'          ← 浏览器侧会收到 ok
$ pgrep -ax vsr-player
76195 /home/zmq/projects/vsr-player/build/src/client/vsr-player -- http://127.0.0.1:8931/catlove_480p.webm
$ grep -E '^(playlist|playlistCurrent)=' ~/.config/vsr-player/vsr-player.conf
playlist=http://127.0.0.1:8931/catlove_480p.webm              ← 确实载入并播放（正常路径写设置）
playlistCurrent=0
```

**播放列表被覆盖**（测试前后 `~/.config/vsr-player/vsr-player.conf`，已恢复）

```
before: playlist=/home/zmq/projects/vsr-player/input/catlove_loop_470s.webm   playlistCurrent=0
after : playlist=http://127.0.0.1:8931/                                      playlistCurrent=-1
```

（该 before/after 来自**非 benchmark** 的一次运行；benchmark 因 `settingsEnabled_=false` 不写设置，见 §3 #12。）

**监控通道：透传 `--terminal=yes` 即可（本机实测）**

`tests/ff2mpv/mpv_terminal_probe.c`（纯 libmpv、`vo=null`、`terminal=yes`、`input-terminal=no`）：

```
$ ./build/scratch/mpv_terminal_probe 2>/dev/null | head -5
● Video  --vid=1               (av1 854x480 30 fps) [default]
AO: [null] 48000Hz stereo 2ch floatp
VO: [null] 854x480 yuv420p
AV: 00:00:00 / 00:05:05 (0%) A-V:  0.000        ← libmpv 也能把状态行写到 stdout
```

真实应用（非 benchmark）：

```
$ ./build/src/client/vsr-player -- --terminal=yes input/catlove_480p.webm > stdout.log 2> stderr.log
$ grep -c '^AV:' stdout.log
44                                              ← 12 s 内 44 行
$ ./build/src/client/vsr-player -- --terminal=yes http://127.0.0.1:8931/ > stdout2.log 2> stderr2.log
$ grep -n 'ytdl_hook\|ERROR' stdout2.log
2:[ytdl_hook] ERROR: Unsupported URL: http://127.0.0.1:8931/     ← 与 wrapper 正则逐字匹配
3:[ytdl_hook] youtube-dl failed: unexpected error occurred
# 同一事件在 stderr 的形态（应用自己的转发）:
61:[mpv error] ERROR: Unsupported URL: http://127.0.0.1:8931/
```

**wrapper 对扩展 profiles 的处理（顺带发现）**

`ff2mpv` 会把扩展 profiles 的文本行作为 options **追加在 `player_args` 之后、URL 之前**；wrapper 的 `main()` 只认 `--no-playlist`，其余 `--xxx` 会走 `elif arg != "--": url = arg` 被当成 URL（因为真正的 URL 在最后，最终 `url` 仍正确，但这些选项被静默丢弃）。要与 mpv 行为一致，wrapper 应把这些未知 `--xxx` 收集起来追加到播放器的 `--` 之后。

**mpv 参考语义**（要镜像的行为）

```
$ printf 'http://127.0.0.1:8931/catlove_480p.webm\n' | mpv --no-config --vo=null --ao=null --frames=1 --playlist=-
Playing: http://127.0.0.1:8931/catlove_480p.webm
$ mpv … --input-terminal=no --length=3 input/goose_480p.webm 2>/dev/null   ← stdout 监控行
AV: 00:00:00 / 00:05:05 (0%) A-V:  0.000
```
