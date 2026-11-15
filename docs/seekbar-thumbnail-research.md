# 进度条缩略图（seekbar thumbnail）调研与设计

> 状态：**已实现**（2026-11-15，分支 `feat/seekbar-thumbnails`）。前两节是调研结论，
> §11 记录实现落地情况与偏离点。
> 来源 issue：<https://github.com/zhangmq/vsr-player/issues/57>
> 关联实测：本机 2026-11-09 用 `build/mpv/_build/mpv`（v0.41.0-dev-a3d4b59，
> 与发布包同源）在 `/tmp/thumb_bench.py` 里跑过取帧延迟与拼版生成。

## 1. 已拍板事项（用户 2026-11-15）

| 项 | 决定 |
|---|---|
| 形态 | **悬浮单帧** + 时间 + 章节名；另需显示**指针在进度条悬停位置的时间** |
| 拖动时整条缩略图带（filmstrip） | **不做** |
| 后台 storyboard 预生成（磁盘缓存） | **不做** |
| 引擎 | **内置解码器实时解**（= 进程内第二个 libmpv 实例）；子进程方案只作降级备选 |
| 缩略图尺寸 | **可配置**，默认宽 320；**暂不开放 UI 修改**（走配置键） |
| HDR 内容 | **强制 tonemap 到 SDR** 再显示 |
| 3D 内容 | 按当前 `layout`/`half` **裁单眼**；预览只在桌面 control 窗口显示 |

## 2. mpv 有没有现成支持（核查结论）

| 能力 | 结论 | 证据 |
|---|---|---|
| `--vf=thumbnail` 之类的内建缩略图滤镜 | **没有** | 上游 v0.33–v0.41 均无此文件（raw 404）；`mpv --vf=help` 无 thumbnail |
| OSC 的缩略图 UI | 只有**接口**，不含生成 | [DOCS/man/osc.rst:765](../third_party/mpv/DOCS/man/osc.rst)：OSC 只写 `user-data/osc/draw-preview`，画面由外部 thumbnailer 画 |
| 单帧导出（encode 模式） | 有 | `--ovc=rawvideo --of=image2 --ofopts=update=1 --o=x.bgra`；thumbfast 整套方案基于它 |
| 进程内取帧 | 有两条：`screenshot-raw`（需 VO）与 `MPV_RENDER_API_TYPE_SW` | [render.h:128](../third_party/mpv/include/mpv/render.h)、`video/out/libmpv_sw.c`（已编入本构建，[meson.build:247](../third_party/mpv/meson.build)） |
| 拼版（storyboard） | 有（lavfi `tile`） | 本次不用，仅记录可行性：233 s/1080p 出 8×8 拼版约 1.1 s |

**结论**：mpv 没有“进度条缩略图”这个成品功能，但取帧所需的原语齐全；“内置解码器实时解”
在我们这套技术栈里就等于**进程内第二个 libmpv 实例**（Haruna 就是这么做的：
`src/mpv/mpvpreview.cpp`，第二个 mpv + `vo=libmpv` + 暂停/静音/无音轨字幕 + `osd-level=0`）。

## 3. 引擎选型

| 方案 | 做法 | 评价 |
|---|---|---|
| **A. 进程内第二 libmpv + SW render**（选定） | 独立 `mpv_handle`，`vo=libmpv`，`MPV_RENDER_API_TYPE_SW` 渲进 CPU buffer → `QImage` | 无新进程、无 IPC/文件轮询、无 GPU 上下文；延迟 = 纯解码时间。mpv 文档提示它“不适合抽帧、HDR 要手工处理”，我们只出 320×180 单帧，HDR 本来就要插 tonemap 链，影响可控 |
| B. 进程内第二 libmpv + Vulkan render + readback | 用主实例的 VkDevice 建离屏 image，`vkCmdCopyImageToBuffer` | 能拿到 mpv 完整色彩管线（含 tonemap），但要第二套 render context + 同步，和主渲染路径耦合，320×180 不值当 |
| C. 子进程 `mpv-vsr`（thumbfast 配方） | `--idle --pause` + IPC `seek` + 轮询 BGRA 文件 | 实测成熟（热路径 10–85 ms），但多一个进程、IPC + 文件轮询、生命周期管理；**保留为降级备选**（若 SW render 有硬伤） |
| D. 直接用 FFmpeg 库自己解码 | — | 属于自行实现解码器，且要处理硬解/色彩/旋转，违反“尽量不自行实现” |

