---
name: hardware-fruc-status
description: 硬件光流 FRUC 路线状态：NVOFA 在 Blackwell 可用；涂抹=编码问题（已证）；横纹残余待解决；fruc_gpu.cu 即未来 vf_fruc 核心

# 硬件光流 FRUC 路线（2026-08-09：已降级为备选）

## ⚠️ 路线最终评价（2026-08-09，用户明确）

- **NVOFA 硬件光流插帧 repeat 率太高，效果不理想**——不再是主路线；用户确认的"相当完美"（2026-08-08）是编码污染修正后的相对评价，最终以本条为准
- **全片 repeat 实测（2026-08-09）**：heya_720p 全片 13439 对 repeat **90.8%**（前帧 11890/后帧 309），有运动对成功率仅 9.7%——几乎全部插值帧回退为前帧复制（回退目标 warp0≈prev 所致）。旧 9.5%/15.5%/13% 均为安静段局部统计，勿引用
- **RIFE / rife lite 插帧成功率远高于 NVOFA**——RIFE lite 定为主插帧引擎（[[rife-model-selection]] 决策）
- **此前"涂抹=编码问题"结论修正**：三个引擎"涂抹"评价的污染根源是**我方后处理阶段的问题**（用户明确），yuv420p 编码是其中一环；RIFE 引擎本身无缺陷
- 本路线沉淀的经验（stride 布局、444 编码、官方批模式、sm_120 内核）仍有效，RIFE 路线若遇阻可回退参考

## ⚠️ 血泪教训（2026-08-08 第二次踩坑）：FRUC 输出视频色块 = yuv420p 编码污染

- **症状**：1 分钟插帧视频目测"色块"→ 误判为后处理/garbage flow → 排查两天 → **PNG 帧序列证明插值帧零瑕疵 → 色块 = 4:2:0 色度下采样**
- **正确姿势（必须第一条执行）**：输出视频 `-pix_fmt yuv444p -colorspace bt709 -color_range tv`——**FRUC 输出视频一律 444 编码，怀疑任何"视觉瑕疵"先出 PNG 帧序列对照，再碰引擎/后处理**
- **为什么反复踩**：v8 时代已有此结论（见下"涂抹=编码问题"）——v1 视频生成时未应用，直接进后处理排查。**教训：新视频产出默认 444 编码，瑕疵出现先对照已知教训清单**
- 已收尾：v3 后处理（occ→warp0）+ 444 编码 = 用户确认"相当完美"（commit f1eb574）；densify 网格 ceil(W/G) 修正（854 非整除，commit 2372e4a）；**goose_480p 全片（9158 对）+ heya_720p 1 分钟均用户确认完美**
- **终极教训（用户原话"本来从一开始就能做对，生生折腾我两天"）**：正确路径 = 官方 NVOFA 批模式 + 自写 FRUC 后处理 + **444 编码**——**两天前就具备**。弯路来源：① 制造假问题（"渐进退化"/"720p 限制"）② 色块排查绕圈（应第一时间应用 444 编码教训）③ 删除已验证实现重建。**流程纪律：任何"视觉瑕疵"→ 先出 PNG 序列对照（1 分钟出 1800 张 16s）；任何"引擎退化"怀疑 → 先对照内容帧差曲线；新视频一律 444 编码**

## 重大结论修正：涂抹 = 编码问题，不是引擎问题

- 用户目检：goose 硬件光流版 yuv420p+无 metadata 有"涂抹"；**yuv444p + bt709/tv metadata 版涂抹消失**
- **三个引擎（RIFE/Nano/硬件光流）的"涂抹"评价全部含编码污染**——之前基于涂抹的"RIFE 不可用"结论需要重新审视（RIFE 内容可能没问题，只是编码）
- 编码正确姿势：输出视频必须显式 `-colorspace bt709 -color_range tv`（与源一致）+ 尽量 yuv444p/10bit（4:2:0 色度下采样是涂抹来源之一）
- 预处理已验证无问题（ffmpeg rawvideo 提取 full range 正确，与 PNG 逐字节一致）

## 硬件光流（NVOFA）关键事实

