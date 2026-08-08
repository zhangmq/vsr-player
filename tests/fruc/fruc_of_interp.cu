/* FRUC offline, official-mode: NVOFA batch pattern (AppOFCuda.cpp) + v8
 * warp/blend synthesis (validated offline, /tmp/nvofa/fruc_gpu.cu v8).
 *
 * NVOFA call sequence is line-by-line the official SDK 5.0.7 pattern:
 *   - 16 input + 15 output buffers (nvOFCreateGPUBufferCuda)
 *   - upload via cuMemcpy2DAsync + strideInfo pitch (NvOFCuda.cpp:205-229)
 *   - batch loop: upload -> curFrameIdx==15||lastSet -> batch execute ->
 *     swap(input[15], input[0]) (AppOFCuda.cpp:294-361)
 *   - batch execute: sync(inStream) -> consecutive nvOFExecute, NO per-pair
 *     sync -> sync(outStream) (AppOFCuda.cpp:39-83 measureFPS variant)
 *   - per-pair fresh memset ein/eout (NvOF.cpp:183-197) plus the official
 *     bwdOutputBuffer field (predDirection=BOTH) for the consistency check
 *   - disableTemporalHints = NV_OF_FALSE (official default, NvOF.h:210)
 *
 * stdin:  ABGR8 frames (W*H*4 each)
 * stdout: RGB24 interpolated mid frames (W*H*3 each)
 * stderr: fallback/dev statistics
 * Usage: fruc_of_interp <W> <H> [maxframes]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <algorithm>
#include <cuda.h>
#include <cuda_runtime.h>
#include "official/nvOpticalFlowCommon.h"
#include "official/nvOpticalFlowCuda.h"

#define CHECK(x) do { NV_OF_STATUS _s = (x); if (_s != NV_OF_SUCCESS) { \
    fprintf(stderr, "FAIL %s line %d status=%d\n", #x, __LINE__, _s); return 1; } } while (0)
#define CUDA_CHECK(x) do { CUresult _s = (x); if (_s != CUDA_SUCCESS) { \
    fprintf(stderr, "CUDA FAIL %s line %d (code=%d)\n", #x, __LINE__, (int)_s); return 1; } } while (0)

/* ── v8 synthesis kernels (validated) ──────────────────────────────────── */

#define OCC_BAD 0.5f

/* Edge-aware block-level flow upsampled to dense (official library's
 * EdgeAwareFlowUpscale analog): guide = prev frame, weights by color
 * similarity — flow does not bleed across object edges. */
__global__ void upsample_flow_kernel(const short2 *blk, short2 *dense,
                                     const uchar4 *guide, int W, int H, int G) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= W || y >= H) return;
    int gw = W / G, gh = H / G;
    float gx = ((float)x + 0.5f) / G - 0.5f, gy = ((float)y + 0.5f) / G - 0.5f;
    int bx = (int)floorf(gx), by = (int)floorf(gy);
    float fx = gx - bx, fy = gy - by;
    int bx0 = min(max(bx, 0), gw - 1), bx1 = min(bx0 + 1, gw - 1);
    int by0 = min(max(by, 0), gh - 1), by1 = min(by0 + 1, gh - 1);
    short2 a = blk[by0 * gw + bx0], b = blk[by0 * gw + bx1];
    short2 c = blk[by1 * gw + bx0], d = blk[by1 * gw + bx1];
    uchar4 pc = guide[y * W + x];
    float wa[4], wsum = 0;
    float2 cen[4] = { make_float2((bx0 + 0.5f) * G - 0.5f, (by0 + 0.5f) * G - 0.5f),
                      make_float2((bx1 + 0.5f) * G - 0.5f, (by0 + 0.5f) * G - 0.5f),
                      make_float2((bx0 + 0.5f) * G - 0.5f, (by1 + 0.5f) * G - 0.5f),
                      make_float2((bx1 + 0.5f) * G - 0.5f, (by1 + 0.5f) * G - 0.5f) };
    short2 fv[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) {
        int cx = (int)fminf(fmaxf(cen[i].x, 0), W - 1);
        int cy = (int)fminf(fmaxf(cen[i].y, 0), H - 1);
        uchar4 gc = guide[cy * W + cx];
        float dc = (fabsf(pc.x - gc.x) + fabsf(pc.y - gc.y) + fabsf(pc.z - gc.z)) / 3.0f;
        wa[i] = fmaxf(1.0f - dc / 48.0f, 0.02f);
        wsum += wa[i];
    }
    float sx = 0, sy = 0;
    for (int i = 0; i < 4; i++) {
        sx += fv[i].x * wa[i]; sy += fv[i].y * wa[i];
    }
    sx /= wsum; sy /= wsum;
    dense[y * W + x] = make_short2((short)sx, (short)sy);
}

