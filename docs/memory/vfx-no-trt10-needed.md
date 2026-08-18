---
name: vfx-no-trt10-needed
description: VFX SDK 对 TRT 10 零符号调用（纯打包冗余），cudnn 仅调 cudnnGetVersion——player 进程只有 RIFE 需要 TRT；CLAUDE.md 依赖链图该段有误

# VFX 不需要 TRT 10（2026-08-09 readelf/nm 实证）

## 证据

| 库 | NEEDED TRT10 | TRT 符号调用 | cudnn 符号调用 |
|---|---|---|---|
| libVideoFX.so | 是 | 0 | 0 |
| libVideoFXLocal.so | 是 | 0 | 1（仅 cudnnGetVersion） |
| libnvVFXVideoSuperRes.so | 是 | 0 | 0 |
| libnvidia-ngx-vsr.so | 否 | 0 | 0 |

- nm -D 检查 UND 符号；注意 strtol/strtoul 误匹配 "trt" 子串的坑（grep -iE "trt" 匹配 str**t**ol）
- VSR 推理 = ngx-vsr 自研 CUDA + NPP，与 TRT 无关
- 唯一外部调用 = libVideoFXLocal 的 cudnnGetVersion（版本查询，任意 9.x 可满足）→ cudnn 9.25 系统组件错配无风险

## 含义

- **player 进程里真正需要 TRT 的只有 RIFE（TRT 11）**——TRT 10 三件套（nvinfer 672MB + plugin 53MB + onnxparser 4.3MB）是纯冗余，stub（含 SONAME + cudnnGetVersion）可替代，节省 ~1.4GB
- 方案：A. stub 化（vsr_proc.c 不动）B. 移除加载段 C. 维持现状（RTLD_LOCAL 隔离无功能风险）——2026-08-09 时未决策
- **CLAUDE.md 依赖链图 "ngx-vsr → libnvinfer.so.10" 段有误**（当时按 bundle 目录推断），引用时勿据此推断
- 关联：[[vfx-trt-symlink-rename]]（TRT 10/11 软链改名历史）、[[rife-implementation-status]]
