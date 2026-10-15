---
name: fruc-auto-degrade-removal
description: 插帧性能自动降级已移除（2026-10-15）：旧判据是节点内自测、看不到下游（VSR 按输出帧计费 + 出图通路），实测目标达不到时全程沉默；统一跨节点方案搁置待设计。含完整实测证据与复现方法

# 插帧性能自动降级：移除记录 + 统一方案待设计（**2026-10-15 搁置**）

> **当前状态**：旧的分级降级逻辑（`cost` 超预算 → 60→48→40→30 → 直通）**已从代码移除**
> （2026-10-15，见 §4 变更清单）。
> 覆盖 rife/vsr/VFX 新版插帧的**统一性能自适应方案尚未设计**，用户决定短时间不处理。
> 本文记录：现象、根因、同族缺陷、已做的处置、以及重新开工时需要先定清的问题。

## 0. 一句话结论

降级判据 `adapt_avg(RIFE 单对耗时) > frame_budget_ms` 是**节点内自测**：它只能看到 RIFE 自己的
GPU 耗时，而真实短缺通常发生在该节点**下游**（VSR 按**输出帧**计费 = k=out/src 次/对，再加上
VO/渲染通路）。于是出现"目标远远达不到、判据却全程沉默"的失效模式；这类场景下降级**根本不触发**，
而一旦触发它确实有效（见 §1）。因此问题不是"降级无效"，而是"该降级时不降级"。判据不可靠 →
按要求先移除，统一方案另议。

## 1. 现象与实测（口径：`VSR_LOG_VIDEO=1` 逐帧渲染时间戳 = 真实上屏率）

`estimated-vf-fps` 反映的是**过滤链产出率**（#3 里显示 59.94，看着完全正常），**不能**当上屏率用；
`frame-drop-count` 与客户端出图计数才是真值。

| # | 场景（RTX 5060 Ti） | 目标 | 链产出 | **实际上屏** | mpv 丢帧 | 降级触发 |
|---|---|---|---|---|---|---|
| 1 | goose_480p 30fps，scale auto(3×)，fruc 60 | 60 | 60/s | **60.0 fps** ✓ | 0 | 不需要 |
| 2 | 同上，fruc 48 | 48 | 48/s | **48.0 fps** ✓ | 0 | 不需要 |
| 3 | Jellyfish_1080p 29.97，scale auto(1.33×)，fruc 60 | 60 | 60/s | **17.5 fps** ✗ | **+52/s** | **否**（判据沉默） |
| 4 | 同上，scale 4× | 60 | 48/s | **10.9 fps** ✗ | **+38/s** | **否** |
| 5 | 同上，scale 4×，fruc 40 | 40→30 | 30/s | **30.0 fps** ✓ | 0 | 是（22.8 > 21.4） |
| 6 | 同上，scale auto，fruc 48 | 48→40 | 40/s | **40.0 fps** ✓ | 0 | 是（23.4 > 21.4） |
| 7 | Fallout 2160p 24fps，fruc 60 | 60→直通 | 24/s | 源帧率 ✓ | 0 | 是（102.6 ≫ 41.7） |
| 8 | 同上（scale auto/4×），fruc off（对照） | — | 30/s | **30.0 fps** ✓ | 0 | — |

读法：**目标可达时能精确达标**（#1/#2 正好 60.0/48.0 fps）；**降级一旦触发就能救回**（#5/#6/#7）；
**失效只发生在"该降级时没降级"**（#3/#4）。

#3/#4 的上屏节拍不是均匀变慢，而是"~30/s 基线 + 周期性 100~133ms 卡顿"，即 mpv 非
display-sync 的丢帧路径（`vo.c` 的 `in->dropped_frame → drop_count++`，以及同文件里
"degrade to 10 FPS rather than freeze"的 100ms 钳制）在持续丢帧。

## 2. 根因

判据（当时的 `vf_rife.c`）：`adapt_avg(pair_cost) > frame_budget_ms * 1.05`，
预算 = `1000/src_fps − (big ? 0 : 12ms)`（下限 5ms，`big` = 宽>1920 或 高>1080）。

- **#3**：RIFE 16.0ms < 预算 21.4ms → 不触发。但一对帧在目标 60 下的真实开销 =
  RIFE 16ms + **VSR 2×8ms**（VSR 按输出帧计费，k=2 → 一对帧跑两次）+ 出图通路
  （对照 #8：同配置**每张画出的帧** 30.7ms）≫ 源帧间隔 33.3ms。
