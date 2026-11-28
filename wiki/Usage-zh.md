# 使用

> [English](Usage) · 中文

## 播放

```bash
vsr-player video.mkv                  # one file
vsr-player ~/Videos                   # directory → whole playlist (mpv expands it)
vsr-player a.mkv b.mkv                # positional args are playlist entries, like mpv
vsr-player --playlist list.m3u        # playlist file (`-` reads from stdin)
vsr-player                            # restore the last playlist, load nothing
```

**加载目录用的是 mpv 自己的展开逻辑**（`--directory-mode`，默认 `lazy`）：顶层的媒体文件成为条目，子目录
保留为条目，点击时才展开。

**无参数启动保持 idle。** 播放列表会被恢复（含名字），但不载入任何媒体 —— 不解码、不做 VSR/FRUC 引擎运算、
不占用显存。按下播放后载入当前条目，并经 mpv 的 watch-later 机制从上次保存的位置继续。在命令行传入文件 ——
或显式 `-- --pause=yes` —— 则立即载入，遵循 mpv 语义。

## 界面控件

界面会自动隐藏；指针进入底部悬停区（进度条及其下方的一段边距）时出现，离开后再隐藏。

- **底栏** —— 进度/跳转、播放/暂停、上一个/下一个、停止、音量、画质（缩放、VSR 画质、降噪、插帧、
  SDR→HDR10）、硬件/软件解码切换、倍速、宽高比、循环模式、全屏、播放列表。
- **进度条悬停** —— 指针上方会显示该位置的缩略图，时间位于其底部的黑条上；片源有章节时，章节名显示在顶部的
  半透明条上。本地文件与网络流都支持：播放器复用已经解析好的地址和请求头，所以悬停不会二次提取（联网缩略图在
  首次悬停时抓取）。无法生成缩略图时，气泡回退为时间加「缩略图不可用」，下次悬停会在短暂冷却后重试。面板宽
  192 逻辑像素，按显示缩放渲染，因此在 HiDPI 屏幕上依然清晰。可在菜单中关闭（**Interface → Thumbnails**）；
  关闭后悬停只显示时间。
- **右键菜单**（`Menu`、`Shift+F10` 或 `F10`）—— 按以下顺序分组：**File**（打开文件 / 文件夹 / URL，
  删除当前文件）、**Playlist**（添加文件 / 文件夹，移除当前条目）、**Preferences → Interface**（缩略图、
  隐藏不可用菜单项、显示 3D 菜单区块）、**Tracks**（视频轨道与网络流、音频、字幕（含加载外部文件）、字幕
  延迟与音频延迟）、**Picture processing**（反交错、旋转）、**3D**、**About**、**Quit**。3D 区块在
  [3D 与单眼](3D-and-Single-Eye) 中说明；**About** 显示图标、程序名、版本、许可证以及可点击的仓库链接。
  键盘：`↑`/`↓` 移动（跳过分隔符与分组标题，循环），`Home`/`End` 跳到首/末项，`→` 展开二级菜单，`←` 收起，
  `Enter` 激活，`Esc` 逐级返回（延迟控件 → 二级菜单 → 菜单）。菜单高度超过窗口时会滚动（▲/▼ 三角、
  鼠标滚轮，或键盘导航并保持当前行可见），而不是被裁掉。
- **播放列表面板**（`P`）—— 打开时当前条目居中；把文件拖到面板上可追加，不会打断播放。
- **OSD**（`Tab`，2D 下也可用 `h`）—— `Source`（分辨率/编码/帧率/帧数，加上内容侧的 HDR 状态）、
  `Output`（视口、渲染帧率以及**实际**输出模式：`HDR10 · BT.2020/PQ · display 380nit`）、`VSR`、
  `TrueHDR`、`FRUC`、解码器、倍速、GPU。三个 VFX 行读作 `<setting> | <what the filter reports>`：阶段在跑时是
  `mode=active cost=…`（VSR 还会报告实际生效的缩放倍率与帧率；插帧报告源帧率与输出帧率）；阶段被有意闲置时
  是 `mode=passthrough reason=…`（例如 `reason=max-fps`）；阶段关闭时只显示 `off`。
- **宽高比**（`A` / `Shift+A`，或底栏弹出面板）—— 循环 mpv 自带的那份列表：`16:9` → `4:3` → `2.35:1` →
  `no`。片源的显示宽高比错误或缺失时用它；3D 模式下该值按**单眼**宽高比解释。

## 键盘快捷键