- **RTX 5060 Ti (Blackwell) 支持**（驱动 610.57.04 自带 libnvidia-opticalflow.so.1，API 5.0）
- 入口：`NvOFAPICreateInstanceCuda(NV_OF_API_VERSION, &fl)`（**必须用 SDK 5.0 头文件**——GitHub 版是 API 2.0，结构错位导致 INVALID_PARAM）
- **perfLevel 必须显式设置**（UNDEFINED=0 无效 → NV_OF_ERR_INVALID_PARAM）
- 性能：**0.63ms/对**（864×480，grid1）——比 RIFE 13.4ms 快 21×
- 质量：方块 60px 位移检测 54.5px；真实视频与模板匹配一致（goose 帧间位移本来就 2-3px）
- 能力：Vulkan VK_NV_optical_flow 扩展 + CUDA API 均可用；grid 1x1/2x2/4x4（CUDA 查询 0x1，Vulkan 查询 0x7）
- ABGR8 字节序：**小端 = byte0=R,1=G,2=B,3=A**（曾红蓝互换）
- SDK 5.0.7 在 Optical_Flow_SDK_5.0.7.zip（用户下载目录），解压 /tmp/ofsdk/（含 AppOFCuda 示例、NvOFInterface 头文件）

## fruc_gpu.cu（未来 vf_fruc 核心，/tmp/nvofa/）

- 全 GPU 管线：NVOFA 光流（fwd+bwd）→ CUDA warp（3×3 flow 平滑）→ blend（静止区平均/运动区 fwd warp/遮挡区 bwd warp，flow<0.5px 阈值）
- stdin 帧对（ABGR8）→ stdout mid 帧（RGB24）
- 性能 ~4ms/对（全片 9158 对 ~2 分钟）
- 已修复：ABGR 字节序（颜色）、静止区混合权重（w0w=1 曾导致静止区输出原帧而非平均）、temporal hints 启用、3×3 flow 平滑、flow 幅度阈值

## 当前状态（v2，2026-08-08）

- ✅ 颜色正确、涂抹消失（444 编码后）
- ✅ **横纹异常帧全片扫描 0 处**（阈值 p99>7：21 → 5 → 0）
- 横纹修复链（注意：v1 前记忆称"hints+平滑已应用"**不实**——磁盘代码全部 `disableTemporalHints=1` 且 warp 无平滑，是这次才真正生效）：
  1. temporal hints 启用（流式连续帧对适用）→ 21→5
  2. 3×3 flow box 平滑（smooth_flow_kernel，warp+一致性共用）→ 同上
  3. **遮挡回退帧平均**（occ→avg 而非 occ→bwd warp）→ 5→0。证据：极端运动（±90-130px flow）两 warp 均不可靠（误差 0.12-0.21），bwd warp 产出行跳变垃圾；avg 是唯一安全项（最多淡入淡出，无硬边）
- t_eff 0.384（median 0.370）——比 v1 低是极端运动帧变 cross-fade 所致，正常帧无回归（t=90/150 v2≈v1 且 rowdiff 更低）
- 剩余已知限制：极端运动（单帧位移 >~40px，全片个别帧）插值=安全十字淡入淡出而非真实中间帧——硬件光流跟踪上限，可接受

## 产物

- 视频：/tmp/goose_of_out/goose_merge_60_v2.mp4（最新，待用户目检）+ goose_merge_60_hints444.mp4（v1）+ goose_merge_60_444.mp4（更旧）+ goose_merge_60.mp4（yuv420p 旧版）
- 程序：/tmp/nvofa/fruc_gpu.cu、flow_stream.c、flow_batch.c、test_flow.c、vk_of_test.c
- 管线：/tmp/nvofa/goose_full_444.py（ffmpeg→fruc_gpu→ffmpeg 全流式）
- RIFE 相关：/tmp/rife_community_full/rife_merge_60.mp4（yuv420p 旧编码）、rife_wf16 引擎

## 关联

- [[fruc-research-conclusion]]：OFA FRUC 库不可用的旧结论（仅针对老 FRUC 库，硬件光流本身可用）
- [[rife-model-selection]]：RIFE 4.25 调研 + 涂抹修正后需重新评估

