// rife_trt.cpp — TensorRT engine wrapper (compiled only when HAVE_TRT).
//
// dlopen scheme (NOT linked): libnvinfer.so.11 is loaded with RTLD_LOCAL so
// its symbols never enter the global namespace. VFX SDK ships its own TRT 10
// libraries (loaded RTLD_LOCAL by vsr_proc.c); if TRT 11 were linked globally,
// the two versions' symbols would cross-bind and crash. RTLD_LOCAL keeps both
// versions isolated — each resolves its own NEEDED dependencies.
//
// Entry: createInferRuntime_INTERNAL is an extern "C" symbol; everything else
// (IRuntime/ICudaEngine/IExecutionContext) is vtable-based, so no further
// dlsym is needed. NvInfer.h is used at compile time for types/vtable layout.
#include <cstdio>
#include <dlfcn.h>
#include <fstream>
#include <memory>
#include <vector>

#include <NvInfer.h>
#include <cuda_runtime_api.h>

#include "rife_trt.h"

namespace {

struct Logger : nvinfer1::ILogger {
    rife_log_fn fn;
    explicit Logger(rife_log_fn f) : fn(f) {}
    void log(Severity sev, const char *msg) noexcept override {
        if (!fn) return;
        int s = sev == Severity::kERROR ? 2 : (sev == Severity::kWARNING ? 1 : 0);
        fn(s, msg);
    }
};

} // namespace

struct rife_engine {
    void *dl = nullptr;
    Logger logger{nullptr};
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::unique_ptr<nvinfer1::IExecutionContext> ctx;
    void *d_in = nullptr;
    void *d_out = nullptr;
    int size = 0;
};

extern "C" {

struct rife_engine *rife_engine_load(const char *path, rife_log_fn log)
{
    auto *e = new rife_engine;
    e->logger.fn = log;

    e->dl = dlopen("libnvinfer.so.11", RTLD_NOW | RTLD_LOCAL);
    if (!e->dl) {
        if (log) log(2, "rife_trt: dlopen libnvinfer.so.11 failed");
        delete e;
        return nullptr;
    }
    // extern "C" entry (NvInferRuntime.h): IRuntime* (ILogger*, int32_t version)
    auto *create = reinterpret_cast<void *(*)(void *, int32_t)>(dlsym(e->dl, "createInferRuntime_INTERNAL"));
    if (!create) {
        if (log) log(2, "rife_trt: dlsym createInferRuntime_INTERNAL failed");
        dlclose(e->dl);
        delete e;
        return nullptr;
    }

    // read plan file
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        if (log) log(2, "rife_trt: cannot open engine file");
        dlclose(e->dl);
        delete e;
        return nullptr;
    }
    std::streamsize sz = f.tellg();
    std::vector<char> blob(sz);
    f.seekg(0, std::ios::beg);
    f.read(blob.data(), sz);
    if (!f) {
        if (log) log(2, "rife_trt: engine file read failed");
        dlclose(e->dl);
        delete e;
        return nullptr;
    }

    e->runtime.reset(static_cast<nvinfer1::IRuntime *>(create(&e->logger, NV_TENSORRT_VERSION)));
    if (!e->runtime) {
        if (log) log(2, "rife_trt: createInferRuntime failed");
        dlclose(e->dl);
        delete e;
        return nullptr;
    }
    e->engine.reset(e->runtime->deserializeCudaEngine(blob.data(), blob.size()));
    if (!e->engine) {
        if (log) log(2, "rife_trt: deserializeCudaEngine failed");
        dlclose(e->dl);
        delete e;
        return nullptr;
    }
    e->ctx.reset(e->engine->createExecutionContext());
    if (!e->ctx) {
        if (log) log(2, "rife_trt: createExecutionContext failed");
        dlclose(e->dl);
        delete e;
        return nullptr;
    }

    // fixed static shapes: (1,7,512,512) in, (1,3,512,512) out
    if (!e->ctx->setInputShape("input", nvinfer1::Dims4{1, 7, 512, 512})) {
        if (log) log(2, "rife_trt: setInputShape failed");
        dlclose(e->dl);
        delete e;
        return nullptr;
    }
    e->size = 512;

    // device buffers (allocated on the context current at load time — the
    // filter pushes its CUDA context before calling rife_engine_load)
    if (cudaMalloc(&e->d_in, 7ull * 512 * 512 * sizeof(float)) != cudaSuccess ||
        cudaMalloc(&e->d_out, 3ull * 512 * 512 * sizeof(float)) != cudaSuccess) {
        if (log) log(2, "rife_trt: cudaMalloc failed");
        dlclose(e->dl);
        delete e;
        return nullptr;
    }
    e->ctx->setTensorAddress("input", e->d_in);
    e->ctx->setTensorAddress("output", e->d_out);
    return e;
}

bool rife_engine_run(struct rife_engine *e, void *stream)
{
    if (!e) return false;
    return e->ctx->enqueueV3(static_cast<cudaStream_t>(stream));
}

void *rife_engine_input(struct rife_engine *e)
{
    return e ? e->d_in : nullptr;
}

void *rife_engine_output(struct rife_engine *e)
{
    return e ? e->d_out : nullptr;
}

int rife_engine_size(struct rife_engine *e)
{
    return e ? e->size : 0;
}

void rife_engine_destroy(struct rife_engine *e)
{
    if (!e) return;
    cudaFree(e->d_in);
    cudaFree(e->d_out);
    if (e->dl) dlclose(e->dl);
    delete e;
}

} // extern "C"
