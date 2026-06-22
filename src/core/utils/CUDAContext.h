#pragma once

namespace vsr {

/// RAII wrapper around CUDA context management.
/// Ensures the CUDA context is properly pushed/popped.
class CUDAContext {
public:
    CUDAContext();
    ~CUDAContext();

    /// Initialize CUDA context on the specified device ordinal.
    bool init(int device = 0);

    /// Get the raw CUDA context handle.
    void* context() const { return ctx_; }

    /// Push this context to the current thread.
    bool push();

    /// Pop the context from the current thread.
    bool pop();

private:
    void* ctx_ = nullptr;  // CUcontext
};

}  // namespace vsr
