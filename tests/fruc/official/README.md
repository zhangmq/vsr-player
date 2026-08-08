# Official NVIDIA Optical Flow SDK 5.0.7 — 参考源码（只读）

原样复制自 `Optical_Flow_SDK_5.0.7`（NVIDIA 官方，许可见各文件头）。**修改 NVIDIA 调用逻辑 = 对照本目录，禁止自行设计。**

- `AppOFCuda.cpp` — 官方应用框架：批处理模式（`NvOFBatchExecute` + 主循环）
- `NvOF.cpp` / `NvOF.h` / `NvOFDefines.h` — 通用逻辑：Init 参数、Execute 的 ein/eout 构造、buffer desc
- `NvOFCuda.cpp` / `NvOFCuda.h` — CUDA 实现：API 加载、buffer 创建/上传/下载
- `nvOpticalFlowCommon.h` / `nvOpticalFlowCuda.h` — 官方 API 头（结构体、函数指针）
- `NvOFUtils.h` — 工具类头（本实验未用）

## 官方行为准则（迁移必须遵守）

| # | 准则 | 出处 |
|---|------|------|
| 1 | API 创建后**立即** `nvOFSetIOCudaStreams(hOF, inputStream, outputStream)` | NvOFCuda.cpp:50 |
| 2 | 输入/输出 buffer 由**驱动创建**：`nvOFCreateGPUBufferCuda` → `nvOFGPUBufferGetCUdeviceptr` → `nvOFGPUBufferGetStrideInfo`，**不是 cuMemAlloc** | NvOFCuda.cpp:192-197 |
| 3 | 上传/下载用 `cuMemcpy2DAsync`，**pitch = strideInfo.strideXInBytes**（不是 raw width×4） | NvOFCuda.cpp:205-256 |
| 4 | 16 输入 buffer + 15 输出 buffer；批内对 i 用 `(input[i], input[i+1]) → output[i]`，**每个输出 buffer 批内只被一对使用** | AppOFCuda.cpp:241-246, 60-66 |
| 5 | **每对 Execute 构造全新 memset 的 ein/eout**（栈对象），绝不复用 | NvOF.cpp:183-197 |
| 6 | `disableTemporalHints = NV_OF_FALSE`（官方默认——**时间序列开着 hints 跑**） | NvOF.h:210 |
| 7 | 批内连续 execute **无逐对 sync**；measureFPS 模式整批前 `cuStreamSynchronize(inputStream)`、整批后 `cuStreamSynchronize(outputStream)` | AppOFCuda.cpp:58, 67 |
| 8 | 下载在批末逐 output[i] 执行，`DownloadData` 内部 `cuStreamSynchronize(outputStream)` | NvOFCuda.cpp:254 |
| 9 | 批边界 `swap(inputBuffers[15], inputBuffers[0])` —— 上一批最后一帧成为下批第一帧，**连续帧流跨批不中断** | AppOFCuda.cpp:357 |
| 10 | `enableExternalHints=FALSE`、`enableOutputCost=FALSE`、`enableRoi=FALSE` | AppOFCuda.cpp:239, NvOF.cpp:161-167 |

## 我们此前实验与官方的偏离（全部已对照源码确认）

| 偏离点 | 我们 | 官方 |
|--------|------|------|
| 输出 buffer | 每对复用同一 fwdBuf | 每对独立 output[i] |
| ein/eout | 复用（ein2 每对重建是后加的测试改动） | 每对全新 memset |
| sync | 逐对 execute→sync→execute | 批内无 sync，整批前后各一次 |
| disableTemporalHints | 测试中设 TRUE | 官方默认 FALSE |
| 上传 pitch | raw 拷贝 | cuMemcpy2DAsync + strideInfo |
| buffer 创建 | 自建 + 注册 | nvOFCreateGPUBufferCuda |