## NVIDIA 官方 FRUC 生态现状（2026-08-08 查证，含来源）

- **Optical Flow SDK 最后版本 = 5.0（2023-02）**，无 6.x；FRUC 编程指南页脚 "Last updated on Feb 27, 2023"（docs.nvidia.com/video-technologies/optical-flow-sdk/nvfruc-programming-guide/）。5.0 只加 Vulkan 光流接口，**FRUC 库未重编**（cubin 仍是 sm_75/sm_80 = SDK 4.0 时代产物）→ RTX 40（CC 8.x）能跑（cubin 同 major 兼容）、**RTX 50（CC 12.0）必然崩**（无 PTX 无 JIT）——与黑盒实测（Process SIGSEGV）一致
- **视频播放插帧无官方新方案**：RTX Video Enhancement 只做超分；插帧是 2025-07 社区 feature request（forums.developer.nvidia.com/t/...337981）未实现；Maxine VFX SDK 0.7.7（2025-06）功能列表无插帧；NVIDIA 插帧投入全部在游戏侧（DLSS Smooth Motion，2025 驱动级，非视频）
- **结论：官方"flow→插值帧"库永久停更，Blackwell 上官方硬件插帧唯一路径 = NVOFA 出 flow + 自建 warp/blend**（或转 RIFE 社区方案）

## v4 官方参数组合（2026-08-08 晚）

- **v3 → v4 参数链**：grid1+FAST+3×3box(21→5→0 offenders) → v3 occ>25% 整帧重复回退（0 offenders, repeats 17.6%）→ **v4 = grid4+SLOW+bilinear 块上采样**（官方参数，0 offenders, repeats 9.5%, t_eff 0.364 vs 0.306）
- **官方参数证据**：libNvOFFRUC 初始化日志 "Optical Flow Grid Size: 4"；SDK 文档 NVOFA_Application_Note Table 3（Ada 4×4 SLOW Fl-all=17.26 vs FAST=23.48）；编程指南 §8.3 temporal hints 保持默认开启；FRUC 指南 §2.2 流水线 = 一致性校验→稀疏→稠密化填充→warp→图像域洞填充
- **官方库 Blackwell 不可用根因（三层证据，2026-08-08 重验）**：
  1. `cuobjdump --list-elf libNvOFFRUC.so`（SDK 自带 /tmp/ofsdk/.../NvOFFRUCSample/bin/ubuntu/ 与 third_party/nvoffruc/lib/ 同源）：32 个 ELF 段**全部 sm_75/sm_80 cubin，无 PTX**
  2. cubin 精确架构绑定（sm_80 只能 CC 8.x）；RTX 5060 Ti = CC 12.0（nvidia-smi compute_cap=12.0）；无 PTX 无 JIT → 必然无法加载
  3. **黑盒实证（/tmp/fruc_process_test.cu）**：NvOFFRUCCreate → SUCCESS（只初始化硬件 OFA，驱动级，**不加载 cubin——"Create 成功"会误导**）；RegisterResource → SUCCESS；**NvOFFRUCProcess 第一次调用 → SIGSEGV**（内部 cubin 加载/执行崩溃）。**修正旧结论："kernel 静默失败→全黑"实为 Process 直接崩溃**
  - libNvOFFRUC.so 依赖 libcudart.so.11.0（bin/ubuntu 只有 .11.6.55，需软链）
- **横纹残留候选（若 v4 目检仍有）**：① FlowInfillOneDir 缺失（valid flow 未传播进 rejected 区，avg 双曝光）② EdgeAwareFlowUpscale 缺失（bilinear 跨边缘 bleed）——两者均为库内真实 kernel（strings 证实）
- v4 产物：/tmp/goose_of_out/goose_merge_60_v4.mp4；扫描器：scan_bands.py（列分带，检测局部条带）+ scan_compare.py（v3/v4 分布对比）

## v5 阈值提升 + 架构结论（2026-08-08 深夜）