| 按键 | 操作 |
|-----|--------|
| `Space` | 播放 / 暂停 |
| `←` / `→` | 快退 / 快进 5 s（`Shift` + 方向键 = ±10 s） |
| `↑` / `↓` | 音量 ±5 % |
| `Ctrl+←` / `Ctrl+→` | 上一个 / 下一个播放列表条目 |
| `,` / `.` | 后退 / 前进一帧 |
| `A` / `Shift+A` | 向前 / 向后循环切换宽高比 |
| `V` | 字幕开 / 关 |
| `z` / `Shift+Z` | 字幕延迟 −50 / +50 ms |
| `Ctrl+Alt+Z` | 重置字幕延迟（`Ctrl+0` 现在是超分的「auto」） |
| `Ctrl++` / `Ctrl+-` | 音频延迟 +50 / −50 ms |
| `Ctrl+Alt+0` | 重置音频延迟 |
| `S` | 截图 |
| `Tab`（2D 下也可用 `h`） | 切换 OSD 叠加层 |
| `P` | 切换播放列表面板 |
| `F` | 切换全屏 |
| `[` / `]` / `\` | 倍速 0.5× / 2× / 1× |
| `Delete` / `Shift+Delete` | 从播放列表移除当前条目 / 把文件移到回收站 |
| `Ctrl+O` / `Ctrl+Shift+O` / `Ctrl+L` | 打开文件 / 文件夹 / URL |
| `Menu` / `Shift+F10` / `F10` | 打开右键菜单 |
| `Ctrl+1/2/3/4` / `Ctrl+0` | 超分：关 / 2× / 3× / 4× / auto |
| `Ctrl+Shift+1/2/3/4` / `Ctrl+Shift+0` | 插帧：关 / 2× / 3× / 4× / 循环输出帧率上限（48→60→120） |
| `Ctrl+H` | SDR→HDR10 上变换开 / 关 |
| `Ctrl+Q` | 退出 |
| `Esc` | 退出全屏 / 关闭播放列表（不会停止播放） |
| `H` | 切换热键帮助叠加层（3D 演示） |
| `J` / `K` / `L` | 3D 或单眼模式：布局 · 眼序（或选择眼）· full/half |

右键菜单打开时，方向键、`Home`/`End`、`Enter` 与 `Esc` 用于操作菜单而不是播放；字幕/音频延迟的按键仍然
有效，因此可以一边看效果一边调延迟。3D 会话中眼镜窗口上有意忽略 `Esc`（全屏会把窗口移回获得焦点的桌面）；
请从菜单退出 3D。

## 命令行选项

| 选项 | 取值 | 默认 | 说明 |
|--------|--------|---------|-------------|
| `--scale` | `off`、`auto`、`2`、`3`、`4` | `auto` | 超分倍率 |
| `--quality` | `low`、`medium`、`high`、`ultra` | `high` | 超分画质 |
| `--denoise` | `off`、`low`、`medium`、`high`、`ultra` | `off` | 降噪画质（在 scale=1 时生效） |
| `--fruc` | `off`、`2`、`3`、`4`（默认 VFG 后端）；`off`、`40`、`48`、`60`、`2`、`3`、`4`（RIFE 源码构建） | 持久化值 | 插帧：整数倍率（VFG）或目标帧率 40/48/60（RIFE） |
| `--fruc-quality` | `low`、`medium`、`high` | `medium` | VFG 模型画质（可热切换；RIFE 忽略） |
| `--fruc-order` | `pre`、`post` | `post` | VFG 在链上的位置：`post` = 在 VSR 之后，`pre` = 在 VSR 之前（RIFE 固定） |
| `--fruc-max-fps` | `48`、`60`、`120` | `60` | 输出帧率上限：源帧率 × 倍率高于该值时直通（RIFE 忽略） |
| `--no-hwaccel` | — | — | 禁用 NVDEC，改用软件解码 |
| `--lang` | 例如 `en`、`zh_CN` | 系统 locale | 界面语言 |
| `--benchmark` | — | — | 无界面吞吐量测量（无 UI，`all=no` 级别日志） |
| `--vsync` | — | 关闭 | FIFO 呈现（默认非阻塞；Wayland 没有撕裂） |
| `--no-rpc` | — | — | 禁用 JSON IPC 服务器 |
| `--playlist` | `<file>`、`-` | — | 播放列表文件（m3u/纯文本；跳过空行与 `#` 注释） |
| `--` | — | — | 其后的所有内容作为选项传给 mpv |

由应用自己管理的选项（`vo`、`vf`、`hwdec`、`msg-level`）不得写在 `--` 之后。

## JSON IPC

Unix socket 位于 `/tmp/vsr-player.sock`，每行一个 JSON 对象：

```bash
printf '{"command":["play"]}\n' | socat - UNIX-CONNECT:/tmp/vsr-player.sock
```

命令按使用场景分组：

- **播放** —— `play`、`pause`、`stop`、`seek`、`open <path>`、`fullscreen`、
  `resize <w> <h>`、`playlist-play`、`clear-playlist`、`remove-current`、`delete-current`、
  `quit`。
