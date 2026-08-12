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
//
// Engines are dynamic-shape full builds ([1,7,PH,PW] in, [1,3,PH,PW] out,
// FP16) — shapes and dtype are read from the engine file itself. Legacy
// fixed-shape engines are still accepted.
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
    int ph = 0, pw = 0;      // current input spatial dims
    int max_ph = 0, max_pw = 0;  // profile max (dynamic) / built dims (fixed)
    int in_c = 0;            // input channel count (engine)
    int in_elems = 0;        // in_c*max_ph*max_pw (buffers sized at max)
    int out_elems = 0;       // 3*max_ph*max_pw
    bool half = false;       // input dtype FP16?
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

    // Read dims/dtype from the plan. Dynamic-shape engines (built with
    // min<max profile) report -1 — use the profile max for buffers.
    nvinfer1::Dims d_in = e->engine->getTensorShape("input");
    nvinfer1::Dims d_out = e->engine->getTensorShape("output");
    if (d_in.nbDims != 4 || d_out.nbDims != 4 ||
        d_in.d[0] != 1 || d_out.d[0] != 1 || d_out.d[1] != 3) {
        if (log) log(2, "rife_trt: unexpected engine shape (want [1,C,PH,PW]/[1,3,PH,PW])");
        dlclose(e->dl);
        delete e;
        return nullptr;
    }
    e->in_c = d_in.d[1];
    e->half = e->engine->getTensorDataType("input") == nvinfer1::DataType::kHALF;
    if (d_in.d[2] == -1) {
        // dynamic: buffers sized at profile max; current shape starts at opt
        nvinfer1::Dims mx = e->engine->getProfileShape(
            "input", 0, nvinfer1::OptProfileSelector::kMAX);
        nvinfer1::Dims op = e->engine->getProfileShape(
            "input", 0, nvinfer1::OptProfileSelector::kOPT);
        e->max_ph = mx.d[2];
        e->max_pw = mx.d[3];
        e->ph = op.d[2];
        e->pw = op.d[3];
    } else {
        // fixed-shape engine (legacy builds): dims are both current and max
        if (d_in.d[2] != d_out.d[2] || d_in.d[3] != d_out.d[3]) {
            if (log) log(2, "rife_trt: unexpected engine shape (want [1,C,PH,PW]/[1,3,PH,PW])");
            dlclose(e->dl);
            delete e;
            return nullptr;
        }
        e->max_ph = e->ph = d_in.d[2];
        e->max_pw = e->pw = d_in.d[3];
    }
    e->in_elems = e->in_c * e->max_ph * e->max_pw;
    e->out_elems = 3 * e->max_ph * e->max_pw;
    size_t in_bytes = e->in_elems * (e->half ? 2 : 4);
    size_t out_bytes = e->out_elems * (e->half ? 2 : 4);

    // device buffers (allocated on the context current at load time — the
    // filter pushes its CUDA context before calling rife_engine_load)
    if (cudaMalloc(&e->d_in, in_bytes) != cudaSuccess ||
        cudaMalloc(&e->d_out, out_bytes) != cudaSuccess) {
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

int rife_engine_height(struct rife_engine *e)
{
    return e ? e->ph : 0;
}

int rife_engine_width(struct rife_engine *e)
{
    return e ? e->pw : 0;
}

int rife_engine_max_height(struct rife_engine *e)
{
    return e ? e->max_ph : 0;
}

int rife_engine_max_width(struct rife_engine *e)
{
    return e ? e->max_pw : 0;
}

bool rife_engine_set_shape(struct rife_engine *e, int h, int w)
{
    if (!e) return false;
    if (h == e->ph && w == e->pw)
        return true;
    if (h <= 0 || w <= 0 || h > e->max_ph || w > e->max_pw)
        return false;
    nvinfer1::Dims d;
    d.nbDims = 4;
    d.d[0] = 1;
    d.d[1] = e->in_c;
    d.d[2] = h;
    d.d[3] = w;
    if (!e->ctx->setInputShape("input", d))
        return false;
    e->ph = h;
    e->pw = w;
    return true;
}

bool rife_engine_half(struct rife_engine *e)
{
    return e ? e->half : false;
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
