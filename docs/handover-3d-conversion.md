# 交接文档：3D 蓝光离线转换（2026-10-27）

> **用途**：当前会话上下文已满，新会话从这里接手。
> **一句话**：播放器侧的 3D 功能已完工，**MVC 内建解码已决定不做**；现在唯一在办的是
> **把 `test.iso` 里的 6 个 title 全部离线转出来**（2D → 直接压，3D → 转全宽 SBS），
> 并记录耗时。
> 项目记忆（架构/铁律/历史）在 `AGENTS.md`；MVC 调研结论在 `docs/mvc-3d-open-source.md`。

---

## 0. 立刻要做的第一件事（有阻塞）

**`/tmp` 是 tmpfs（16 GB 内存盘），被我之前的测试填满了**，导致命令执行工具全部报
`ENOSPC`。用户已同意清理。需要先执行（用户在自己终端跑）：

```bash
rm -rf /tmp/*.yuv /tmp/*.raw /tmp/*.mkv /tmp/chunk_stream.264 /tmp/s0000*.bin /tmp/ff9 /tmp/mvcp /tmp/mvc.patch
```

**保留 `/tmp/ffmvc`**：那是打过 MVC 补丁的 FFmpeg 构建树（含 `libavcodec.a`），
`mvc2sbs` 等工具要靠它重编。

空间恢复后**立刻**把它搬到磁盘，之后**所有中间文件只放 `~/.cache/mvc-work/`**：

```bash
mkdir -p ~/.cache/mvc-work
mv /tmp/ffmvc ~/.cache/mvc-work/ffmvc
```

> ⚠ 用户明确要求：**不要再往 `/tmp` 放任何临时文件**（"你没有需要快速缓存的临时文件"）。
> 全部用 `~/.cache/mvc-work/`（磁盘，约 112 GB 空闲）。

---

## 0.1 转换脚本（**已就绪，直接用**）

`~/mvc-tools/convert_all.sh` —— 一条命令转全部 6 个 title：

```bash
~/mvc-tools/convert_all.sh /home/zmq/mvc-out          # 全部
~/mvc-tools/convert_all.sh /home/zmq/mvc-out 5        # 只转 title 5
```

- 自动探测挂载点（`/run/media/zmq/CW3DBD*`，每次挂载后缀会变：`CW3DBD_04` / `_041` …）
- 2D → `ffmpeg -i <clip>.m2ts`（视频重编、音频转 FLAC）
- 3D → `ssif_to_mvc.py`（SSIF→MVC，流式）→ `mvc2sbs`（解码+拼 SBS，**已改流式**）→ `hevc_nvenc`
- title 4 的三段 clip：分别编码后 `concat` 拼接，音频同样拼接
- **计时输出**：`<out>/timings.csv`（title/playlist/kind/时长/clip/耗时/大小/码率）
  + `~/.cache/mvc-work/log_t{N}.log`（分阶段：SSIF→MVC 耗时、解码+编码耗时）
  + `<out>/run.log`（整体进度）
- 中间文件：`~/.cache/mvc-work/<clip>.264`（每个 title 转完即删）

冒烟测试（title 1，10 s）：5 秒完成，输出 3840×1080 Main10 + FLAC，6.7 MB / 5.58 Mbps，
严格解码通过、时长正确。

## 1. 任务规格

### 输入
- 盘：`/home/zmq/视频/test.iso`（20.6 GB，Blu-ray 3D，**无 AACS**，卷标 `CW3DBD_04`）
- 挂载（不要用 `7z` 抽 m2ts，它报 Unsupported Method；SSIF 可以但慢）：
  ```bash
  udisksctl loop-setup -f "/home/zmq/视频/test.iso"     # → /dev/loopN
  udisksctl mount -b /dev/loopN                          # → /run/media/zmq/CW3DBD_04
  ```

### title 结构（libbluray 实测，`/tmp/bd3dinfo` 可重编）

| title | playlist | 时长 | clip(s) | 视频 | 有 SSIF | 处理方式 |
|---|---|---|---|---|---|---|
| 0 | 00004 | 0:00 | 00008 | **MPEG-2** 1920×1080 47.95fps，无音轨 | 无 | 2D 直接压 |
| 1 | 00002 | 0:10 | 00003 | H.264 1080p 23.976 + PCM 2ch | **有** | **3D → SBS** |
| 2 | 00000 | 0:30 | 00000 | H.264 1080p 29.97，**无音轨** | 无 | 2D 直接压 |
| 3 | 00001 | 1:30 | 00002 | **MPEG-2** 1080p 24 + PCM 2ch | 无 | 2D 直接压 |
| 4 | 00005 | 4:00 | 00011 + 00013 + 00009 | H.264 1080p 23.976 + PCM，**3 段** | **三段都有** | **3D → SBS，三段拼接** |
| 5 | 00003 | **1:18:30** | 00006 | H.264 1080p 23.976 + PCM 2ch | **有** | **3D → SBS（主片）** |

