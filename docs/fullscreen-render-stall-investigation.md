# 首次切全屏后 Qt 停止出帧（2026-11-19 调查）

> 现象：Linux/Wayland + Vulkan（`QSG_RHI_BACKEND=vulkan`，外部 Vulkan 设备由本项目创建并
> 交给 Qt）+ `QSG_RENDER_LOOP=basic`。**视频开始播放后第一次进入全屏**，画面定格、UI 不再刷新；
> 音频/时间轴继续。不是每次必现？——本机用 RPC 注入 `F`（与按键同一条链路）**8/8 稳定复现**。

## 1. 结论（已证实的部分）

- **不是视频阻塞 UI**：卡住时 GUI 线程在 `ppoll`（事件循环空闲），RPC 正常应答；
  mpv 侧 `time-pos` 前进、`frame-drop-count` 以 ≈50/s 增长（= 输出帧率）、`vsr-status` /
  `vfg-status` 持续输出 ⇒ 解码与滤镜链仍在跑。
- **是 Qt 不再出帧**：`QQuickWindow::beforeRendering` / `frameSwapped` 计数**完全冻结**；
  我们自己的 `update()` + `UpdateRequest` 请求仍在发生（`updates` 计数继续涨），但
  `updatePaintNode` 一次都不再被调用。
- **触发条件是"窗口状态切换"本身**，不是尺寸变化、不是副屏、不是 HDR：
  - `VSR_HDR=off`、`VSR_HDR_DECLARE=0` 照样卡；
  - 先把窗口 `resize 1920 1080`（与全屏同尺寸）再按 `F`，照样卡；
  - `QSG_RENDER_LOOP=threaded` 照样卡（所以不是 basic 循环特有的 bookkeeping）。
- **只有 surface 重建能恢复**：`resize`、`hide()+show()`、全屏→窗口 都立刻恢复；
  单纯"强制重绘"（`update()` + `UpdateRequest`）**无效**。

### 1.1 后续补充（同日 20:00 之后）

- **与网络流无关**：本地文件（`input/Jellyfish_1080_100s_h264.mp4`）同样 100% 复现。
- **与"视频在播"也无关**：启动即 `--pause=yes`（没有帧在跑）→ 按 `F` 后仍停摆；
  此时 `open-menu`（纯 QML 变更）也不再触发任何一帧（`paint` 不动）。
- **不是通用 Qt/Wayland bug**：用同一套 Qt 6.12 + `QSG_RHI_BACKEND=vulkan` +
  `QSG_RENDER_LOOP=basic` 写了个 ~80 行最小程序（`QQuickView` + 动画矩形，
  `build/qt-repro/`），切全屏后 `beforeRendering` 继续 50/s —— **不复现**。
  ⇒ 差异在本项目的用法，最大嫌疑是 `QQuickGraphicsDevice::fromDeviceObjects()`
  共享外部 Vulkan 设备（Qt 需为重建后的 Wayland surface 重新创建 swapchain）。
- 应用内 `VSR_QT_OWN_VK=1`（让 Qt 自己建设备）的对照**不成立**：视频画面依赖我们设备上的
  rtImage，换设备后整条渲染本来就坏，计数太小无法区分。

## 2. 关键计数（`RPC get-render-stats`，2026-11-19 实测）

正常播放（窗口 1280×720）：

```
paint=122 updates=122 swaps=169 befores=266 sincePaint=3ms drops=0 size=1280x720
```

按 `F` 进入全屏后（watchdog 关闭的对照）：

```
paint=139 befores=297 sincePaint=3669ms drops=165 winVisible=1 winExposed=1 size=1920x1080
（3 秒后）paint=139 befores=297 sincePaint=6669ms drops=254
```

即 `paint`/`befores`/`swaps` 冻结，`updates` 与 `drops` 继续增长。

恢复矩阵：

| 手段 | 结果 |
|---|---|
| `force-render`（update + UpdateRequest） | ❌ 无效 |
| `nudge-window`（hide + show） | ✅ 恢复，但会掉出全屏（1920×1006） |
| `resize <w> <h>` | ✅ 恢复（同时离开全屏） |
| 再按 `F`（全屏→窗口） | ✅ 恢复 |

## 3. 现场代码（未提交，2026-11-19）

诊断/自救接口（`RpcServer`）：

- `get-render-stats`：`rendered= paint= updates= swaps= befores= sinceRender= sincePaint= drops= winVisible= winExposed= size= screen=`
- `force-render`：只补一次重绘（`renderRequested_` + `update()` + `UpdateRequest`）
- `nudge-window`：`window()->hide(); show();`
- `bounce-visibility`：`setVisibility(Windowed)` → `setVisibility(FullScreen)`

