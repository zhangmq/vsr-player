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