### 编码参数（用户已定，**不要再调画质**）

用户原话："画质就不再调了，17% 体积差别不大，大一点就大一点" → 用 **CQ21**（不是 CQ23）。

HEVC NVENC，10-bit，与已给用户看过的样片完全同一套：

```
-c:v hevc_nvenc -preset p7 -tune hq -profile:v main10 -pix_fmt p010le \
-rc vbr -b:v 0 -cq 21 \
-spatial-aq 1 -temporal-aq 1 -aq-strength 8 -rc-lookahead 32 -multipass fullres -b_ref_mode middle \
-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv
```

### 音频 / 字幕
- 音频：源是 **PCM（pcm_bluray）2ch** → 转 **FLAC**（无损、体积小）；`-c:a flac`。
  title 0 / 2 无音轨，跳过。
- 字幕：**跳过**。3D 盘 PG 字幕带 depth（我们不支持），2D 的也没要求；不要擅自加。

### 输出
- 目录：`~/.cache/mvc-work/out/`（用户未指定位置，交付前可再确认）
- 命名：`t{tid}_{playlist}_{2d|3dsbs}.mkv`，例如 `t5_00003_3dsbs.mkv`、`t3_00001_2d.mkv`
- **必须记录**：每片 开始时间 / 结束时间 / 耗时 / 输出大小 / 实测码率 → 最后给汇总表

### 预期
- 主片：≈ **26–30 分钟**，≈ **12 GB**（20.4 Mbps）
- 其余 5 个 title（合计约 6 分钟素材）：几分钟

---

## 2. 工具与数据位置

| 路径 | 内容 | 状态 |
|---|---|---|
| `~/mvc-tools/ssif_to_mvc.py` | **流式** SSIF → MVC 码流（内存有界） | 已写，800 MB 片段上跑通（1.3 s） |
| `~/mvc-tools/ssif_split_merge.py` | 旧版，**整文件读入内存**（19 GB 会 OOM） | 只用于小片段 |
| `~/mvc-tools/mvc2sbs.c` | MVC → SBS raw 的 C 工具（`view_ids={0,1}` 解码 + 并排） | **需改成流式 I/O**（现在是 `readall` 整文件进内存） |
| `~/mvc-tools/mvc_probe.c` | 解码探针（导出帧、打尺寸） | 可用 |
| `~/mvc-tools/mvc_to_sbs.sh` | 端到端脚本（ISO/merged.264 → SBS MKV） | 需按新流程更新 |
| `~/mvc-eval/30s_hevc_cq21_10bit_tuned.mkv` | 给用户看过画质的 30 s 样片（20.4 Mbps） | 保留 |
| `~/mvc-eval/frames/*.png` | 判定"是否真交错"用的无损帧 | 保留 |
| `~/mvc-eval/旧_色度有bug/` | 早期色度 bug 版本样片 | 作废，勿用于评估 |
| `/tmp/ffmvc` → 待搬到 `~/.cache/mvc-work/ffmvc` | 补丁 FFmpeg 构建树（libavcodec.a 等） | **必须保留** |

### 编译 `mvc2sbs`（在迁移后的构建树里）

```bash
cd ~/.cache/mvc-work/ffmvc
gcc -O2 -o ~/mvc-tools/mvc2sbs ~/mvc-tools/mvc2sbs.c -I. \
    libavcodec/libavcodec.a libavutil/libavutil.a libswresample/libswresample.a \
    -lm -lpthread -lz -lx265 -lx264 -lstdc++
```

---

## 2.5 长任务必须用工具的后台作业（**踩过**）

`nohup ... &` 起的转换进程在父 shell 退出时**被整组杀掉**（实测：主片跑到 2:23 时进程消失，
日志无任何错误、`pgrep` 空、`run.log` 停更）。长任务一律用 agent 工具的
**后台作业**（`run_in_background: true`），不要用 `nohup &`。