实测数据（`--hwdec=no`、宽 320、软件解码；子进程口径，进程内解码耗时相同、省掉启动与轮询）：

| 素材 | 冷启动每条 | 常驻后 exact seek | keyframe seek |
|---|---|---|---|
| 1080p h264 / 233 s | 122–163 ms | 10–85 ms（中位 ~25） | 12–21 ms |
| 4K HEVC / 20 s | 445–876 ms | 60–760 ms | 8–60 ms |

连续悬浮（30 次请求 / 20 ms 间隔）：中位 49 ms、p90 117 ms、最大 169 ms。
该片关键帧间隔中位 1.54 s、最大 5.34 s —— 这是“先出关键帧图、再精修”的依据。

## 4. 架构

```
ProgressSlider hover ──(sec)──► SeekPreview (QML，uiLayer 顶层 Item)
                                   │ 位置/时间/章节名/缩略图显示
                                   ▼
                        PreviewThumbnailer (C++)
                          ├─ 节流：同时只 1 个 seek 在途，新请求只记最新目标
                          ├─ 两级：keyframe seek 先回填 → 80 ms 静默后 exact 精修
                          ├─ 缓存：QHash<bucket, QImage> LRU 64，bucket = 2 s
                          └─ 自有 mpv_handle（事件线程）+ SW render context（GUI 线程）
```

### 4.1 预览实例的 mpv 参数

`vo=libmpv`、`config=no`、`load-scripts=no`、`osc=no`、`input-default-bindings=no`、
`audio=no`、`sub=no`、`pause=yes`、`osd-level=0`、`ytdl=no`、
`demuxer-readahead-secs=0`、`demuxer-max-bytes=128KiB`、
`vd-lavc-skiploopfilter=all`、`vd-lavc-fast`、`vd-lavc-threads=2`、
`hwdec=no`（首版；4K 精修慢再评估 `nvdec`）、`sws-scaler=fast-bilinear`。

`vf` 链（顺序即语义）：

1. **3D 裁单眼**（仅 3D 且 `half=false`）：SBS → `crop=iw/2:ih:0:0`；TAB → `crop=iw:ih/2:0:0`；
   `half=true` 时源本身已是单眼宽高，不裁。
2. **缩放**：`scale=w=<previewWidth>:h=-2`（保持单眼 aspect，SW_SIZE 与它一致 → 无黑边）。
3. **HDR tonemap**（仅内容 HDR）：`lavfi=[zscale=t=linear:npl=100,format=gbrpf32le,
   zscale=p=bt709,tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv]`（具体参数实现时定）。
   先缩小再 tonemap，CPU 成本按 320×180 计。

旋转：传 `--video-rotate=<菜单角度>`（文件元数据自带旋转由 mpv 叠加，语义与主播放器一致）。

### 4.2 线程与调用纪律（照 AGENTS.md）

- GUI 线程**不调用**任何同步 mpv API（`mpv_get_property`/`mpv_set_property`/`mpv_command`）；
  命令一律 `*_async`，属性走观察器（回调在事件线程）。
- 事件循环独占一个 `std::thread`（`mpv_wait_event` 每个 handle 只能一个线程调）。
- `mpv_render_context_update()` / `render()` 只在 GUI 线程调（与主播放器的约定一致），
  SW 渲染 320×180 单帧约 1 ms 量级；渲完 `report_swap()`。
- 帧就绪 → 事件线程 emit（QueuedConnection）→ GUI 线程 render → `QImage` → QML。

