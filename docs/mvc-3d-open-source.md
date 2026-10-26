# 双轨 3D（H.264 MVC / MV-HEVC）开源方案调研（2026-10-26）

> 结论先放前面：**开源解码器已经有了（FFmpeg PR #24303，软件解、未合并）**，
> 但"整盘可用"还缺两块：**mpegts 双 PID 关联**（作者明确说不在该系列）和
> **播放器侧把两视图接进现有管线**。短期零开发可用的只有"离线转 SBS"。

## 1. 我们面对的是哪种"双轨"

| 类型 | 码流 | 容器/封装 | 现状 |
|---|---|---|---|
| **Blu-ray 3D** | H.264 **MVC**（Annex H，profile 118 Multiview / 128 Stereo） | SSIF 交错，视频在**两条 PID**：0x1011 base + 0x1012 dependent；NAL 15（subset SPS）/ NAL 20（coded slice extension） | 本机 `test.iso` 就是这种 |
| MV-HEVC | HEVC Annex G 多视图 | mp4/mkv（Apple 空间视频、部分新片） | **stock FFmpeg 9 已支持**（`view_ids`），见 §4 |
| 双轨 MKV | MVC 两视图各一条 track | mkv（MakeMKV 导出） | 解码同样需要 MVC |
| 帧交替（Sensio 等） | 两个独立码流按时分 | 少见 | 不适用我们现有 SBS/TAB 管线 |

本机 `ffmpeg n9.0.2` 解 MVC 报 `sps_id 1 out of range` —— 这正好对应 PR 补丁 04
的说明（"此前每条 MVC 码流都会刷 sps_id N out of range"）。

## 2. 开源方案盘点

### 2.1 FFmpeg：H.264 MVC 解码（PR #24303，**未合并**）