- **处理** —— `set-vsr` / `get-vsr`、`set-fruc`、`set-fruc-quality`、
  `set-fruc-max-fps`、`set-deinterlace` / `get-deinterlace`、`set-rotate` / `get-rotate`，
  以及 `vf-command <filter> <cmd> [arg]`，用于在运行期调整某个滤镜。
- **查询** —— `get-playlist`（mpv 侧的 `title`/`filename` 加上面板显示的条目名）、
  `get-video-size`（滤镜之后的帧尺寸）、`get-source-format`、`get-media-phase`、
  `get-chapters`、`get-stereo`、`get-eye`、`get-screens`、`get-menu-state`、
  `get-control-state`、`get-focus`、`get-keys`、`grab-window <path>`（把播放器窗口写成 PNG）。
- **缩略图** —— `preview <out.png> <seconds>`（渲染一张悬停缩略图并等待完成）、
  `get-preview-state`、`get-preview-width`、`set-preview-width`、
  `set-preview-thumbnails`。
- **UI 测试钩子** —— `toggle-osd`、`open-menu`、`menu-key`、`key`、`key-log`、
  `clear-key-log`、`debug-hover`、`warp-pointer`、`nudge-volume`、`set-menu-hide-invalid`。
- **通用** —— `get_property` / `set_property`，以及 `command` 用于执行原始 mpv 命令
  （`{"command":["command","loadfile","<path>","replace"]}`）。

## 独立的 `mpv-vsr` CLI

打过补丁的 mpv 二进制（含 `vf_hwup` + `vf_vsr` + `vf_truehdr` + `vf_vfg`，RIFE 构建还含 `vf_rife`）
以 `mpv-vsr` 的名字分发，可直接当作普通 mpv 使用：

```bash
mpv-vsr --hwdec=auto --vf=hwup,vsr:scale=2,truehdr:enable=yes:luminance=650,vfg:fps=2:mode=medium video.mkv
```

链顺序（从左到右）：`hwup`（软件帧上传到 CUDA，启用软解路径）→ `vsr`（超分）→ `truehdr`
（SDR→HDR10，默认关）→ `vfg`（插帧）。要 `pre` 顺序就把 `vfg` 放在 `vsr` 前；RIFE 构建使用
`hwup` → `rife` → `vsr` → `truehdr`。硬件解码请用 `--hwdec=nvdec`（此时 `hwup` 是无操作的直通）。

| 选项 | 取值 | 说明 |
|--------|--------|-------------|
| `scale`（vsr） | `off`、`auto`、`2`、`3`、`4`、比例（如 `4/3`） | 超分倍率；`auto` 按视口选择 |
| `quality`（vsr） | `low`、`medium`、`high`、`ultra` | VSR 推理画质 |
| `denoise`（vsr） | `off`、`low`、`medium`、`high`、`ultra` | 降噪处理（在 scale=1 时也可用） |
| `stereo-layout` / `stereo-half`（vsr） | `-1|0|1`、`0|1` | 3D 片源的单眼缩放（见 [3D 与单眼](3D-and-Single-Eye)） |
| `enable`（truehdr） | `yes`、`no` | SDR→HDR10 上变换（HDR 帧直通） |
| `luminance`（truehdr） | 400…2000 | 目标 HDR 峰值亮度，单位 nit（默认 650；超出该范围时 SDK 会静默忽略） |
| `saturation` / `contrast`（truehdr） | 0…200 | 默认 100 |
| `middle-gray`（truehdr） | 10…100 | 默认 50 |
| `debanding-off`（truehdr） | `0`、`1` | `1` 跳过抗色带处理（仅供诊断；默认 `0` = 开启） |
| `fps`（vfg） | `off`、`2`、`3`、`4` | 插帧倍率 |
| `mode`（vfg） | `low`、`medium`、`high` | VFG 模型画质（默认 `medium`） |
| `mode` / `layout` / `half`（eye） | `off|left|right`、`sbs|tab`、`0|1` | 单眼提取（见 [3D 与单眼](3D-and-Single-Eye)） |
| `fps`（rife） | `off`、`auto`、1..120 的整数 | 插帧目标帧率（仅 RIFE 构建）；`auto` = 源帧率的 2× |
| `scale`（rife） | `off`、`2`、`3`、`4` | 基准测试倍率（用于测设备上限；仅 RIFE 构建） |

除 RIFE 选项外，其余都可以在运行期通过 `vf-command` 修改：

```bash
printf '{"command":["command","vf-command","truehdr","luminance","1000"]}\n' | socat - UNIX-CONNECT:/tmp/vsr-player.sock
```

`mpv-vsr-wrapper.py`（浏览器集成，ff2mpv 风格）导出 Chrome cookies，通过 yt-dlp 提取播放列表
（YouTube/Bilibili/Niconico）并启动 `mpv-vsr`。属个人使用工具，不随发布压缩包分发。