进程被杀的后果与补救：
- 中间文件保留 → 不需要重跑前置阶段。主片这次已生成 `~/.cache/mvc-work/00006.264`
  （**17 GB**：base 13.1 GB + dep 5.8 GB 的纯 payload，比 SSIF 略小是正常的），
  直接复用即可，省掉 58 s 的 SSIF→MVC；
- 脚本：`~/mvc-tools/finish_t5.sh`（复用已合并的 00006.264，只做解码+编码+合音轨）。

## 2.6 内存：`mvc2sbs` 的帧缓冲泄漏（**已修，别再引入**）

现象：主片转换跑到一半整机内存被吃光，`mvc2sbs` RSS 涨到 **17.6 GB**，被 OOM killer `SIGKILL`
（用户看到"内核总报内存不足"）。日志无任何错误、进程凭空消失。

根因：配对成功写出 SBS 帧后**只 `av_frame_unref(frm)`，忘了 `av_frame_unref(base)`** →
每个 AU 泄漏一个 1920×1080 帧缓冲（≈3 MB），5600 个 AU ≈ 17 GB。

修法（`~/mvc-tools/mvc2sbs.c`）：写完一对之后 `av_frame_unref(base);`。
修复后实测 25 秒峰值 RSS **241 MB**（平稳），之前 20 秒就能涨到 GB 级。

⚠ 顺带记住：**`/tmp` 是 tmpfs（内存盘）**，写大文件等于吃内存（之前往 `/tmp` 写 3 GB 的
Raw YUV 就是内存压力的来源之一）。中间文件一律 `~/.cache/mvc-work/`。

## 3. 已知坑（都踩过，别再踩）

1. **`/tmp` 是 tmpfs**：19 GB 的中间文件会吃光内存盘并让工具链全部 ENOSPC。只用 `~/.cache/mvc-work/`。
2. **色度打包（已修）**：SBS 拼帧时色度一行只有 `W2/2 = W` 字节，且 **U、V 是两个独立平面**
   （先写完所有 U 行再写 V 行）。早期版本按亮度行宽步进、把 U/V 塞同一行 → 颜色错乱 +
   看起来像"横向条纹/隔行"。**用户看到的那种条纹就是这个 bug**（已用无损帧确认片源是逐行）。
3. **配对必须按 PTS**：同一 AU 的 base/dependent PES 的 PTS 完全相同。用"每两帧配一对"
   在某 AU 只解出一帧时整体错位（实测 PSNR 从 48 dB 掉到 18 dB）。