- **#4**：RIFE 17.3ms + VSR 2×11ms ≈ 39ms > 33.3ms（算力只够 ~24 对/秒），仍不触发。
- 12ms 的 VSR 估算是**猜的**，而且**不随插帧倍率 k 缩放**。
- **这类过载下过滤链照样按 60/s 产出**（#3 实测 72 条 status×30 / 36.5s ≈ 60/s），
  丢帧发生在 filter 写 pin **之后**：过滤器内部天然看不到。

### 2.1 为什么判定真值必须来自客户端（vf 节点能力边界，2026-10-15 查证）

- `struct mp_filter`（`filters/filter.h`）只暴露 `global` / `log` / `packet_pool` / `pins` /
  `stream_info`——**没有 MPContext、没有 VO 句柄、没有属性接口**。
- `enum mp_filter_command_type` = `TEXT` / `GET_META` / `SET_SPEED` / `SET_SPEED_RESAMPLE` /
  `SET_SPEED_DROP` / `IS_ACTIVE`——**没有任何运行时可观测项**。唯一的入向通道是外部发的
  `vf-command`。
- `frame-drop-count` / `vo-delayed-frame-count` 实现在 `player/command.c`，针对 `MPContext`，
  filter 拿不到；虽然 `mpv_global` 带 `client_api`，但 filter 在 mpv 线程里开 client 做同步
  属性读会**自死锁**（同类风险项目已踩过：filter 持锁 ↔ VO dispatch ↔ 渲染循环）。
- `stats` 也不是后门：`MP_STATS(vo, "drop-vo")`（`vo.c`）只写 `--dump-stats` 文件
  （`msg.c` 的 `dump_stats`），**不进** `stats_global_query()` 读的那张图；图里由显式
  `stats_*()` 调用产生，vo 侧只注册了计时项（`video-draw`/`video-flip`/`iterations`），
  **没有丢帧计数**。
- filter 能自测的只有**输入消费速率**（现在仍以 `fruc-status ... fps=` 输出）：它只在
  "过滤链自己吃不下"时有效（#4：24/s vs 源 29.97 可判）；瓶颈在呈现通路时无效——#3 实测
  仍是 30/s（core 按音频钟继续拉，丢帧在 VO），而实际只上屏 17.5/s。

## 3. 同族缺陷（已随本次移除一并消失或需在统一方案里解决）

- **A. 阶梯承诺的"src-fps 兜底"是死代码**：判据比的是 `opts->fps`（用户原目标）而非当前档位
  `out_fps`，注释却写"降到 ≤src×0.95 由 src-fps 直通兜底"。后果：最低档仍按 ≈1.0× 插值
  （实测 `out=30.00`、src=29.97、`mode=active`）——白烧 GPU 且有极端 tval 伪影，本该直通。
- **B. 启动瞬态误触发且不可回退**：实测 1.5s 内连跳 `48→40`(avg 22.9ms) → `40→30`(36.5ms) →
  `passthrough`(22.9ms)，而该档稳态成本只有 15.3ms（测量窗含引擎初启/首帧 staging 瞬态；
  `RIFE_ADAPT_SKIP=4` 只跳 4 对）。阶梯单向 → 从此降死。
- **C. 降级状态不持久**：一次 mpv 内部流重检测/整链重建（实测 ~17.7s；日志特征 = track list
  重印 + `Using hardware decoding` + VSR/rife 重新 init，客户端同期无任何操作日志）会把
  `out_fps` 重算回用户目标，阶梯需重新收敛（实测中间约 3s 暴跌）。

## 4. 已做的处置（2026-10-15）

变更范围：`vf_rife.c` / `PlayerViewModel.cpp` / README×2 / 参考索引（代码与用户可见文档）：

- **删除**：`rife_try_downgrade()` 整条阶梯、触发判据、预算模型（`frame_budget_ms`、
  `RIFE_BUDGET_VSR_MS=12ms`、"4K 不扣 VSR"特例、`adapt_warmup_done`）、`adaptive` filter 选项
  与 `vf-command rife adaptive`；`fruc-status` 去掉 `budget=`；`PlayerViewModel` 的 vf 串不再传
  `adaptive=...`；README/README_zh 选项表删行、`docs/mpv-reference-index.md` 更新、
  `AGENTS.md` 记为"无性能自动降级"并写明理由。
- **保留**（都不是性能判据）：`src-fps` 直通（目标≈源帧率时插帧无意义）、
  引擎缺失/无 PTS/软解帧的失败直通（`reason=engine|no-pts|sw`）、每对帧耗时测量窗口
  （`fruc-status` 的 `cost=`，纯观测 + 后续统一方案的原始证据）。

