# 启动 idle 策略（无参数启动不载入媒体）

**日期**：2026-10-17  **结论**：采纳（用户明确要求）
**触发**：用户看启动日志发现"启动后 VFG 要工作一段"（19 行 `vfg-status`）并怀疑"无参数启动有几率不暂停"。

## 一、"启动不暂停"不成立（实测）

用复制到临时 HOME 的用户配置（不碰真实 watch_later）跑无参数启动 8 次：
`pause=yes`、`time-pos` 全程不动，**8/8 都是暂停态**（`resume state found → start paused` 生效）。
日志里那些 `vfg-status` 不是播放产生的，而是**精确 seek 的丢弃帧**：

mpv 的 hr-seek（`--hr-seek=default`，`start=` 走 `MPSEEK_ABSOLUTE` → `playloop.c:337` 的
`opts->hr_seek >= 0 && seek.type == MPSEEK_ABSOLUTE`）把 demuxer 跳到 ≤ 目标的关键帧，
然后**把这些帧全部解码、全部过完滤镜链（VSR+VFG）再丢掉**
（丢弃点在滤镜之后：`player/video.c:522` 的 `hrseek && img->pts < hrseek_pts`）。
用户那份 watch_later 是 `start=12.302`，该 webm 关键帧在 **6.25 s** →
6.05 s ≈ **142 个源帧**被白跑，VFG 按 k=4 输出 **570 帧**。

`--hr-seek-framedrop`（默认 yes，本意让解码器提前丢帧）在本机**无效**：它只设
`avctx->skip_frame = AVDISCARD_NONREF`（`video/decode/vd_lavc.c:1158`），nvdec 不理会
（`hr-seek-framedrop=no/yes` 测得丢弃帧数一样），所以活儿全落在滤镜链上。

实测（同一份状态，海报可显示的时刻 = `VO: [libmpv]` 行，`build/scratch/pause_cost.py`）：

| 方案 | 海报出现 | VFG 白干 |
|---|---|---|
| 现状：链里 VFG 开着跑 seek | 2.85 s | 570 输出帧（~1.3 s）+ VSR 142 帧（~0.5 s）|
| seek 期间 `vfg:fps=off`，seek 完再开 | 1.28 s | 0 |
| seek 期间整链只留 hwup | 0.70 s | 0（但海报是未超分画面——mpv 不会重跑已显示帧的滤镜）|

mpv 自带 CLI 同参数（`--pause --start=12.302 --vf=hwup,vsr:scale=2,vfg:fps=4`）表现一致
（VO 行前同样 19 行 vfg-status）→ 属上游行为，被重滤镜放大。

## 二、决定：无参数启动不载入（idle）

用户拍板："启动时不载入，点播放载入按照上次记录的进度启动即可。"

- 有历史进度时 `main.cpp` 只调 `PlayerViewModel::restorePlaylistIdle()`：
  `loadfile <p> append`（`action.play=false`，`player/command.c` cmd_loadfile 的启动条件是
  `LOAD_TYPE_REPLACE || (action.play && !playlist->current)`）→ 列表进 mpv、**没有 current 条目**
  → 不解码、不建滤镜链、不 Load 引擎、不做续播 seek。
- 按播放 → `play()`/`togglePlayPause()` 的 `!fileLoaded_` 分支 → `resumeLastPath()` →
  `playlist-play-index` → 此时才载入，mpv 用 watch_later **自动恢复上次进度**
  （与从列表正常启动同一条路径，无需新逻辑）。
- 代价（用户已知并接受）：那 142 帧的丢弃过滤没消失，只是从"启动"挪到"按播放"——
  实测按播放→首帧 **2.25 s**（`build/scratch/idle_start_test.py`）。idle 启动本身零滤镜/零引擎工作。
- 显式 `-- --pause=yes` 不进 idle（`main.cpp` 的 `explicitPause`）：仍载入并停在暂停（海报可见），
  因为这是用户显式要求的；`MpvController::init` 的 `start_paused` 参数随之删除
  （暂停语义完全交给 mpv 自己的选项）。
- 顺带修一个 idle 附带项：`pause` 属性观察器改为 `updatePlaying(!p && fileLoaded_)`——
  idle 时 mpv 的 `pause` 初值是 false，否则 UI 会在没载入时显示"正在播放"。

## 三、被否掉的方案

1. **启动 seek 期间把插帧置 off、`PLAYBACK_RESTART` 后再开**（省 1.6 s，海报仍精确、仍暂停）：
   用户认为绕远（多了"延迟打开插帧"的状态机）。数据保留在上表，若将来觉得按播放等太久可复用。
2. **不恢复位置 / 关键帧 seek 后再精确 seek**：位置错（差一个关键帧间隔，本文件 6 s），
   或把同样的丢弃过滤再跑一遍。
3. **只令 mpv idle 但仍 loadfile**：`loadfile` 必然建链并执行 `start=` 续播 seek，绕不开。

## 四、复现/验证脚本（均在 `build/scratch/`，未入库）

- `idle_start_test.py` — idle 启动零工作 + 按播放续播 + watch_later/列表不被破坏
- `startup_paths_test.py` — 四条启动路径回归（CLI 文件 / 无历史进度 / 显式 pause=yes / 3 条列表从第 2 条续播）
- `pause_cost.py` — 上表三种链配置的海报时刻与丢弃帧数
- `pause_repro.py` — 无参数启动暂停态判定（8/8）
