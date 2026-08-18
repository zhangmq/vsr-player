---
name: nano-investigation-status
description: Open-Frame-gen(Nano) 调查最终结论：发布权重=屏幕捕获训练的"帧平均器"（论文配方与代码不符）；用户目检大色块不可接受；Nano 不适合 30fps 视频插帧

# Nano（Open-Frame-gen）调查最终结论（2026-08-08 修正 + 2026-08-12 三轮复核终版）

## 2026-08-12 最终复核（用户两次质疑后三轮验证，结论确证 + 证据升级）

- **结论（终版，不可辩驳）**：Nano 无插值能力——**flow 恒零（决定性证据）**：
  - flow 分解（PyTorch return_flow，dx=32/80/160px）：**flow max 0.15-0.22px、mean 0.009-0.010px、|f|>1px 占比 0.0%**——模型完全没学到光流
  - warp(img, flow*t)=恒等 → 输出 = `img0*mask + img1*(1-mask)` = **mask 加权 blend**（mask 空间变化、偏向 B 帧；旧记 mask 值域 [0.24,0.55] 一致）
- **方法学审计（用户质疑点）**：
  - 官方路径核对 ✓：ui_overlay.py 预处理（BGRA→RGB、/255.0、cat[prev,curr] 顺序、t=0.5 CUDA Graph 固化）与测试逐项一致，无误用
  - **mean diff 0.0077 是弱判据**（小位移下 RIFE 对照也 0.0074，全局平均被背景稀释）——当时的表述欠佳，但 flow≈0 的证据当时已存在（std 0.016px），本次独立复现（0.009-0.010px）
  - **位移档位盲区**（首轮只测 40/80/160px）：补测 dx=4/8/16/32/64px 后，边缘位置判据表面"像插值"（dx=32: [177,275] vs 插值[161,271]）——**这是加权平均的形状巧合**（A∩B 重叠区恒 1.0 + mask 偏 B 削弱非重叠区 → 亮区收窄成"像居中圆"），flow 分解证明非插值
  - **t 注入"失效"是 flow≈0 的下游推论**（flow=0 时 warp 对 t 不敏感），非独立缺陷；官方 infer_tensor 的 t 参数根本未被使用（CUDA Graph replay 固化 t=0.5）
- **流程纪律（本次沉淀）**：判定"引擎行为"时，**flow/mask 中间量分解是唯一决定性判据**；mean diff / 边缘位置 / 质心都可能被加权平均的形状巧合欺骗
- **兜底价值：维持否决**（flow=0 → 任何位移都只有加权平均/重影）
- 测试脚本 /tmp/nano_recheck.py（首轮）、/tmp/nano_recheck2.py（小位移档）；产物 /tmp/nano_recheck_out/

## 结论：Nano 发布权重 = 两帧平均器（数值证据独立成立）；**但"色块"目检证据含 4:2:0 编码污染嫌疑（2026-08-08 修正）**

- **修正点**：当时判"色块不可接受"的视频为 yuv420p 编码——而"色块 = 4:2:0 编码污染"是 2026-08-08 在 NVOFA 路线上实证的（PNG 序列零瑕疵 vs 420 视频色块）。Nano 的"色块"目检**需 444 编码复核**；但模型=平均器的判定基于数值（mean diff 0.0077/flow≈0/mask≈0.5），独立于视觉证据——**平均器的双重曝光重影是真实缺陷，444 编码不会消除**（重影 vs 编码色块需区分）
- 教训：与 RIFE/NVOFA 同——**视觉瑕疵先排除编码（444/PNG 对照）再归因模型**；Nano 的"帧平均器"判定本身未被推翻

## 决定性证据链

1. **模型真实行为 = 两帧平均器**：Nano 输出 vs (a+b)/2 帧平均的 mean diff **仅 0.0077**；位移敏感性≈0（1×/2× 位移偏差 0.0057/0.0062）→ 模型=平均，双重曝光重影 = 视觉"大色块"
2. **flow≈0 固有**（std 0.016px）+ **mask 值域 [0.24,0.55]≈0.5** → 输出 = 0.5·img0+0.5·img1
3. **论文（IEEE JCSSE 2026, DOI 10.1109/jcsse68839.2026.11597073）与发布代码不一致**：
   - 论文训练：Vimeo-90K+UCF101+Middlebury、L1+TV+**Laplacian pyramid loss**（λlap=1.0）、100 epochs
   - 代码 training.ipynb：**屏幕捕获数据**（dataset/raw/images + 相机 log.csv）、L1+VGG perceptual+TV、80 epochs——**Laplacian loss 在代码中不存在**
   - → 发布权重 v17_best.pth（与 v16.onnx 一致，ORT/pth 交叉验证过）是屏幕捕获配方的产物：小位移数据 + flow 无监督 → 平均是最优解
4. **pipeline 无 bug**：加载 100 keys 0 missing、e1 权重 0.177、set_timestamp 生效、官方 40% 路径 t_eff 0.537（帧序正确）、ORT/pth 一致 1e-3、无系统色偏/无块状伪影（输出比源更平滑）
5. **仓库真实性**：论文真实（IEEE Xplore 11597073，PES University 4 作者），README 会议名有 typo（JSCCE→JCSSE）；stars=1 无社区验证

## 技术发现（即使不用 Nano 也有价值）

- **TRT 11.2.1.2 图级 bug**：flow 来自图内 head 分支时全图输出错 0.15（所有局部子网/常量 flow 全图正确；优化等级 0-3、TF32 无关）——绕不过，TRT 集成 Nano 需换引擎（ORT CUDA EP 验证一致 1e-3）
- **官方 nano_v16.onnx 违反 ONNX 规范**：GridSample input F16/grid F32（T1≠T2）→ TRT 严格拒绝解析；ORT 宽容可跑且数值正确（mean 1e-3）——"数值正确但不符合规范"，不能说"坏"
- 官方 Python UI：40% downsample 直出（不 upsample）；C++ 引擎 80-100% + sharpness 0.6
- RIFE 对照：真插值（t_eff 0.185/0.75 精确）、内容干净（用户确认两个序列没问题）、1080p tile 串行性能不可行

## 用户目检反馈

- nano_merge_60.mp4（官方 40% 路径全片）：**帧序对**（t_eff 0.537），但**"色块很严重，完全不在可接受范围"**
- 用户多次质疑"是否用错"——已用完整证据链回应：没用错，是权重训练配方的质量边界

## 后续选项（未决策）

1. 用论文配方（Vimeo90K + Laplacian loss）重新训练 Nano——需数据/时间/GPU，论文数字（PSNR 28.4）可信度待验证
2. 放弃 Nano 回 RIFE——720p 性能需重新评估（1080p tile 串行无解）
3. 找作者确认权重来源（GitHub issue）
4. 其他引擎（OFA FRUC 等）

## 环境/产物

- 源帧：/tmp/goose_full/（9159 帧 854×480 30fps）
- 全片视频：/tmp/nano_redo_full/nano_merge_60.mp4（用户已看，色块不可接受）
- 脚本：nano_redo_smoke.py / nano_redo_full.py / nano_debug_run.py / nano_export_fixed.py / nano_export_fp32.py / nano_onnx_check.py / nano_trt_fp32_check.py（/tmp）
- ONNX：nano_fixed.onnx / nano_fp32.onnx / nano_debug.onnx / nano_fp32.onnx（/tmp）
- vfi env：torch 2.12+cu130、onnxruntime-gpu 1.28、tensorrt 11.2.1.2
- 论文文本：/tmp/ofg_paper.txt
