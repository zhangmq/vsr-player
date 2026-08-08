// fruc_of_flow — 官方批处理模式离线验证（第一阶段：纯 flow 统计，无后处理）
//
// NVOFA 调用部分逐行对照 official/AppOFCuda.cpp（行号见注释），只替换：
//   - 数据源：stdin ABGR8 帧流（官方用 NvOFDataLoader）
//   - 输出：flow 统计到 stderr（官方写 flow 文件）
//
// 验证目标：
//   1. 官方模式（16 输入 + 15 输出 buffer、每对全新 ein/eout、批内无逐对 sync、
//      disableTemporalHints=FALSE、批边界 swap）在 720p 长时间运行无
//      NV_OF_ERR_GENERIC、无渐进退化
//   2. flow 时间连续性（相邻帧对的 flow 应连续；突变 = 引擎状态异常）
//
// 用法：./fruc_of_flow W H [maxframes] < 帧流(ABGR8, W*H*4/帧)
// 输出：stderr 每对一行统计 + 汇总

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>
#include "official/NvOFCuda.h"

// ============================================================
// 官方 NvOFBatchExecute（AppOFCuda.cpp:39-83，measureFPS=true 变体：
//   整批前 cuStreamSynchronize(inputStream)，整批后 sync(outputStream)，
//   批内连续 execute、无逐对 sync）
// ============================================================
static void BatchExecute(NvOFObj &of,
    std::vector<NvOFBufferObj> &inputBuffers,
    std::vector<NvOFBufferObj> &outputBuffers,
    uint32_t batchSize,
    CUstream inputStream, CUstream outputStream)
{
    cuStreamSynchronize(inputStream);
    for (uint32_t i = 0; i < batchSize; i++)
    {
        // 官方 Execute（NvOF.cpp:183-197）：每对全新 memset 的 ein/eout，
        // disableTemporalHints=NV_OF_FALSE（NvOF.h:210 默认），externalHints=nullptr
        of->Execute(inputBuffers[i].get(),
            inputBuffers[i + 1].get(),
            outputBuffers[i].get());
    }
    cuStreamSynchronize(outputStream);
}