- 作者 Dom Cobley（popcornmix，Raspberry Pi 内核维护者），2026-08-25 发到
  ffmpeg-devel（18 个补丁），2026-08-28 开 PR 跑 CI：
  [patchwork 封面](https://patchwork.ffmpeg.org/project/ffmpeg/cover/20260825165002.1815001-1-popcornmix@gmail.com/)｜
  [PR #24303](https://code.ffmpeg.org/FFmpeg/FFmpeg/pulls/24303)（抓取时状态 **Open**，19 commits，32 files，+3016/−108）。
- 设计：沿用 hevcdec 的 `view_ids` / `view_ids_available` / `view_pos_available`
  选项接口；per-view 状态（POC、参考列表）放 `H264ViewContext`，视图切换时换入换出；
  改 `receive_frame()`（一个 AU 出两帧）；两视图同 AU 一起输出；inter-view 参考列表
  修改（H.8.2.2.3）；frame/slice 线程；field coding；mp4 `mvcC` box。
- **默认只解 base view**（对现有调用方零影响）。
- 作者自测：43 个真实 MVC/3D 文件、35 GB、两视图端到端；损坏码流 12000 次解码、
  ASan/UBSan 干净；`make fate` 全过。
- **明确写出的缺口**：
  1. **mpegts 两条 PID（0x1011/0x1012）没有关联** → `.ssif` / `.MTS` **仍然只解被
     map 的那一路**；"Associating the PIDs needs demuxer work that is not in this series"。
  2. `cbs_h264` 仍不接受 NAL 15/20（后续系列在写）。
- **硬解被显式拒绝**（补丁 15）：所有 h264 hwaccel 用 `short_ref`/`long_ref` 建参考
  列表并跳过 `reference == 0` 的图，而 inter-view 参考恰好两者都不是 → 会指向加速器
  从未拿到过的 surface，所以只解 base view 并告警。
- 上游反应：Jean-Baptiste Kempf（VLC）问是否基于当年的旧 MVC 解码器，作者答只把
  `britz/mvc_multi_H264Context` 当参考、没用其代码，架构照 MV-HEVC 模板。
- CI：PR 上部分 Fate job 目前 **failing**（aarch64/32-bit/wine/qemu 等），Linux
  Pre-Commit 通过 —— 距离合并还有距离。

### 2.2 popcornmix 的 Kodi / LibreELEC 3D 分支（**目前唯一"整盘能放"的开源栈**）

- [LibreELEC 主题帖（RPi）](https://forum.libreelec.tv/thread/30548-rpi-3d-support-builds/)：
  自述"能跑"的清单包含 **MVC decode**、**Blu-ray ISO playback**、SBS/TAB/帧打包、
  HDMI 3D infoframe、MKV 3D、MV-HEVC（mk-hevc）。
- [x86-64 + AMD 主题帖](https://forum.libreelec.tv/thread/30606-x86-64-3d-support-builds-for-amd-graphics/)：
  同一套 Kodi/ffmpeg 补丁在 x86 上可用，**MVC 是软件解码**（"5800H 足够 1080p"），
  并需要 amdgpu 内核补丁（HDMI 1.4 3D 模式/VSIF/帧打包时序，
  [amd-gfx 系列](https://lore.kernel.org/amd-gfx/20260907125015.69811-1-adrian.betschart@cinemaone.ch/)）。
- Kodi 代码侧的关键点（读 `fix-stereoscopic-3d-gbm-upstream` 分支得到）：
  - `DVDVideoCodecFFmpeg.cpp:395` 对解码器设 `view_ids = -1`；
  - `DVDDemuxFFmpeg.cpp:SupportsMultiviewDecode()` 用"解码器私有选项里有没有
    `view_ids`"来判断能不能解第二视图（**不能**按 ffmpeg 版本判断，H.264 比 HEVC 晚）；
  - 立体信息来自 `AV_PKT_DATA_STEREO3D` 侧数据；MV-HEVC 是 `AV_STEREO3D_UNSPEC` 且
    `view != AV_STEREO3D_VIEW_PACKED` → 标记 multiview，注释写明**解码器输出是并排
    (SBS) 的一帧**；
  - MVC dependent view 的 NAL 20 不被 H.264 parser 当画面起点 → 用 `noparse` +
    `max_packet_size` 8 MiB，靠"一个 PES = 一个 AU"的容器分帧；
  - `DVDInputStreamBluray` 有 `IsStereoscopic()` / `GetStereoscopicClip()`，说明
    3D 盘的 dependent view 是**单独取 SSIF clip** 再合并的（这就是 PR 里说"不在本系列"
    的那部分，Kodi 侧自己做了）。
- 上游计划：popcornmix 说 Kodi 侧等 Piers 分支后提交；ffmpeg MVC 也想上游。
  现状是**纯 fork**，没有官方构建。

### 2.3 离线转换（零播放器开发，今天就能用）

- **BD3D2MK3D**（Windows，AviSynth+ 64-bit + **FRIM** MVC 解码器）→ 输出 SBS/TAB/FS
  MKV；FRIM 是免费但**闭源**的 Intel 参考实现。Linux 需 wine。
  [doom9 支持帖](https://forum.doom9.org/showthread.php?p=2050958)
- **MakeMKV**：只能把 3D 盘 remux 成"两轨 MVC MKV"，**不解码**；
  播放仍要 MVC 解码器。([MakeMKV 论坛](https://forum.makemkv.com/forum/viewtopic.php?p=178791))
- 用打过补丁的 ffmpeg CLI 自己转：`ffmpeg -view_ids ... -i in.mkv -vf stereo3d=sbsl:...`
  之类，本质同 BD3D2MK3D，但要先把补丁合进 ffmpeg。

### 2.4 硬件解码：profile 存在，但**没有开源栈接**

- VA-API 有 `VAProfileH264StereoHigh` / `VAProfileH264MultiviewHigh`；
  NVDEC 有 `CUVIDH264MVCEXT`。
- 但 FFmpeg 的 hwaccel 框架按 `short_ref`/`long_ref` 建参考帧列表，**语义上无法表达
  inter-view 参考**（PR 补丁 15 的论证）。要支持得改每个 hwaccel 的状态传递，并且
  dependent view 的解码依赖 base view 的**重建像素**（必须能回读），不是纯硬件流水线。

### 2.5 其它播放器

- **VLC**：无 MVC（Jean-Baptiste 只是在 ffmpeg-devel 上问过话）。
- **mpv**：无 MVC；但有 `vf_stereo3d`（只吃已打包的 SBS/TAB/交错）。mpv 支持
  `--vd-lavc-o=key=value` 透传解码器私有选项（实测该选项存在），理论上能传
  `view_ids`，不用改 mpv —— 但数组型选项的写法要拿样本验证。
- **Kodi 官方**：还没有；只有 popcornmix 分支。

## 3. 对我们播放器的三条路线

### 路线 A（今天可用，零开发）：离线转 SBS
BD3D2MK3D/FRIM 或 patched-ffmpeg 把 3D 盘转成 SBS MKV，播放器按"3D片源 + 左右并排"
直接播（现有能力）。
- 优点：零风险；缺点：用户要手动转、磁盘翻倍、不是"打开就能看"。

### 路线 B（在播放器里解 MVC）：backport FFmpeg 补丁 + 自己补 demux
1. 把 PR #24303 的 18 个补丁合进 ffmpeg（我们目前**依赖系统 ffmpeg**，需要改成自带
   构建或要求系统 ffmpeg 打补丁 —— 打包体积/维护成本）。
2. 补 **mpegts 双 PID 关联**（PR 没做）：可选做法
   a. 学 Kodi：把 SSIF 的 dependent view 当独立 elementary stream 打开（`noparse` +
      大 `max_packet_size`），在 demux 层把两路 PES 按解码顺序拼成一条 H.264 码流；
   b. 或在 mpegts demuxer 里按 PMT/stream_type 0x20 关联到 base 视频流，按 AU 交错。
3. 播放器侧：若 ffmpeg 输出的是 **SBS 一帧**（Kodi 注释如此说，需我们自测），
   则**现有 3D 管线零改动**——只需在打开时自动切"3D片源 + 左右并排 全"。
4. 风险：解码是纯软件（1080p24 每眼 = 双倍 H.264 负载）；hwaccel 不可用。

### 路线 C（先做，验证链路）：MV-HEVC
stock FFmpeg 9 已有 `view_ids`（本机 `ffmpeg -h decoder=hevc` 可见），**不需要补丁**。
若能确认它输出 SBS 一帧，就把"多视图 → 双眼"的整条链路（解码 → mpv → 我们的合成器）
跑通，作为路线 B 的前置验证；顺带直接支持 Apple 空间视频这类新素材。
- 唯一不确定：mpv 侧怎么把 `view_ids` 传给 lavc（`--vd-lavc-o` 的数组写法）。

## 3.5 本机实测（2026-10-26，全部离线，不涉及上屏）

工具与可复现步骤见 [tools/mvc-prototype/](../tools/mvc-prototype/README.md)。要点：

1. 从 `test.iso` 取出 `BDMV/STREAM/SSIF/00006.ssif` 前 30 MB → 手工拆 TS：确实有两条
   视频流 `0x1011`（base）与 `0x1012`（dependent）。
2. 按 **PTS 配对**（同一 AU 的 base/dep PES 的 PTS 完全相同）把两视图拼成一条 MVC 码流，
   打补丁的 ffmpeg 能接受，`view_ids={0,1}` 解码 189 个 AU 得到 **378 帧**。
3. **输出布局**：每个 AU 出**两帧 1920×1080**，**不是** SBS 打包成一帧
   （Kodi 那句"解码器并排打包"应指 Kodi 自己那层的行为）。
4. **两帧确实是不同视角**：取第 60 个 AU，Y 平面平均 |Δ| = **10.19**，|Δ|>8 占 **27.6%**
   （第 0 个 AU 是纯白帧，Δ=0，不能用来判断）。
5. 解码性能（i5-14600KF）：单线程 **2.9× 实时**、多线程 **4.4× 实时**（7.9 s 素材）。
6. `sps_id 1 out of range` 仍会打印几次但不影响解码。

对实现的意义：**解码这一步已经不是问题**，真正要做的是
① demux 层把两条 PID 按 AU 配对（原型 ~100 行 Python，可移植到 C）；
② 播放器侧把同 PTS 的两帧配成一帧立体（最省事：写一个 CUDA vf 拼成 SBS 3840×1080，
   后面 VSR/VFG/3D 映射全部不动）。

## 4. 建议的下一步（都便宜）

1. **验证 FFmpeg 多视图输出格式**：拿一个 MV-HEVC 样本（或自己造），
   `ffmpeg -view_ids -1 -i x.mkv -f nut -` 看输出帧宽高是否 2×W（SBS 打包）。
   这决定路线 B/C 是否"零播放器改动"。
2. **验证 mpv 透传**：`mpv --vd-lavc-o=view_ids=... file` 的实际写法与效果。
3. **确认 popcornmix 的 ISO MVC 机制**：读他 Kodi 分支里 `DVDInputStreamBluray` 的
   `GetStereoscopicClip` 用法，看 dependent view 是怎么被喂进解码器的 —— 这正是
   PR 缺的那块 demux 工作，可以直接照抄思路。
4. 只有在 1–3 都成立后，再评估路线 B 的工期（补丁维护 + 自带 ffmpeg 的打包）。

## 5. 结论

- **能解**：FFmpeg PR #24303（软件、未合并、作者承诺维护）+ popcornmix 的 Kodi 栈
  （含 ISO，实测在 RPi/x86-AMD 上跑过）。
- **不能直接用**：`stock ffmpeg`（我们现在的依赖）解不了 MVC；`stock mpv/VLC` 也不解。
- **我们的盘**（SSIF 双 PID）是**最难的一档**：即使合了 PR，还得自己补 PID 关联。
- **最省事的可用路径**：短期离线转 SBS；想"打开就 3D"，路线 C（MV-HEVC）先跑通链路，
  再决定是否为 MVC 自带 ffmpeg。
