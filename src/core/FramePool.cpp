#include "FramePool.h"

#include <cstdlib>

namespace vsr {

FramePool::FramePool() = default;
FramePool::~FramePool() { free_all(); }

bool FramePool::allocate(int count, size_t size_bytes) {
    free_all();
    size_bytes_ = size_bytes;
    slots_.resize(count);
    for (auto& slot : slots_) {
        slot.ptr = std::aligned_alloc(256, size_bytes);  // TODO: use cudaMalloc
        slot.in_use = false;
    }
    return true;
}

void* FramePool::acquire() {
    for (auto& slot : slots_) {
        if (!slot.in_use) {
            slot.in_use = true;
            return slot.ptr;
        }
    }
    return nullptr;
}

void FramePool::release(void* ptr) {
    for (auto& slot : slots_) {
        if (slot.ptr == ptr) {
            slot.in_use = false;
            return;
        }
    }
}

void FramePool::free_all() {
    for (auto& slot : slots_) {
        std::free(slot.ptr);  // TODO: use cudaFree
    }
    slots_.clear();
    size_bytes_ = 0;
}

}  // namespace vsr
