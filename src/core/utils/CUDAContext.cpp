#include "CUDAContext.h"

#include <cstdio>
#include <cuda.h>

namespace vsr {

CUDAContext::CUDAContext() = default;

CUDAContext::~CUDAContext() {
    if (ctx_) {
        cuCtxDestroy(static_cast<CUcontext>(ctx_));
        ctx_ = nullptr;
    }
}

bool CUDAContext::init(int device) {
    CUresult res = cuInit(0);
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDAContext: cuInit failed (%d)\n", res);
        return false;
    }

    CUdevice cu_dev;
    res = cuDeviceGet(&cu_dev, device);
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDAContext: cuDeviceGet(%d) failed (%d)\n", device, res);
        return false;
    }

    char name[256];
    cuDeviceGetName(name, sizeof(name), cu_dev);
    int major = 0, minor = 0;
    cuDeviceComputeCapability(&major, &minor, cu_dev);

    CUcontext cu_ctx;
    res = cuCtxCreate(&cu_ctx, nullptr, CU_CTX_SCHED_BLOCKING_SYNC, cu_dev);
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDAContext: cuCtxCreate failed (%d)\n", res);
        return false;
    }

    fprintf(stderr, "CUDAContext: GPU %d — %s (compute %d.%d)\n",
            device, name, major, minor);
    ctx_ = cu_ctx;
    return true;
}

bool CUDAContext::capture_current() {
    CUcontext cur;
    CUresult res = cuCtxGetCurrent(&cur);
    if (res != CUDA_SUCCESS || cur == nullptr) {
        fprintf(stderr, "CUDAContext: no current CUDA context\n");
        return false;
    }
    // Retain so we can safely push/pop later
    cuCtxPushCurrent(cur);
    ctx_ = cur;
    return true;
}

bool CUDAContext::push() {
    if (!ctx_) return false;
    return cuCtxPushCurrent(static_cast<CUcontext>(ctx_)) == CUDA_SUCCESS;
}

bool CUDAContext::pop() {
    CUcontext popped;
    return cuCtxPopCurrent(&popped) == CUDA_SUCCESS;
}

}  // namespace vsr
