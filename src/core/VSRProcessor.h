#pragma once

#include <cstdint>

#include "api/Player.h"

namespace vsr {

/// Wrapper around NvVFX VideoSuperRes C API.
///
/// Manages a VideoSuperRes instance. Input: GPU float32 RGB (3,H,W) [0,1].
/// Output: GPU uint8 RGBA (H',W',4) via CUDA device pointer.
class VSRProcessor {
public:
    VSRProcessor();
    ~VSRProcessor();

    /// Initialize with input/output dimensions and quality level.
    bool init(int in_w, int in_h, int out_w, int out_h, Quality quality);

    /// Process one frame. Input must be a CUDA float32 RGB tensor.
    /// Returns the output RGBA CUDA device pointer.
    bool process(void* input_cuda_ptr, void** output_cuda_ptr, int* out_w, int* out_h);

    /// Reconfigure for new dimensions or quality.
    bool reconfigure(int out_w, int out_h, Quality quality);

    /// Release VSR resources.
    void release();

private:
    void* vsr_handle_ = nullptr;  // NvVFX handle
    int in_w_ = 0, in_h_ = 0;
    int out_w_ = 0, out_h_ = 0;
    Quality quality_ = Quality::HIGH;
};

}  // namespace vsr