计数来源：

- `Video::updateReqs_`（mpv update callback 次数）、`Video::paintCb_`（`updatePaintNode` 次数）
- `QQuickWindow::frameSwapped` / `beforeRendering` → `PlayerViewModel::frameSwaps_` / `beforeRenders_`
- `PlayerViewModel::lastRenderMs_`（真正渲染了 mpv 帧）/ `lastPaintMs_`（Qt 画了帧）

看门狗（`PlayerViewModel::checkRenderStall()`，在 `osdPoll()` 入口每 ~100ms 跑一次）：

- 条件：`playing && fileLoaded && !pause && now-lastRender > 1500ms && drops 在涨`
- 阶段 1：`emit renderStalled()` → `main.cpp` 调 `Video::forceRender()`
- 阶段 2（仍 >3s）：`emit renderStallPersisted()` → `Video::nudgeWindow()` + 若 `fullscreen` 则重新 `setVisibility(FullScreen)`
- 可用 `VSR_RENDER_WATCHDOG=0` 关闭

副作用：hide/show 期间 `window.visibility` 变成 Windowed，QML 里
`onVisibilityChanged → viewModel.fullscreen = (visibility === FullScreen)` 会把
`fullscreen` 置 false（`main.qml:427-439` 双向同步），所以恢复后**掉出全屏**。

## 4. 尚未验证的假设（下一步）

1. **外部 Vulkan 设备 + Wayland surface 重建**：Qt 的 `QRhiVulkanSwapChain` 在 surface
   重建后没能继续出帧。验证手段：把 `QQuickGraphicsDevice::fromDeviceObjects(...)` 换成
   Qt 自建设备（放弃与 mpv 共享设备仅做实验），看是否还卡；或 `QSG_RHI_BACKEND=opengl` 对照。
2. **Qt 版本已知问题**：本机 Qt 6.12.0；查 Qt bug tracker（QTBUG）里
   "fullscreen stops rendering" / "Vulkan swapchain Wayland" 相关条目。
3. **最小复现**：一个只含 `QQuickView` + `QQuickGraphicsDevice::fromDeviceObjects` 的
   ~100 行程序，切全屏看是否复现——能把责任落到 Qt 还是本项目特有组合。
4. 若确认是 Qt 侧：考虑"全屏不用窗口状态切换"（无边框 + resize 到屏幕尺寸）作为规避。

## 4.1 根因（2026-11-19 21:00 结论，已确证）

**这是 Qt 6.12.0 的缺陷，上游已修但尚无发布版本：QTBUG-151831。**

- 修复提交：`89499484de5431a84947cf175d9166b3b337d2c5`（2026-10-27 提交 / 11-04 入库，
  `Fixes: QTBUG-151831`、`Pick-to: 6.8`），6.12 分支 cherry-pick = `2921382a52`。
- 提交说明逐字命中本机现场：*"NVIDIA 的 Vulkan 驱动会在 Wayland 合成器更新 surface 的
  dmabuf feedback 时返回 `VK_ERROR_OUT_OF_DATE_KHR`；两个渲染循环只要窗口尺寸没变就继续
  复用旧 swapchain，之后每一帧都以同样方式失败、窗口再也画不出来。Vulkan 要求此时必须
  重建 swapchain。"*
- 我们 Arch 上的 Qt 是 **6.12.0**（10-09 打包），**早于该修复**；6.12.1 尚未发布
  （`download.qt.io/official_releases/qt/6.12/` 只有 6.12.0）。

### 证据链（本机实测）

1. **最小复现不含本项目任何代码**：~40 行 `QQuickView` + 动画矩形 +
   `QSG_RHI_BACKEND=vulkan` + `QSG_RENDER_LOOP=basic`，`setVisibility(FullScreen)` 后
   永久停止出帧（20:14–20:25 连测 12+ 次必现；20:26 后一段时间转为不现）。
   对照：**OpenGL 后端全屏正常**、**只 resize 到同尺寸正常** ⇒ Wayland+Vulkan 全屏专属。
2. **卡住现场状态全"正常"**（用 `QQuickWindowPrivate` 直接读）：`exposed=1 visible=1
   fullscreen`、`isRenderable()=1`、`updatesEnabled=1`、`hasActiveSwapchain=1`、
   `hasRenderableSwapchain=1`、`swapchain->surfacePixelSize()=3840x2160`、
   `qrhi->isDeviceLost()=0`、`QPlatformWindow` 指针未变、**Qt 无任何 qWarning**。