### 4.3 请求与缓存策略

- 时间取整到 **2 s 桶**（同一桶命中缓存直接出图，不再 seek）。
- 命中新桶：立刻 `seek <t> absolute+keyframes`（10–20 ms，图可能偏 ≤5 s，先显示）；
  80 ms 内没有新请求再 `absolute+exact` 精修同一目标（1080p 25–85 ms，4K 0.3–0.9 s，后台替换）。
- 缓存键 = `path + bucket + 3D 签名(layout/half/swap) + rotMode + HDR 标志`；
  换文件/停止/退出 3D 时清空。
- 无请求 30 s：卸载媒体（`loadfile` 空），实例保留；退出时先 `mpv_render_context_free()`
  再 `mpv_destroy()`。

### 4.4 生命周期/边界

| 场景 | 行为 |
|---|---|
| 本地文件 | 启用 |
| URL / 网络流 / 直播（`viewModel.networkSource`、`isLiveSource()`） | **不启用**（不出图，只显示悬停时间） |
| 纯音频（无视频轨） | 不启用 |
| 3D（presentation） | 预览只出现在**桌面 control 窗口**；不需要逐眼复制，缩略图本身裁单眼 |
| 主播放器 seek 门控 | 完全不受影响（预览用独立实例，从不在主实例上 seek） |
| 弹窗/右键菜单打开 | 不显示预览（避免与菜单叠层） |

## 5. UI

### 5.1 进度条悬停时间

`ProgressSlider.qml` 已有 `hoverX`（2 px 竖线）。新增：紧贴竖线的 `mm:ss` 文本，
左右夹取（不越出窗口），时间格式复用 BottomBar 的 `fmt()`（抽成单一函数，避免两处实现）。
即使没有缩略图（网络流/音频）也显示。

### 5.2 预览面板

- 普通 `Item`（**不用 `Popup`**：不需要 grab/互斥，也不进 `PopupRegistry`），放在
  `uiLayer` 顶层、`z` 高于底栏；跟随指针水平居中，窗口边缘夹取，垂直固定在进度条上方。
- 样式沿用现有语言：底 `#d9111111`、radius 8、边框 `#22ffffff`（同 `PopupBase`），
  缩略图宽 = `previewWidth`（默认 320），高 = 宽 / 单眼 aspect；
  下方一行 13 px `#e0e0e0` 文本：`mm:ss · 章节名`（无章节则只有时间）。
- 显示条件：指针在进度条上、有视频、非网络源、无弹窗打开；拖动中保持显示。

### 5.3 章节

当前客户端没有章节数据。需要 `PlayerViewModel` 观察 mpv 的 `chapter-list`
（`MPV_FORMAT_NODE`）→ 暴露 `Q_PROPERTY(QVariantList chapters)`，
QML 按悬停时间取“最近一个 time ≤ t 的章节标题”。

## 6. 配置

`QSettings` 键 **`previewThumbWidth`**（默认 320，clamp 到 160–640），
只读配置 + RPC 可改，**本期不加 UI**（用户 2026-11-15）。

## 7. 风险 / 需要在实现阶段实测的点

1. **SW render 的帧时序**：暂停态 `seek` 之后是否稳定触发
   `mpv_render_context_update() & MPV_RENDER_UPDATE_FRAME`；不触发时的兜底（`frame-step`）。
2. **HDR 色彩**：mpv 文档明确 “HDR may not work properly, have to be manually handled”；
   用 4.1 的 tonemap 链实测，必要时加 `--sws-allow-zimg=yes`。
3. **4K exact 精修 0.3–0.9 s**：只作后台替换，不能阻塞已显示的关键帧图；
   若体验不可接受，再评估预览实例 `hwdec=nvdec`（多一个 CUDA 上下文，需测对主播放器 VSR/VFG 的影响）。