// ============================================================
// 主循环：官方 AppOFCuda.cpp:294-361 结构
// ============================================================
int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s W H [maxframes] < ABGR8 frames\n", argv[0]);
        return 1;
    }
    const uint32_t W = (uint32_t)atoi(argv[1]);
    const uint32_t H = (uint32_t)atoi(argv[2]);
    const uint32_t maxFrames = argc > 3 ? (uint32_t)atoi(argv[3]) : 0;

    const uint32_t GRID = 4;
    const uint32_t ow = (W + GRID - 1) / GRID;
    const uint32_t oh = (H + GRID - 1) / GRID;
    const size_t frameBytes = (size_t)W * H * 4;
    const uint32_t NBUF = 16;                       // AppOFCuda.cpp:241

    // ---- 官方 main（AppOFCuda.cpp:474-495）：cuInit / cuCtxCreate / 双 stream ----
    CUcontext cuContext = nullptr;
    CUdevice cuDevice = 0;
    CUstream inputStream = nullptr, outputStream = nullptr;
    uint64_t pairIndex = 0;         // 全局对索引（catch 分支也要用）
    CUDA_DRVAPI_CALL(cuInit(0));
    CUDA_DRVAPI_CALL(cuDeviceGet(&cuDevice, 0));
    // CUDA 13 起 cuCtxCreate 需 CUctxCreateParams（官方 SDK 面向 CUDA 12；空参数 = 默认）
    CUctxCreateParams ctxParams{};
    CUDA_DRVAPI_CALL(cuCtxCreate(&cuContext, &ctxParams, 0, cuDevice));
    CUDA_DRVAPI_CALL(cuStreamCreate(&inputStream, CU_STREAM_DEFAULT));
    CUDA_DRVAPI_CALL(cuStreamCreate(&outputStream, CU_STREAM_DEFAULT));

    try
    {
        // ---- 官方 EstimateFlow（AppOFCuda.cpp:195-196, 239）----
        NvOFObj nvOpticalFlow = NvOFCuda::Create(cuContext, W, H, NV_OF_BUFFER_FORMAT_ABGR8,
            NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,
            NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,
            NV_OF_MODE_OPTICALFLOW, NV_OF_PERF_LEVEL_SLOW,
            inputStream, outputStream);
        // Init(grid, hintGrid, enableExternalHints=false, enableRoi=false)
        nvOpticalFlow->Init(GRID, NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED, false, false);

        // ---- 官方 buffer 创建（AppOFCuda.cpp:245-246）：16 输入 + 15 输出 ----
        std::vector<NvOFBufferObj> inputBuffers =
            nvOpticalFlow->CreateBuffers(NV_OF_BUFFER_USAGE_INPUT, NBUF);
        std::vector<NvOFBufferObj> outputBuffers =
            nvOpticalFlow->CreateBuffers(NV_OF_BUFFER_USAGE_OUTPUT, NBUF - 1);

        std::vector<uint8_t> frame(frameBytes);
        std::vector<NV_OF_FLOW_VECTOR> flow(ow * oh);
        std::vector<NV_OF_FLOW_VECTOR> prevFlow(ow * oh);

        uint32_t curFrameIdx = 0;                   // AppOFCuda.cpp:294
        bool lastSet = false;
        uint32_t framesRead = 0;

        // 退化统计累积量
        double maxDiff = 0; uint64_t maxDiffPair = 0;
        uint64_t nAbnormal = 0;                     // diff 突变的对
        double abnThreshold = 0;                    // 动态阈值（前 64 对 p90 * 3）

        for (;;)                                    // AppOFCuda.cpp:298
        {
            if (!lastSet && (maxFrames == 0 || framesRead < maxFrames))
            {
                size_t got = fread(frame.data(), 1, frameBytes, stdin);
                if (got == frameBytes)
                {
                    inputBuffers[curFrameIdx]->UploadData(frame.data());   // 官方 302
                    curFrameIdx++;
                    framesRead++;
                }
                else
                {
                    if (curFrameIdx == 0) break;    // 无数据可执行
                    curFrameIdx--;                  // 官方 321：最后一批不足 NBUF
                    lastSet = true;
                }
            }
            else
            {
                if (curFrameIdx == 0) break;
                curFrameIdx--;
                lastSet = true;
            }

            if (curFrameIdx == NBUF - 1 || lastSet) // 官方 325
            {
                BatchExecute(nvOpticalFlow, inputBuffers, outputBuffers,
                             curFrameIdx, inputStream, outputStream);

                // 下载 + 统计（官方 331-345 下载在批末逐 output[i]）
                for (uint32_t i = 0; i < curFrameIdx; i++)
                {
                    outputBuffers[i]->DownloadData(flow.data());   // 官方 340（内部 sync outputStream）

                    // ---- 统计 ----
                    double nonzero = 0, sumDiff = 0;
                    std::vector<float> mags; mags.reserve(ow * oh);
                    for (size_t k = 0; k < (size_t)ow * oh; k++)
                    {
                        float fx = flow[k].flowx / 32.0f;   // SHORT2 缩放（AppOFCuda.cpp:291, 32.0f）
                        float fy = flow[k].flowy / 32.0f;
                        float m = sqrtf(fx * fx + fy * fy);
                        if (m > 0.01f) nonzero++;
                        mags.push_back(m);
                        if (pairIndex > 0)
                        {
                            float dx = (flow[k].flowx - prevFlow[k].flowx) / 32.0f;
                            float dy = (flow[k].flowy - prevFlow[k].flowy) / 32.0f;
                            sumDiff += sqrtf(dx * dx + dy * dy);
                        }
                    }
                    std::nth_element(mags.begin(), mags.begin() + mags.size() / 2, mags.end());
                    double magMedian = mags[mags.size() / 2];
                    double diff = pairIndex > 0 ? sumDiff / ((size_t)ow * oh) : -1;

                    // 动态退化阈值：前 64 对 diff 的 p90 × 3
                    if (pairIndex < 64)
                    {
                        abnThreshold = abnThreshold == 0 ? diff
                            : (diff > abnThreshold ? abnThreshold * 0.9 + diff * 0.1 : abnThreshold);
                    }
                    bool abn = pairIndex >= 64 && diff > abnThreshold * 3 && abnThreshold > 0;
                    if (abn) nAbnormal++;
                    if (diff > maxDiff) { maxDiff = diff; maxDiffPair = pairIndex; }

                    fprintf(stderr,
                        "pair %llu nonzero=%.3f mag_med=%.2f diff_prev=%.2f%s\n",
                        (unsigned long long)pairIndex,
                        nonzero / ((size_t)ow * oh), magMedian, diff,
                        abn ? "  <-- ABNORMAL" : "");

                    std::swap(prevFlow, flow);
                    pairIndex++;
                }

                if (lastSet) break;                 // 官方 352-355
                // 批边界：最后一帧作为下批第一帧（官方 357）
                swap(inputBuffers[curFrameIdx], inputBuffers[0]);
                curFrameIdx = 0;                    // 官方 358
            }
        }

        fprintf(stderr,
            "\nSUMMARY pairs=%llu frames=%u abnormal=%llu max_diff=%.2f@pair%llu\n",
            (unsigned long long)pairIndex, framesRead,
            (unsigned long long)nAbnormal, maxDiff,
            (unsigned long long)maxDiffPair);
    }
    catch (const std::exception &ex)
    {
        fprintf(stderr, "\nEXCEPTION at pair=%llu: %s\n",
                (unsigned long long)pairIndex, ex.what());
        return 1;
    }

    cuStreamDestroy(outputStream);
    cuStreamDestroy(inputStream);
    cuCtxDestroy(cuContext);
    return 0;
}