3. **信号二分**：`beforeFrameBegin` ✅ → `afterFrameEnd` ✅，但 `beforeSynchronizing` /
   `beforeRendering` ❌ ⇒ 死在 `rhi->beginFrame()`；该路径里唯一静默失败是
   `FrameOpSwapChainOutOfDate`。
4. **Vulkan WSI 隐式层抓到的真值**（`build/qt-fix/wsi-trace/wsi_layer.cpp`，只写内存环形缓冲、退出时
   落盘，零侵入）：全屏切换（t=3.00s）后第一帧
   `acquire res=-1000001004`（`VK_ERROR_OUT_OF_DATE_KHR`），**此后所有 acquire 都是同一个
   错误，且再也没有一次 `vkCreateSwapchainKHR`**（trace 里 create 只有 2 次：初始
   2560x1440 和全屏时 3840x2160）。⇒ Qt 没有按 Vulkan 要求重建 swapchain。
5. **代码对照**：`v6.12.0` tag 的 `qsgrenderloop.cpp` / `qsgthreadedrenderloop.cpp` 里
   **完全没有** `FrameOpSwapChainOutOfDate` 分支；6.12 分支补丁加上
   `data.swapchainOutOfDate = true` + `maybeUpdate()`，并在重建条件里加上该标志。
   ⇒ 我们缺的正是这段。
6. **release 变体无关**：`full` 变体自带的 `libQt6Quick.so.6` 与系统
   `/usr/lib/libQt6Quick.so.6` **md5 完全相同**（`98745e86…`）——打包方式不影响该缺陷。

### 修法（按可用性排序）

| 方案 | 说明 |
|---|---|
| 等 `qt6-declarative` 6.12.1 | 上游已修，等 Arch 打包；无需我们改动 |
| 自行给 6.12.0 打补丁 | 从源码构建 qtdeclarative + 应用补丁（`2921382a52.patch`，+15/-2 行） |
| full 包内替换 `libQt6Quick.so.6` | full 变体本来就自带 Qt 库，换成打过补丁的即可（无需 system 权限） |

验证判据（补丁前后对照，同一个最小程序）：
```bash
# 打补丁的 Qt 用 LD_LIBRARY_PATH 指过去，全屏后 beforeRendering 计数应持续增长
LD_LIBRARY_PATH=/path/to/patched/lib ./qtfs10
```

### 补丁有效性实测（2026-11-19 21:20）

用 WSI 层**合成**该故障（`VSX_WSI_FORCE_OOD=1`：第一次 acquire 起持续返回
`VK_ERROR_OUT_OF_DATE_KHR`，直到 swapchain 被重建——与 NVIDIA 的真实行为一致）：

| | 系统 Qt 6.12.0 | 打了 2921382a52 的 Qt |
|---|---|---|
| 重建 swapchain | 无 | `acquire OOD @0.006s` → **`createSwapchainKHR @0.010s`** |
| 日志 | 无 | `qt.scenegraph.renderloop: swapchain out of date` |
| 结果 | 一直失败（真实场景下即永久定格） | 一帧内恢复出帧 |

### 多轮 A/B（2026-11-19 21:33，**同一个已安装二进制**，只换 Qt Quick 这一个库）

播放器私有库目录 `~/.local/lib/vsr-player/` 在二进制 RUNPATH 上，因此
`libQt6Quick.so.6` 放进去即生效，无需 sudo、不影响系统其它 Qt 程序。

| 配置 | 轮数 | 结果 |
|---|---|---|
| 系统 Qt 6.12.0（用 `LD_LIBRARY_PATH=build/qt-fix/sysqt` 把该库抢回来） | 4 | **4/4 卡死**（`rendered` 冻结 80→80，`drops` +135/5s，回窗口后自行恢复） |
| 补丁 Qt（`~/.local/lib/vsr-player/libQt6Quick.so.6`） | 8 | **0/8 卡死**（`rendered` 稳定 +120/5s，`drops` 0–6） |

测试脚本：`build/qt-fix/vsr_round.py`（启动→等稳定→`F`→采样→`F` 回来→判定），可用
`vsr_round.py <system|patched> <轮数> [--player 路径]` 复跑。

真实播放器 + 补丁库（`LD_LIBRARY_PATH` 指过去，VSR auto + VFG 2）：
按 `F` 进全屏、再按 `F` 回窗口，`rendered` 451→631→751 持续增长、`drops=0`；
`/proc/<pid>/maps` 确认加载的是 `libQt6Quick.so.6`（补丁版）。