/* Forward-backward consistency mask: occ = 1 where flow unreliable. */
__global__ void occ_kernel(const short2 *fwd, const short2 *bwd, unsigned char *occ, int W, int H) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= W || y >= H) return;
    float2 f = make_float2((float)fwd[y * W + x].x / 32.0f, (float)fwd[y * W + x].y / 32.0f);
    int xf = min(max((int)(x + f.x), 0), W - 1);
    int yf = min(max((int)(y + f.y), 0), H - 1);
    float2 bb = make_float2((float)bwd[yf * W + xf].x / 32.0f, (float)bwd[yf * W + xf].y / 32.0f);
    float o = (fabsf(f.x + bb.x) + fabsf(f.y + bb.y) - 1.0f) / 4.0f;
    occ[y * W + x] = (o > OCC_BAD) ? 1 : 0;
}

/* Row-structure smoothing of the dense flow Y component (edge-aware). */
__global__ void smooth_y_kernel(const short2 *flow, short2 *out,
                                const uchar4 *guide, int W, int H, int radius) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= W || y >= H) return;
    uchar4 pc = guide[y * W + x];
    float sy = 0, wsum = 0;
    for (int dy = -radius; dy <= radius; dy++) {
        int yy = min(max(y + dy, 0), H - 1);
        uchar4 gc = guide[yy * W + x];
        float dc = (fabsf(pc.x - gc.x) + fabsf(pc.y - gc.y) + fabsf(pc.z - gc.z)) / 3.0f;
        float w = fmaxf(1.0f - dc / 24.0f, 0.05f);
        sy += flow[yy * W + x].y * w;
        wsum += w;
    }
    out[y * W + x] = make_short2(flow[y * W + x].x, (short)(sy / wsum));
}

/* Blend-weight continuity: smooth |flow| along y (edge-aware). */
__global__ void smooth_mag_kernel(const short2 *flow, short *mag_out,
                                  const uchar4 *guide, int W, int H, int radius) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= W || y >= H) return;
    uchar4 pc = guide[y * W + x];
    float sm = 0, wsum = 0;
    for (int dy = -radius; dy <= radius; dy++) {
        int yy = min(max(y + dy, 0), H - 1);
        short2 f = flow[yy * W + x];
        float mag = sqrtf((float)f.x * f.x + (float)f.y * f.y);
        uchar4 gc = guide[yy * W + x];
        float dc = (fabsf(pc.x - gc.x) + fabsf(pc.y - gc.y) + fabsf(pc.z - gc.z)) / 3.0f;
        float w = fmaxf(1.0f - dc / 24.0f, 0.05f);
        sm += mag * w;
        wsum += w;
    }
    mag_out[y * W + x] = (short)(sm / wsum);
}

/* Forward sample: out(x,y) = bilinear(img, x+fx*k, y+fy*k), border=replicate */
__global__ void warp_kernel(const uchar4 *img, const short2 *flow,
                            float4 *out, int W, int H, float k) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= W || y >= H) return;
    float2 f = make_float2((float)flow[y * W + x].x * (k / 32.0f), (float)flow[y * W + x].y * (k / 32.0f));
    float sx = fminf(fmaxf(x + f.x, 0.0f), W - 1.0f);
    float sy = fminf(fmaxf(y + f.y, 0.0f), H - 1.0f);
    int x0 = (int)sx, y0 = (int)sy;
    int x1 = min(x0 + 1, W - 1), y1 = min(y0 + 1, H - 1);
    float wx = sx - x0, wy = sy - y0;
    uchar4 a = img[y0 * W + x0], b = img[y0 * W + x1];
    uchar4 c = img[y1 * W + x0], d = img[y1 * W + x1];
    float4 top, bot;
    top.x = a.x * (1 - wx) + b.x * wx;
    top.y = a.y * (1 - wx) + b.y * wx;
    top.z = a.z * (1 - wx) + b.z * wx;
    bot.x = c.x * (1 - wx) + d.x * wx;
    bot.y = c.y * (1 - wx) + d.y * wx;
    bot.z = c.z * (1 - wx) + d.z * wx;
    out[y * W + x] = make_float4(top.x * (1 - wy) + bot.x * wy,
                                 top.y * (1 - wy) + bot.y * wy,
                                 top.z * (1 - wy) + bot.z * wy, 0);
}