4. **CPU 抢占**：软解 1080p 单帧 10–40 ms，悬浮时每秒数次；4K 建议先 keyframe。
5. **进程内崩溃面**：预览实例与主实例同进程（Haruna 同款取舍）；若遇到解码器崩溃，
   退回子进程方案 C（配方已实测）。

## 8. 验证计划（实现后）

- RPC 钩子：`preview <sec>` → 返回 PNG；`get-preview-state`（尺寸/缓存命中/在途）；
  `get-chapters`。沿用 `tests/hotkeys/rpc.py` 风格。
- 断言：尺寸 = 320×宽高比、非全黑、p50 延迟 <120 ms（1080p 本地）；
  悬停时间与 `time-pos` 一致；章节名匹配 `chapter-list`；
  3D 下缩略图为单眼（宽高比 = 单眼）；HDR 内容不惨白；URL/音频/直播不出图。
- 手工：悬浮/拖动/离开、窗口边缘夹取、3D 控制窗口 vs presentation。

## 9. 不做（Non-goals）

- 拖动时的整条缩略图带（filmstrip）。
- 后台 storyboard 预生成 / 磁盘缓存 / 大文件 IO。
- 缩略图尺寸的 UI 控件。
- 在 mpv 的 OSC 上画缩略图（`overlay-add`）：我们的 UI 是 QML、3D 下还在另一个窗口。

## 10. 许可

- 本项目 GPL-2.0；mpv 以 `-Dgpl=true` 构建（GPL-2.0+）。
- thumbfast 的配方（MPL-2.0）只作为设计参考；Haruna（GPL-3.0-or-later）**只借鉴思路，不拷代码**。
- 不新增第三方依赖（FFmpeg 库、libmpv 都已在包里）。

## 11. 实现落地（2026-11-15）

### 11.1 代码位置

| 文件 | 职责 |
|---|---|
| `src/client/PreviewThumbnailer.{h,cpp}` | 进程内第二个 libmpv 实例 + `MPV_RENDER_API_TYPE_SW`；两级 seek、2 s 桶缓存、看门狗、CPU 旋转 |
| `src/client/PlayerViewModel.{h,cpp}` | 参数汇聚（路径/视频可用性/3D 布局/旋转/HDR）、章节镜像、QML 接口、`previewThumbWidth` 持久化 |
| `src/client/RpcServer.cpp` | `preview` / `get-preview-state` / `get-chapters` / `get|set-preview-width` / `debug-hover` |
| `src/client/ui/SeekPreview.qml` | 悬浮面板：缩略图 + `时间 · 章节名`；无图时退化成时间气泡 |
| `src/client/ui/ProgressSlider.qml` | 悬停位置/比例/时间、请求节流入口、测试用 `debugHoverMs` |
| `src/mpv/video/out/vo_libmpv.c` | **mpv 补丁**：SW 后端的 rect 计算忽略 `rotate`（见 11.3） |
| `tests/hotkeys/test_thumbnails.py` | 26 项运行期断言（取帧/缓存/宽度/章节/面板/单眼裁剪/音频不启用） |

### 11.2 与调研设计的差异

1. **取帧尺寸不再依赖 vf `scale`**：SW 渲染器按 `SW_SIZE` 用 sws 缩放，尺寸取
   `video-out-params` 的 `dw/dh`（未旋转的显示宽高比）。少一层滤镜、也避免
   滤镜链与 VO 的宽高比打架。
2. **旋转在 CPU 上做**：mpv 的 `video-params/rotate`（= 文件元数据 + 菜单角度）
   在渲染后对 `QImage` 调 `QTransform::rotate`（320×180 级别 <1 ms）。原因见 11.3。
3. **half（源里每眼被压扁一半）**必须 `crop…,scale=iw*2:ih,setsar=1`：crop 会把
   像素宽高比改成 1:2 以保持显示比，漏掉 `setsar=1` 时 mpv 的 `dw/dh` 会再乘一次
   （实测 640×360 → 640×720，缩略图变成竖着的）。