⚠ **只换 `libQt6Quick.so.6.12.0` 时必须同时带上 Arch 在 qt6-declarative 上打的 3 个
cherry-pick**（`prepare()` 里的 8c23f65e / 7214a320 / 88a046d8，含 Plasma 退出挂起修复），
否则换完会丢 KDE 侧修复。补丁本体见 `docs/patches/qt6-declarative-QTBUG-151831.patch`。


## 4.2 关联问题：关窗后进程/终端不退出（QTBUG-150279，同日定位）

**现象**（2026-11-20 用户报）：关掉播放器窗口后，终端不回到提示符 —— 进程还活着。

**现场**（gdb 抓到，PTRACE 放开后 attach）：
```
pthread_cond_wait ← QBasicReadWriteLock::contendedTryLockForWrite
  ← QtWaylandClient::QWaylandWindow::reset()
  ← QtWaylandClient::QWaylandWindow::~QWaylandWindow()
  ← QWindowPrivate::destroy() ← QWindow::event() ← processCloseEvent
```
主线程 `wchan=futex_wait`、`winVisible=0/winExposed=0`、`sincePaint≈42s`、RPC（独立线程）仍应答。
即：**关窗销毁窗口时卡在写锁上，而这个读锁是 GUI 线程自己刚泄漏的**。

**根因**（上游 **QTBUG-150279**，已在 6.12.1 修复）：`QRhiVulkan::beginFrame()` 早退
（`VK_ERROR_OUT_OF_DATE_KHR` / `DEVICE_LOST` / 其他错误）时**没有通知平台插件
`endFrame`**；Wayland 插件要求 begin/end 成对，否则 `mSurfaceLock` 的读锁一直握着 →
之后 `QWaylandWindow::reset()` 要写锁 → 自锁死。触发条件与 §4.1 完全同源（NVIDIA 返回
`OUT_OF_DATE`），两个问题是同一个驱动行为打出来的两处缺陷。
修复提交：qtbase `c68e1fc1`（6.12 分支，dev = `d4be6688`，6.8 = `6f51214d`），只有 4 行：
beginFrame 入口挂一个 `qScopeGuard` 调 `endFrame`，成功路径 `dismiss()`。

**确定性复现**（不靠运气）：用 WSI 层强制持续 `OUT_OF_DATE`，再关窗 ——
系统 Qt 6.12.0 必然卡死；换补丁版 `libQt6Gui` 后 **2 秒内正常退出**。
脚本：`build/full-test/repro-ood-close.sh`。

**修复产物**：`build/qt-fix/lib/libQt6Gui.so.6`（= qtbase 6.12.0 + c68e1fc1 + Arch 的
md4c 0.6 适配），与 `libQt6Quick` 一样放到播放器私有库目录即可生效（同一个版本闸门）。

**✅ 已收尾（2026-11-26）**：本机 `qt6-base` / `qt6-declarative` 升到 **6.12.1**（Arch，
两个缺陷都在其中），补丁库不再需要：

- 安装包侧：`install.sh` 的闸门本来就按 host 版本判断——`< 6.12.1` 才装补丁库，
  `>= 6.12.1` 跳过并**删除**已有的两个 `libQt6*.so.6`。
- 开发构建侧：`src/client/meson.build` 的 `dev_qt_fix_args` 现在也加了版本闸门
  （只在 host Qt `< 6.12.1` 时才注入 `build/qt-fix` 的 rpath）。没有闸门时，6.12.0 的
  补丁库在 6.12.1 上会在**加载期**失败：
  `undefined symbol: _ZN14QObjectPrivateC2E16QtPrivate_6_12_0, version Qt_6_PRIVATE_API`
  ——Qt 私有 API 带版本化符号，6.12.0 的库不能配 6.12.1 的其它 Qt 库。
- ⚠ **Qt 大版本更新后必须 `meson setup --wipe <builddir>`**，不能只 `--reconfigure`：
  meson 会**缓存 pkg-config 查到的依赖版本**，`--reconfigure` 不重新查询。实测：升级到
  6.12.1 后 `--reconfigure` 仍按 6.12.0（甚至 6.11.1）解析依赖 → 要么编不过
  （私有头路径 `/usr/include/qt6/QtGui/6.12.0/...` 不存在），要么链接进旧 rpath。
```

## 5. 相关文件

- `src/client/Video.{h,cpp}`：渲染循环、`updatePaintNode`、诊断计数、nudge/force
- `src/client/PlayerViewModel.{h,cpp}`：看门狗、`notifyPaint`、`logEndFile`
- `src/client/RpcServer.{h,cpp}`：诊断 RPC
- `src/client/main.cpp`：信号接线、`frameSwapped`/`beforeRendering` 计数