- **劣化帧量化度量**：dev = (d_p + d_n) − d_src（raw 域，无编码噪声）；重复帧 dev=0 天然豁免；dev>3.0 → 整帧回退 prev。v4 残留 ~6% dev>3 劣化帧（用户目检确认）
- **v5 = 边缘感知上采样（guide=prev 帧颜色相似性权重，替代 bilinear）+ 像素级 occ→prev 回退（替代 avg 双曝光）+ dev>3 整帧回退**：repeats 9.5→15.5%，t_eff 0.364→0.337，dev>3 存留=0，扫描 0 offenders
- **FlowInfill 对 gather warp 无意义（实证结论）**：官方 scatter warp 有洞才需 infill；gather warp 每像素取色无洞。infill 实测把运动区 flow 填成背景值 → blend 一致性检查全灭 → t_eff 崩到 0.095 → 已移除。vf_fruc 集成禁止用 infill
- **质量回退两阶段**：bad>25%（occ 像素占比）→ 回退；dev>3.0 → 回退。回退帧 stats 计入 dev=0 桶
- **调试教训**：metrics 必须在 mid 确定（cudaMemcpy/回退拷贝）之后计算——v5/v5.1 曾因 mid 错位一帧导致回退决策全错（t_eff 0.095）
- v5 产物：/tmp/goose_of_out/goose_merge_60_v5.mp4

## v8 横纹根因闭环（2026-08-09）

- **横纹真凶 = blend 权重（stat=|flow| 函数）的块级台阶**，非 y 位移错位：行均值 FFT 证明 4px 周期亮度结构（pair 3381 hf=85515），而 flow.y 量级仅 ~0.006px（8-bit 量化吞掉任何 y 平滑）
- **证据链**：2×2 grid 使 pair 3381 hf 降 22×（台阶更密更小）但全局 hits 11→71（块平均去噪弱化）；mag 平滑（smooth_mag_kernel，guide 颜色权重 y 方向 1D）→ hits 11→3，85515 帧消失，repeats/t_eff 不变
- **v8 最终管线**：grid4 SLOW → 边缘感知上采样 → smooth_y（y 位移平滑，8-bit 下无可见效果但保留）→ smooth_mag（|flow| 平滑，blend stat 用）→ occ mask → gather warp → blend（occ→prev 像素回退，无 avg）→ bad>25% 或 dev>3 整帧回退
- **教训**：v7 无效的根因是编辑时残留旧调用链（upsample→occ→warp→blend 用未平滑 flow 覆盖新结果）——输出逐字节相同是"kernel 没生效"的最强信号，先查调用链再怀疑量化
- v8 产物：/tmp/goose_of_out/goose_merge_60_v8.mp4（剩 3 帧微横纹：t≈132/211/266s，ratio 2-3）

## NVOFA 引擎尺寸限制 + 60fps 慢放验证（2026-08-08 晚）

- **NVOFA ≥1024 宽引擎 flow 噪声（引擎限制，非内容）**：~~960×540 正常（bad 1.5%）、1024×576 噪声（21.4%）、1280×720 噪声（28.4%）~~ **——此结论已推翻（2026-08-08 官方模式验证后）**：当时噪声 = race condition（缺 SetIOCudaStreams）+ 逐对池 buffer 误用 + **布局错位**的混合。官方批模式 + 正确布局下 1280×720 引擎 900 对 flow 健康（连续、无退化、无 GENERIC）。**720p 引擎可用（用户核心关切已闭合）；"尺寸限制"是错误结论，勿再引用**
- **AV1 高细节内容（猫毛）额外噪声**：blur 3×3 有效（sd 5.85→0.02，静态场景）；非刚性毛发运动 blur 无效——内容物理
- **hints 结论修正**：时间序列必须开（编程指南 §8.3）；DeepStream 帖的"关 hints"建议是"重复喂同帧"测试场景，已向用户澄清。官方默认 disableTemporalHints=FALSE（NvOF.h:210）；实验显示 hints 开关对回退率影响小（57% vs 49%）
- **heya 60fps 慢放验证基本通过**：60fps 源插帧→60fps 0.5× 慢放，repeats 13.0%、t_eff 0.342、扫描 0 hits，用户目检"没什么太大问题"。**但用户澄清：横纹并未完全消失，只是降到勉强可接受的程度——残余横纹为已知问题，后续再处理（2026-08-08 记录，勿引用"横纹已解决"）**。产物 /tmp/goose_of_out/heya_slow60_v1.mp4（+ _720.mp4 放大版）
- **排查教训**：bash 管道喂 rgb24 给 ABGR 读取器 → 帧错位 → garbage flow——所有直连管道诊断必须先核对字节/像素格式；pkill -f 匹配自身命令行（exit 144）需分离执行