**验证**（逐帧渲染时间戳；降级事件计数 0/0/0，无 unknown option）：

| 场景 | 移除前 | 移除后 |
|---|---|---|
| 480p 30fps，fruc 60 | 60.0 fps | **60.0 fps**（健康路径未受影响） |
| 1080p29.97，fruc 60，scale auto | 17.5 fps，判据沉默 | **11.1 fps**（同区间波动 11–17） |
| 4K24，fruc 60 | 阶梯触发 → 直通，**24 fps / 0 丢帧** | **7.3 fps** ← **行为变化** |

⚠️ **移除的代价**：4K 纯 RIFE 过载是旧阶梯**唯一真正起作用**的场景（瓶颈恰好在 RIFE 自己身上，
本地判据"歪打正着"）。移除后不再有任何回退，观感从 24fps 变成 7.3fps。若统一方案遥遥无期，
可考虑一条**静态**策略（例如 >1080p 源在 UI 上不提供 60 档）作为临时兜底——**未实施**。

## 5. 重新开工时需要先定清的问题（统一方案）

1. **判据**：以"实际达成率"（上屏 fps / 丢帧）为准，而不是任何单节点成本。真值只有客户端有
   （`frame-drop-count` + `Video::benchFrames_` 出图计数）。
2. **测量与决策的归属**：两条路线——(a) 客户端测 + 客户端用现成 `vf-command rife fps` 逐级下调；
   (b) 策略留在 filter（单一事实源），客户端只上报实测值（需新增 `vf-command rife present-fps <fps>`
   之类的入口）。若不希望依赖客户端，还有 (c) 改 mpv 覆盖层让 core 周期性把丢帧/上屏数作为
   `MP_FILTER_COMMAND_TEXT` 推给 filter（只推数字，**不要**给 filter 加 VO 访问权，避免 §2.1 的死锁类）。
3. **动作空间与代价**：降目标帧率 / 降 VSR 倍率 / 降质量档 / 关插帧——不同动作的算力杠杆与画质
   语义不同（VSR 倍率杠杆最大但改变输出分辨率语义）。
4. **恢复与滞回**：只降不升（旧实现）还是会回升；如何避免启动瞬态（§3 B）与链重建（§3 C）导致
   误判/状态丢失；滞回窗口与阈值。
5. **与 UI 档位的关系**：用户设 60、实际跑 40 时 OSD / 设置面板如何表达，避免"设置被悄悄改掉"。
6. **覆盖范围**：必须同时覆盖 rife / vsr / **VFX 新版插帧**（未来要把 RIFE 换成 VFX SDK 已支持的
   插帧），即判据与动作要挂在"节点无关"的层面（例如由客户端统一监管整条链的达成率）。

## 6. 复现与取证方法

- 取证脚本（**gitignored**，仅本机）：`build/fruc-probe/sample.sh`（起播 + RPC 采样
  `estimated-vf-fps`/`frame-drop-count`/`decoder-frame-drop-count` + 抓 `fruc-status`/`vsr-status`）、
  `build/fruc-probe/render_rate.py`（解析 `VSR_LOG_VIDEO=1` 的 `[V ...]` 行 → 真实上屏 fps 与
  各阶段耗时）。原始日志与表格同目录，汇总见 `build/fruc-probe/FINDINGS.md`（同样 gitignored）。
- 关键命令：
  ```bash
  # 真实上屏率（60 是渲染/呈现节拍，不是"链产出率"）
  VSR_LOG_VIDEO=1 ./build/src/client/vsr-player --fruc 60 --scale auto <video> 2>/tmp/v.log
  grep -c '\[V ' /tmp/v.log      # 稳态每秒行数 = 上屏 fps
  # 逐档位对照（scale 4× / auto / off、fruc 60/48/40）
  ./build/src/client/vsr-player --fruc 48 --scale 4 <1080p>
  ```
- 口径提醒：`estimated-vf-fps` = 过滤链产出率；`fruc-status` 的 `cost=` = **本节点**每对帧 GPU
  耗时（观测值）；上屏率只能由渲染时间戳（或客户端出图计数）得到。

## 7. 待办

- [ ] 统一性能自适应方案设计（覆盖 rife/vsr/VFX 插帧；判据 = 实际达成率）→ 见 §5
- [ ] 是否为 4K 过载场景加静态兜底（§4 末；当前无任何回退，24fps → 7.3fps）
- [ ] 本机原始取证（`build/fruc-probe/`，gitignored）是否另存为长期受控记录
