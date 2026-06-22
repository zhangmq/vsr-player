#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vsr {

/// Pool of pre-allocated GPU frame buffers. Reuses CUDA memory to avoid
/// per-frame allocation overhead.
class FramePool {
public:
    FramePool();
    ~FramePool();

    /// Pre-allocate N buffers of given size (in bytes).
    bool allocate(int count, size_t size_bytes);

    /// Get a free buffer. Returns nullptr if pool exhausted.
    void* acquire();

    /// Return a buffer to the pool.
    void release(void* ptr);

    /// Free all buffers.
    void free_all();

    size_t buffer_size() const { return size_bytes_; }

private:
    struct Slot {
        void* ptr = nullptr;
        bool in_use = false;
    };
    std::vector<Slot> slots_;
    size_t size_bytes_ = 0;
};

}  // namespace vsr
