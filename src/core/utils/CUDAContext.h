#pragma once

namespace vsr {

/// RAII wrapper for CUDA context management.
///
/// Two initialization paths:
/// 1. init(device) — create a fresh CUDA context (standalone usage).
/// 2. capture_current() — grab the already-active context (when sharing
///    with FFmpeg's av_hwdevice_ctx_create, which creates its own CUDA
///    context during decoder init).
///
/// Push/pop for thread-local context stack management.
class CUDAContext {
public:
    CUDAContext();
    ~CUDAContext();

    /// Create a new CUDA context on the specified device.
    bool init(int device = 0);

    /// Capture the currently active CUDA context (e.g. from FFmpeg).
    bool capture_current();

    /// Get the raw CUDA context handle (CUcontext).
    void* context() const { return ctx_; }

    /// Push this context onto the current thread's context stack.
    bool push();

    /// Pop the context from the current thread's context stack.
    bool pop();

private:
    void* ctx_ = nullptr;  // CUcontext
};

}  // namespace vsr