/* Blend + pixel-level fallback (occ -> prev pixel) + fallback counter.
 * Converts ABGR->RGB (out is uchar3 RGB). */
__global__ void blend_kernel(const float4 *w0, const float4 *w1,
                             const uchar4 *src0, const unsigned char *occmask,
                             const short *mag,
                             uchar3 *out, int *d_bad_count, int W, int H) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= W || y >= H) return;
    if (occmask[y * W + x]) {
        atomicAdd(d_bad_count, 1);
        uchar4 a = src0[y * W + x];
        out[y * W + x] = make_uchar3(a.x, a.y, a.z);
        return;
    }
    float magf = (float)mag[y * W + x] / 32.0f;
    float stat = fminf(fmaxf(1.0f - magf / 2.0f, 0.0f), 1.0f);
    float w0w = 1.0f - 0.5f * stat;
    float w1w = 0.5f * stat;
    float4 m;
    m.x = w0[y * W + x].x * w0w + w1[y * W + x].x * w1w;
    m.y = w0[y * W + x].y * w0w + w1[y * W + x].y * w1w;
    m.z = w0[y * W + x].z * w0w + w1[y * W + x].z * w1w;
    out[y * W + x] = make_uchar3((unsigned char)fminf(fmaxf(m.x, 0), 255),
                                 (unsigned char)fminf(fmaxf(m.y, 0), 255),
                                 (unsigned char)fminf(fmaxf(m.z, 0), 255));
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s W H [maxframes] < ABGR8\n", argv[0]); return 1; }
    const uint32_t W = (uint32_t)atoi(argv[1]);
    const uint32_t H = (uint32_t)atoi(argv[2]);
    const uint32_t maxFrames = argc > 3 ? (uint32_t)atoi(argv[3]) : 0;
    const int G = 4;
    const uint32_t ow = (W + G - 1) / G, oh = (H + G - 1) / G;
    const size_t frameBytes = (size_t)W * H * 4;
    const uint32_t NBUF = 16;                        /* AppOFCuda.cpp:241 */

    /* official main (AppOFCuda.cpp:474-495) */
    CUcontext cuContext = nullptr;
    CUdevice cuDevice = 0;
    CUstream inStream = nullptr, outStream = nullptr;
    CUDA_CHECK(cuInit(0));
    CUDA_CHECK(cuDeviceGet(&cuDevice, 0));
    CUDA_CHECK(cuCtxCreate(&cuContext, 0, cuDevice));
    CUDA_CHECK(cuStreamCreate(&inStream, CU_STREAM_DEFAULT));
    CUDA_CHECK(cuStreamCreate(&outStream, CU_STREAM_DEFAULT));

    /* official API load + create + SetIOCudaStreams (NvOFCuda.cpp:32-51) */
    void *h = dlopen("libnvidia-opticalflow.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "FAIL dlopen\n"); return 1; }
    NV_OF_STATUS (*createInstance)(uint32_t, NV_OF_CUDA_API_FUNCTION_LIST *) =
        (NV_OF_STATUS (*)(uint32_t, NV_OF_CUDA_API_FUNCTION_LIST *))dlsym(h, "NvOFAPICreateInstanceCuda");
    NV_OF_CUDA_API_FUNCTION_LIST fl;
    memset(&fl, 0, sizeof(fl));
    CHECK(createInstance(NV_OF_API_VERSION, &fl));
    NvOFHandle handle;
    CHECK(fl.nvCreateOpticalFlowCuda(cuContext, &handle));
    CHECK(fl.nvOFSetIOCudaStreams(handle, inStream, outStream));

    /* official Init (NvOF.cpp:106-168); predDirection=BOTH for the bwd
     * consistency check (official API field, nvOpticalFlowCommon.h:434) */
    NV_OF_INIT_PARAMS ip;
    memset(&ip, 0, sizeof(ip));
    ip.width = W; ip.height = H;
    ip.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
    ip.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    ip.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    ip.mode = NV_OF_MODE_OPTICALFLOW;
    ip.perfLevel = NV_OF_PERF_LEVEL_SLOW;
    ip.enableExternalHints = NV_OF_FALSE;
    ip.enableOutputCost = NV_OF_FALSE;
    ip.enableRoi = NV_OF_FALSE;
    ip.predDirection = NV_OF_PRED_DIRECTION_BOTH;      /* bwd for occ+v8 warp */
    CHECK(fl.nvOFInit(handle, &ip));

    /* official buffer creation (NvOFCuda.cpp:188-198): driver-allocated */
    NV_OF_BUFFER_DESCRIPTOR inDesc, outDesc;
    memset(&inDesc, 0, sizeof(inDesc));
    inDesc.width = W; inDesc.height = H;
    inDesc.bufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
    inDesc.bufferUsage = NV_OF_BUFFER_USAGE_INPUT;
    memset(&outDesc, 0, sizeof(outDesc));
    outDesc.width = ow; outDesc.height = oh;
    outDesc.bufferFormat = NV_OF_BUFFER_FORMAT_SHORT2;
    outDesc.bufferUsage = NV_OF_BUFFER_USAGE_OUTPUT;

    NvOFGPUBufferHandle inBuf[NBUF], fwdBuf[NBUF - 1], bwdBuf[NBUF - 1];
    CUdeviceptr inPtr[NBUF], fwdPtr[NBUF - 1], bwdPtr[NBUF - 1];
    size_t inStride[NBUF];
    for (uint32_t i = 0; i < NBUF; i++) {
        CHECK(fl.nvOFCreateGPUBufferCuda(handle, &inDesc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &inBuf[i]));
        inPtr[i] = fl.nvOFGPUBufferGetCUdeviceptr(inBuf[i]);
        NV_OF_CUDA_BUFFER_STRIDE_INFO si;
        CHECK(fl.nvOFGPUBufferGetStrideInfo(inBuf[i], &si));
        inStride[i] = si.strideInfo[0].strideXInBytes;
    }
    size_t flowStride = 0;
    for (uint32_t i = 0; i < NBUF - 1; i++) {
        CHECK(fl.nvOFCreateGPUBufferCuda(handle, &outDesc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &fwdBuf[i]));
        fwdPtr[i] = fl.nvOFGPUBufferGetCUdeviceptr(fwdBuf[i]);
        NV_OF_CUDA_BUFFER_STRIDE_INFO si;
        CHECK(fl.nvOFGPUBufferGetStrideInfo(fwdBuf[i], &si));
        flowStride = si.strideInfo[0].strideXInBytes;
        CHECK(fl.nvOFCreateGPUBufferCuda(handle, &outDesc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &bwdBuf[i]));
        bwdPtr[i] = fl.nvOFGPUBufferGetCUdeviceptr(bwdBuf[i]);
    }
    fprintf(stderr, "input stride=%zu (W*4=%zu) flow stride=%zu (ow*4=%u)\n",
            inStride[0], (size_t)W * 4, flowStride, ow * 4);

    /* official upload (NvOFCuda.cpp:205-229): cuMemcpy2DAsync + stride pitch */
    auto upload = [&](uint32_t idx, const uint8_t *data) {
        CUDA_MEMCPY2D cp;
        memset(&cp, 0, sizeof(cp));
        cp.WidthInBytes = W * 4;
        cp.srcMemoryType = CU_MEMORYTYPE_HOST;
        cp.srcHost = data;
        cp.srcPitch = W * 4;
        cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cp.dstDevice = inPtr[idx];
        cp.dstPitch = inStride[idx];
        cp.Height = H;
        CUresult r = cuMemcpy2DAsync(&cp, inStream);
        if (r != CUDA_SUCCESS)
            fprintf(stderr, "upload fail slot %u\n", idx);
    };
    /* flow download (official DownloadData, NvOFCuda.cpp:231-256):
     * cuMemcpy2DAsync D2H with stride pitch + sync(outStream) */
    NV_OF_FLOW_VECTOR *hFwd = (NV_OF_FLOW_VECTOR *)malloc((size_t)ow * oh * sizeof(NV_OF_FLOW_VECTOR));
    NV_OF_FLOW_VECTOR *hBwd = (NV_OF_FLOW_VECTOR *)malloc((size_t)ow * oh * sizeof(NV_OF_FLOW_VECTOR));
    /* NOTE: CUDA_CHECK's `return 1` inside a lambda forces an int return type
     * and reaching the end without return is UB (measured: sync "failed" with
     * code=0). Use explicit checks here. */
    auto downloadFlow = [&](uint32_t idx, bool fwd) -> void {
        CUDA_MEMCPY2D cp;
        memset(&cp, 0, sizeof(cp));
        cp.WidthInBytes = ow * 4;
        cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        cp.srcDevice = fwd ? fwdPtr[idx] : bwdPtr[idx];
        cp.srcPitch = flowStride;
        cp.dstMemoryType = CU_MEMORYTYPE_HOST;
        cp.dstHost = fwd ? hFwd : hBwd;
        cp.dstPitch = ow * 4;
        cp.Height = oh;
        CUresult r1 = cuMemcpy2DAsync(&cp, outStream);
        if (r1 != CUDA_SUCCESS)
            fprintf(stderr, "flow download fail code=%d\n", (int)r1);
        CUresult r2 = cuStreamSynchronize(outStream);
        if (r2 != CUDA_SUCCESS)
            fprintf(stderr, "flow sync fail code=%d\n", (int)r2);
    };

    /* linear copies for the synthesis kernels (driver buffers are
     * stride-padded, e.g. 854x480 -> 3584 B/row vs 3416, flow 1536 vs
     * 1280 B/row at 720p; kernels always read linear data) */
    uint8_t *d_linA = nullptr, *d_linB = nullptr;
    short2 *d_flow_lin = nullptr, *d_flowb_lin = nullptr;
    cudaMalloc(&d_linA, frameBytes);
    cudaMalloc(&d_linB, frameBytes);
    cudaMalloc(&d_flow_lin, (size_t)ow * oh * sizeof(NV_OF_FLOW_VECTOR));
    cudaMalloc(&d_flowb_lin, (size_t)ow * oh * sizeof(NV_OF_FLOW_VECTOR));

    /* synthesis scratch */
    short2 *d_fs, *d_bs, *d_fs2, *d_bs2;
    float4 *d_w0, *d_w1;
    unsigned char *d_occ;
    short *d_mag;
    uchar3 *d_out;
    int *d_bad;
    cudaMalloc(&d_fs, W * H * sizeof(short2));
    cudaMalloc(&d_bs, W * H * sizeof(short2));
    cudaMalloc(&d_fs2, W * H * sizeof(short2));
    cudaMalloc(&d_bs2, W * H * sizeof(short2));
    cudaMalloc(&d_w0, W * H * sizeof(float4));
    cudaMalloc(&d_w1, W * H * sizeof(float4));
    cudaMalloc(&d_occ, W * H);
    cudaMalloc(&d_mag, W * H * sizeof(short));
    cudaMalloc(&d_out, W * H * 3);
    cudaMalloc(&d_bad, sizeof(int));
    dim3 block(32, 8), grid((W + 31) / 32, (H + 7) / 8);

    /* host frame ring (aligned with inBuf; swap together) */
    uint8_t *hostF[NBUF];
    for (uint32_t k = 0; k < NBUF; k++) hostF[k] = (uint8_t *)malloc(frameBytes);
    uint8_t *f0 = (uint8_t *)malloc(frameBytes), *f1 = (uint8_t *)malloc(frameBytes);
    uchar3 *mid = (uchar3 *)malloc(W * H * 3);

    struct d_top { double dev; int i; double dp, dn, ds; int bad; };
    static struct d_top dtop[12];
    static int dev_hist[9] = { 0 };

    int repeats = 0, rep_bad = 0, rep_dev = 0, exeFail = 0;
    uint32_t curFrameIdx = 0, framesRead = 0;
    bool lastSet = false;
    uint64_t pairIndex = 0;

    for (;;) {                                          /* AppOFCuda.cpp:298 */
        if (!lastSet && (maxFrames == 0 || framesRead < maxFrames)) {
            size_t got = fread(hostF[curFrameIdx], 1, frameBytes, stdin);
            if (got == frameBytes) {
                upload(curFrameIdx, hostF[curFrameIdx]);
                curFrameIdx++;
                framesRead++;
            } else {
                if (curFrameIdx == 0) break;
                curFrameIdx--;                          /* AppOFCuda.cpp:321 */
                lastSet = true;
            }
        } else {
            if (curFrameIdx == 0) break;
            curFrameIdx--;
            lastSet = true;
        }

        if (curFrameIdx == NBUF - 1 || lastSet) {       /* AppOFCuda.cpp:325 */
            /* PER-PAIR execute (official guideline #5 pool semantics:
             * round-robin buffers, per-pair execute + sync). Test vs the
             * AppOFCuda batch pattern for flow accuracy. Upload sync per
             * pair keeps the driver's reads ordered. */
            CUDA_CHECK(cuStreamSynchronize(inStream));
            for (uint32_t i = 0; i < curFrameIdx; i++) {
                NV_OF_EXECUTE_INPUT_PARAMS ein;         /* NvOF.cpp:183-189 */
                memset(&ein, 0, sizeof(ein));
                ein.inputFrame = inBuf[i];
                ein.referenceFrame = inBuf[i + 1];
                ein.disableTemporalHints = NV_OF_TRUE;   /* TEST: vs official default FALSE */
                NV_OF_EXECUTE_OUTPUT_PARAMS eout;       /* NvOF.cpp:194-196 + bwd */
                memset(&eout, 0, sizeof(eout));
                eout.outputBuffer = fwdBuf[i];
                eout.bwdOutputBuffer = bwdBuf[i];
                NV_OF_STATUS es = fl.nvOFExecute(handle, &ein, &eout);
                if (es != NV_OF_SUCCESS) {
                    fprintf(stderr, "EXEC FAIL pair %llu status=%d\n",
                            (unsigned long long)pairIndex, es);
                    exeFail++;
                }
                CUDA_CHECK(cuStreamSynchronize(outStream));

                /* synthesis per pair (v8 chain): frames from host copies ->
                 * linear GPU (driver input buffers are stride-padded and not
                 * D2D-copyable); flows via official DownloadData (D2H,
                 * stride) -> H2D linear. upsample fwd/bwd -> smooth_y ->
                 * smooth_mag -> occ -> warp(prev,+0.5fwd)
                 * +warp(cur,+0.5bwd) -> blend. */
                CUDA_CHECK(cuMemcpyHtoD((CUdeviceptr)d_linA, hostF[i], frameBytes));
                CUDA_CHECK(cuMemcpyHtoD((CUdeviceptr)d_linB, hostF[i + 1], frameBytes));
                downloadFlow(i, true);
                downloadFlow(i, false);
                CUDA_CHECK(cuMemcpyHtoD((CUdeviceptr)d_flow_lin, hFwd,
                                        (size_t)ow * oh * sizeof(NV_OF_FLOW_VECTOR)));
                CUDA_CHECK(cuMemcpyHtoD((CUdeviceptr)d_flowb_lin, hBwd,
                                        (size_t)ow * oh * sizeof(NV_OF_FLOW_VECTOR)));
                const uchar4 *gprev = (const uchar4 *)d_linA;
                const uchar4 *gcur = (const uchar4 *)d_linB;
                upsample_flow_kernel<<<grid, block>>>((const short2 *)d_flow_lin, d_fs, gprev, W, H, G);
                upsample_flow_kernel<<<grid, block>>>((const short2 *)d_flowb_lin, d_bs, gprev, W, H, G);
                smooth_y_kernel<<<grid, block>>>(d_fs, d_fs2, gprev, W, H, 2);
                smooth_y_kernel<<<grid, block>>>(d_bs, d_bs2, gprev, W, H, 2);
                smooth_mag_kernel<<<grid, block>>>(d_fs2, d_mag, gprev, W, H, 2);
                occ_kernel<<<grid, block>>>(d_fs2, d_bs2, d_occ, W, H);
                warp_kernel<<<grid, block>>>(gprev, d_fs2, d_w0, W, H, 0.5f);
                warp_kernel<<<grid, block>>>(gcur, d_bs2, d_w1, W, H, 0.5f);
                cudaMemset(d_bad, 0, sizeof(int));
                blend_kernel<<<grid, block>>>(d_w0, d_w1, gprev, d_occ, d_mag,
                                              d_out, d_bad, W, H);
                int bad = 0;
                cudaMemcpy(&bad, d_bad, sizeof(int), cudaMemcpyDeviceToHost);
                cudaMemcpy(mid, d_out, W * H * 3, cudaMemcpyDeviceToHost);

                memcpy(f0, hostF[i], frameBytes);
                memcpy(f1, hostF[i + 1], frameBytes);

                double dp = 0, dn = 0, ds = 0;
                if (bad > (int)(0.25f * W * H)) {
                    for (int p = 0; p < (int)(W * H); p++)
                        mid[p] = make_uchar3(f0[p * 4], f0[p * 4 + 1], f0[p * 4 + 2]);
                    repeats++; rep_bad++;
                } else {
                    unsigned long long sp = 0, sn = 0, ss = 0;
                    for (int p = 0; p < (int)(W * H); p++) {
                        int a0 = f0[p * 4], a1 = f0[p * 4 + 1], a2 = f0[p * 4 + 2];
                        int b0 = f1[p * 4], b1 = f1[p * 4 + 1], b2 = f1[p * 4 + 2];
                        sp += abs(mid[p].x - a0) + abs(mid[p].y - a1) + abs(mid[p].z - a2);
                        sn += abs(mid[p].x - b0) + abs(mid[p].y - b1) + abs(mid[p].z - b2);
                        ss += abs(a0 - b0) + abs(a1 - b1) + abs(a2 - b2);
                    }
                    dp = (double)sp / (3.0 * W * H);
                    dn = (double)sn / (3.0 * W * H);
                    ds = (double)ss / (3.0 * W * H);
                    double dev = dp + dn - ds;
                    if (dev > (ds > 0.5 ? fmax(3.0, 0.25 * ds) : 3.0)) {
                        for (int p = 0; p < (int)(W * H); p++)
                            mid[p] = make_uchar3(f0[p * 4], f0[p * 4 + 1], f0[p * 4 + 2]);
                        repeats++; rep_dev++;
                        dp = dn = ds = 0;
                    }
                }
                fwrite(mid, 3, W * H, stdout);
                fflush(stdout);
                {
                    double dev = dp + dn - ds;
                    int dbk = ds <= 0.5 ? 0 : (dev < 1 ? 1 : dev < 2 ? 2 : dev < 3 ? 3 :
                                dev < 4 ? 4 : dev < 6 ? 5 : dev < 8 ? 6 : dev < 12 ? 7 : 8);
                    dev_hist[dbk]++;
                    if (ds > 0.5 && dev > dtop[0].dev) {
                        dtop[0] = (struct d_top){ dev, (int)pairIndex, dp, dn, ds, bad };
                        for (int k = 0; k < 11 && dtop[k].dev > dtop[k + 1].dev; k++) {
                            struct d_top tmp = dtop[k]; dtop[k] = dtop[k + 1]; dtop[k + 1] = tmp;
                        }
                    }
                }
                if (pairIndex % 100 == 0)
                    fprintf(stderr, "pair %llu (repeats %d, rep_bad %d, rep_dev %d)\n",
                            (unsigned long long)pairIndex, repeats, rep_bad, rep_dev);
                pairIndex++;
            }

            if (lastSet) break;                         /* AppOFCuda.cpp:352-355 */
            std::swap(inBuf[curFrameIdx], inBuf[0]);    /* AppOFCuda.cpp:357 */
            std::swap(inPtr[curFrameIdx], inPtr[0]);
            std::swap(inStride[curFrameIdx], inStride[0]);
            uint8_t *tmp = hostF[0];
            hostF[0] = hostF[curFrameIdx];
            hostF[curFrameIdx] = tmp;
            curFrameIdx = 0;                            /* AppOFCuda.cpp:358 */
        }
    }

    fprintf(stderr, "done (%llu pairs, frames %u, repeats %d, rep_bad %d, rep_dev %d, exe_fail %d)\n",
            (unsigned long long)pairIndex, framesRead, repeats, rep_bad, rep_dev, exeFail);
    fprintf(stderr, "dev histogram (static,<1,<2,<3,<4,<6,<8,<12,>=12):");
    for (int k = 0; k < 9; k++) fprintf(stderr, " %d", dev_hist[k]);
    fprintf(stderr, "\ntop degraded (by dev):");
    for (int k = 11; k >= 0 && dtop[k].dev > 0; k--)
        fprintf(stderr, "\n  pair %d dev=%.2f dp=%.2f dn=%.2f ds=%.2f bad=%d",
                dtop[k].i, dtop[k].dev, dtop[k].dp, dtop[k].dn, dtop[k].ds, dtop[k].bad);
    fprintf(stderr, "\n");
    return 0;
}
