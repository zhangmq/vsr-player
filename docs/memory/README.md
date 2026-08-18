# docs/memory — 开发记录（源自 Claude Code 会话记忆）

本目录是项目开发过程中的调研/调试记录，源自开发机上的 Claude Code 会话记忆，2026-08 拉入仓库以便追溯。已按"知识/经验"筛选：移除过程性记录（排查流水、已闭环修复、已迁移到 AGENTS.md 的规则）和过期记录（决策已废止、代码已重构）。

## ⚠️ 重要：内容时效

这些文件是**历史调研记录**，保留当时的状态和结论。其中部分信息**已过时**，与当前代码不符——**引用前务必对照代码，以仓库根 `AGENTS.md`（修正后的权威）为准**。已知过时点：

| 主题 | 记忆文件中的旧说法 | 当前实际（见 AGENTS.md） |
|------|-------------------|------------------------|
| RIFE 模型 | 4.25（"无需更换"） | **4.26 v2**（质量全档胜出） |
| 闪回问题 | "待解决/验证方向=离线实验" | **已解决**（Interleave 语义） |
| 插帧引擎 | "rife lite = 主引擎" | **full FP16 唯一引擎**（lite 已拆） |
| aboutToBlock | "应删除/待验证" | **已删除** |
| FRUC 档位 | 30/40/60 | **[Off, 40, 48, 60]**（`--fruc 30` 已不接受） |

## 保留文件（知识/经验类）

- `mpv-render-context-create-18.md` — mpv_render_context_create -18 根因（libplacebo features 要求）与修复清单
- `nano-investigation-status.md` — Nano（Open-Frame-gen）调查最终结论：发布权重=帧平均器（flow 恒零实证）；TRT 图级 bug 发现
- `vfx-no-trt10-needed.md` — VFX SDK 对 TRT 零符号调用（readelf/nm 实证），VFX/TRT 打包决策依据
- `vfx-trt-symlink-rename.md` — third_party/nvvfx/lib 无版本 TRT 软链改名（TRT 10/11 冲突修复）
- `fruc-research-conclusion.md` — FRUC 调研结论：NVOFA vs RIFE 成功率对比（9.7% vs 96.3%），路线决策依据
- `hardware-fruc-status.md` — NVOFA 硬件光流 FRUC 状态：stride 布局、444 编码教训、引擎事实（备选路线参考）

## 清洗说明

拉入仓库时已移除：frontmatter 中的会话元数据（originSessionId 等）、个人文件路径（如用户"下载"目录中的 SDK 压缩包路径）。文件中的 `~/.local`（应用安装目录）、`/tmp`（实验产物位置）为项目运行/调试相关路径，保留。