4. **AV1 NVENC 不能用于归档**：FFmpeg [#11390](https://trac.ffmpeg.org/ticket/11390)
   输出偶发非法码流（OBU padding bug），NVIDIA 承认驱动侧问题，610 驱动仍未修。
   **用 HEVC NVENC**。H.264 NVENC 效率最低、10-bit High10 太新，也不用。
5. **质量度量必须等帧数**：`ref.yuv`(480 帧) vs `enc.mkv`(120 帧) 时 ffmpeg 会用末帧填充，
   得出 18–30 dB 的**假数据**。用 numpy 逐帧算，或两边都 trim 到同一帧数。
6. **不要看图片**：用户明确"没有我允许，你不要主动看图"。数值统计可以。
7. **libbluray 不提供 SSIF**：`BLURAY_PLAYER_SETTING_3D_CAP` 设 0/1/3/7/255 都只返回
   base view 的 PID 0x1011。必须自己读 `BDMV/STREAM/SSIF/<clip>.ssif`。
8. `00007.m2ts` 疑似"右眼独立流"（大小恰好 = SSIF − base），未验证；不影响当前任务。
9. MPEG-2 的两个 title（0、3）走 nvdec 硬解没问题；编码同样用 HEVC NVENC。

---

## 4. 已验证的数据（可直接引用，不必重测）

| 项目 | 数值 |
|---|---|
| 4K HEVC 10-bit 软解（Fallout 2160p） | 176 fps（我们）/ 208 fps（ffmpeg），≈7–8× 实时 |
| MVC 双视图软解 | 3.7–4.4× 实时（1080p24） |
| HEVC NVENC CQ21 tuned（30 s 样片） | 20.4 Mbps、PSNR-Y 41.0、VMAF 99.37、**73 fps（3.0× 实时）** |
| 同内容 CQ23 | 17.0 Mbps、VMAF 99.31（用户放弃，选 CQ21） |
| 8-bit vs 10-bit 同码率 | 10-bit PSNR 41.0 / VMAF 99.37；8-bit PSNR 39.2 / VMAF 99.61（10-bit 保细节，banding 更少） |
| 原盘 SBS 等效码率 | ≈32 Mbps（13.12 GB base + 5.84 GB dependent / 78.5 min） |

---

## 5. 播放器侧现状（新会话别重复劳动）

- 分支：`feat/stereo-osd-prototype`，最新提交 `650135e`
- **已完成**：3D 三态菜单/选择器、3D 快捷键面板（H/J/K/L）、音量/进度 OSD、
  3D 下 Tab 切管线状态 OSD、按每眼算的 VSR auto 倍率、分屏整数化、全屏切屏修复、
  清空列表置黑、重启后播放修复、ISO 当容器（mpv 补丁）
- **已决定不做**：内建 MVC 解码（真 3D 蓝光）；见 `AGENTS.md` 同名小节
- **已记录待做**：单眼 2D 输出（主窗口按单眼满屏 + 按单眼画幅超分，推荐 `vf_eye`，
  约半天；`--video-crop` 是 VO 级裁切，**不能用**）
- 纪律：看 `AGENTS.md`（日志英文、UI 走 i18n、不自行发挥 UI、打包版本表等）

---

## 6. 接手检查清单

- [ ] `df -h /tmp ~`：确认 /tmp 已清理、~ 有 ≥40 GB
- [ ] `/tmp/ffmvc` 已搬到 `~/.cache/mvc-work/ffmvc`
- [ ] 挂载 ISO，`ffprobe` 抽查 clip 00006（1920×1080 / 23.976 / PCM 2ch）
- [ ] 把 `mvc2sbs` 改成流式读写并重编
- [ ] **先跑 title 1（10 秒）**验证整条链路与耗时，再跑主片
- [ ] 每个输出都做：`ffprobe` 尺寸/帧率/时长 + `ffmpeg -v error -i out -f null -` 严格解码
- [ ] 汇总表：title / 类型 / 时长 / 耗时 / 大小 / 码率
- [ ] 完成后把结果与耗时一并汇报给用户

---

## 7. 需要用户确认的点（开始前问一句）

1. 输出放哪里（默认 `~/.cache/mvc-work/out/`，还是直接放 `~/视频/`？）
2. 0:00 的 title 0（MPEG-2、无音轨、时长 0）要不要留（默认：转，但可以跳过）
3. 音频转 FLAC 是否可接受（源是 PCM 2ch；FLAC 无损、约省 40%）

---

## 8. 已完成（2026-10-27 01:31）——结果与耗时记录

全部 6 个 title 已转到 `/home/zmq/mvc-out/`（全部通过 ffprobe + 严格解码校验）：

| title | playlist | 处理 | 时长 | 输出 | 码率 | 耗时 |
|---|---|---|---|---|---|---|
| 0 | 00004 | 2D 直接压（单帧） | 0.04 s | 49 KB | — | 0 s |
| 1 | 00002 | 3D → SBS | 10 s | 6.7 MB | 5.58 Mbps | 5 s |
| 2 | 00000 | 2D 直接压 | 30 s | 6.4 MB | 1.70 Mbps | 8 s |
| 3 | 00001 | 2D 直接压（MPEG-2 源） | 1:30 | 117 MB | 10.31 Mbps | 14 s |
| 4 | 00005 | 3D → SBS（3 段拼接） | 4:00 | 336 MB | 11.20 Mbps | 81 s |
| 5 | 00003 | 3D → SBS（**主片**） | 1:18:30 | **12.26 GB** | 20.82 Mbps | **1518 s（25.3 min）** |

- 合计：**84.7 分钟素材 → 12.73 GB，总耗时 1626 s ≈ 27 分钟**
  （主片另有一次 58 s 的 SSIF→MVC 是在被杀的那次跑里完成的，从零算 ≈ 28 分钟）
- 压缩比：12.73 GB / ISO 20.6 GB = **0.62**（3D 流 18.96 GB 的 0.67）
- 主片实测：编码 76 fps = **3.18× 实时**（解码+拼帧+编码流水线整体）；
  112939 AU → 112923 SBS 帧；输出 3840×1080 Main10 + FLAC

产物清单（`~/mvc-out/`）：`t0_00004_2d.mkv`、`t1_00002_3dsbs.mkv`、`t2_00000_2d.mkv`、
`t3_00001_2d.mkv`、`t4_00005_3dsbs.mkv`、`t5_00003_3dsbs.mkv`、`timings.csv`、`run.log`

播放：2D 的按普通视频放；SBS 的用播放器「3D片源 + 左右并排 + 全」。