## 官方模式验证 + stride 布局发现（2026-08-08，tests/fruc/）

- **sm_120 FRUC 内核完成（commit f1eb574，用户确认"相当完美"）**：tests/fruc/fruc_of_sm120.cu——官方 NVOFA 批模式（BOTH flow, SLOW, grid4, hints 默认）+ FRUC 后处理（densify→smooth_y→smooth_mag→occ→warp 0.5→blend，occ 回退=warp0）。输入 ABGR 帧流 stdin → mid RGB24 stdout；run_sm120.py 线程化管线（队列深度>批大小16 防管道死锁）；run_sm120_png.py PNG 序列。**1 分钟视频 18s 生成（302 帧 3s）**
- **densify 直读驱动 flow buffer（stride）与官方 DownloadData 路径输出 md5 逐字节相同**——stride 直读无错位，两种路径等价（后者保留为官方语义）
- **occ 回退从"prev 原始像素"改为"warp0（prev+0.5fwd）"**——原始 prev 在运动区呈块状错位感；warp0 连续（用户验证）
- 官方 SDK 5.0.7 源码迁入 tests/fruc/official/（原样+版权，commit e712092）；实验代码 tests/fruc/fruc_of_flow.cu / fruc_of_interp.cu / run_*.py（commit fdf37b7）
- **官方批模式验证通过**：16 输入+15 输出 buffer（nvOFCreateGPUBufferCuda）、每对全新 memset ein/eout（NvOF.cpp:183-197）、批内连续 execute 无逐对 sync（AppOFCuda.cpp:39-83）、批边界 swap(input[15],input[0])（:357）——720p 900 对 exe_fail=0；批模式 vs 逐对模式质量完全相同（441/900 回退一致）
- **🔑 stride 布局（最大发现）**：驱动 CUDEVICEPTR buffer 是 **stride 填充布局**——854×480 输入 stride=3584（W*4=3416，错 168B/行）、720p flow stride=1536（ow*4=1280，错 256B/行）、1080p flow stride=2048。**raw 线性读写全部错位**（运动噪声图案实证：仅"stride 上传+stride 下载"组合 flow=3.000±0.006 完美）。修复：官方上传/下载保持 stride 官方方式；后处理 kernel 读线性副本（host 帧 HtoD + 官方 DownloadData 后 HtoD）。**v8 时代 480p 质量好的原因=错位输入+错位 kernel 读自洽（854 宽错位恰被容忍），非正确**
- **"渐进退化"重判定**：heya_720p 前 200 对（安静）官方模式回退 12.5% ≈ v8 历史 13%；全片 70% 回退随内容剧烈段（ds>8 占比）增长——**内容是主因，非引擎退化**。v8 的"13%"只覆盖安静段
- **CUDA 12 编译**：third_party/cuda12/（gitignored 下载物，redist tar.xz：cuda_cudart 12.9.79 + cuda_nvcc 12.9.86）——12 头 cuCtxCreate 3 参数签名，官方 SDK 零适配编译；nvcc 13.3 + -I 12 头也可（13.3 支持 gcc16，12.9 nvcc 不支持）
- **踩坑记录**：CUDA_CHECK 宏 `return 1` 在 lambda 内 → lambda 推断 int 返回类型 → 结尾无 return UB（实测 sync "失败" code=0）——lambda 内禁用 CUDA_CHECK；cuMemcpy2DAsync D2D 的 device pitch 有对齐约束（854 宽 3416B 非法）→ 逐行拷贝或走 host；cuMemcpyAsync 需 4 参数（driver API 带 stream）；NVOFA 驱动 buffer 不可作 D2D 拷贝源（SIGSEGV）——只能 D2H 或读 devptr