4. **同目标 seek 不出帧**：mpv 对"目标与当前位置相同"的 seek 不产生新帧；两个
   后果都有兜底——
   - `exact` 阶段撞同一目标：说明关键帧已经落在该时间上，直接标记为精确；
   - `keyframe` 阶段撞同一目标（链/宽度变了要重渲）：目标 +250 ms 强制一次真 seek；
   - 另有 2 s 看门狗，清在途状态并重试一次（两次失败放弃，保留上一张图）。
5. **参数代次**：链/旋转/宽度变化时 `seekGen_++`，在途 seek 的旧构图帧直接丢弃，
   避免切 3D 布局时闪一帧错误裁剪。
6. **Wayland 无法程序化移动指针**（`QCursor::setPos` 无效）→ 加 `debug-hover <秒>`
   RPC + `PlayerViewModel::previewDebugHoverMs`，测试靠它驱动面板（同时把
   overlays 显示出来，否则抓窗口只有视频）。

### 11.3 mpv 补丁：SW 渲染器的旋转崩溃（必须保留）

`vo_libmpv` 的 driver caps 带 `VO_CAP_ROTATE90`，所以 mpv 不会插软件旋转滤镜，
而是把 `img_params.rotate` 原样交给渲染后端、并在 `mp_get_src_dst_rects` 里按
"已旋转"算矩形。GPU 后端（libplacebo）会真的旋转，**SW 后端不会**——它拿到的是
未旋转的帧、却按旋转后的 `src` 矩形去 `mp_image_crop`：

```
vsr-player: ../video/mp_image.c:607: mp_image_crop: 断言 "x1 <= img->w && y1 <= img->h" 失败
```

（实测：打开菜单旋转 90° 后取缩略图必崩，帧 1920×1080、src 矩形 1080×1920。）

修法（`src/mpv/video/out/vo_libmpv.c`，只在 `renderer->fns == &render_backend_sw`
时生效）：rect 计算前把 `rotate` 清零，帧原样交给调用方，旋转由
`PreviewThumbnailer` 在 CPU 上完成。主播放器（Vulkan）走原路径，不受影响。

### 11.4 RPC（测试/脚本）

| 命令 | 说明 |
|---|---|
| `preview <out.png> <秒>` | 请求并等待一帧，落盘 PNG，返回 `{w,h,latencyMs,serial}`；超时 5 s；无本地视频时明确报 `preview not available` |
| `get-preview-state` | `{ready,serial,loaded,path,latencyMs,frames,cacheHits,width,imgW,imgH[,error]}` |
| `get-chapters` | `[{time,title}]`（mpv `chapter-list` 镜像） |
| `set-preview-width <160..640>` / `get-preview-width` | 缩略图宽度（持久化 `previewThumbWidth`，本期无 UI） |
| `debug-hover <秒|off>` | 伪造进度条悬停（Wayland 测试用；会顺带显示 overlays） |

### 11.5 实测（本机，发布同源构建）

- 1080p h264：首次取帧 ~42 ms（引擎自报），480 宽 ~25 ms；同桶命中 0 ms。
- 4K HEVC / 8 bit：keyframe 级 ~10–60 ms，exact 精修 0.3–0.9 s（软解，后台替换）。
- 旋转 90°/270°：+1 ms 级（QImage 旋转），输出 180×320。
- HDR10 源：自动插 `zscale/tonemap` 链，缩略图为 SDR 正常色彩。

### 11.6 已知限制

- 3D 的 eye 裁剪按**源帧轴**做（SBS 切左右、TAB 切上下）。若文件同时带旋转
  元数据（极少见），裁剪轴与旋转后的视觉轴不一致。
- 网络流/直播/纯音频不启用（`networkSource` / `isLiveSource` / 无视频轨）。
- `bd://` 蓝光标题允许预览（本地 ISO），但每次取帧要走 libbluray 定位。
- 预览实例软解：4K exact 精修偶发 >1 s。

（补充）RPC `set-preview-thumbnails <0|1>`：等价右键菜单「界面 → 缩略图」。
