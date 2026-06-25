#pragma once

#include <cstdint>

#include <nvVideoEffects.h>

#include "api/Player.h"

namespace vsr {

/// Wrapper around NvVFX VideoSuperRes C API.
///
/// Manages a VideoSuperRes instance. Input: GPU float32 RGB (3,H,W) [0,1].
/// Output: GPU uint8 RGBA (H',W',4) via CUDA device pointer.
///
/// Reference: NVIDIA-Maxine/VFX-SDK-Samples/apps/VideoEffectsApp/
///   "NGX VSR supports RGBA/BGRA U8 interleaved format on GPU"
class VSRProcessor {
public:
    VSRProcessor();
    ~VSRProcessor();

    /// Initialize with input/output dimensions and quality level.
    bool init(int in_w, int in_h, int out_w, int out_h, int quality);

    /// Process one frame. Input must be a CUDA float32 RGB planar pointer.
    /// Returns the output RGBA CUDA device pointer.
    /// out_pitch: GPU row pitch of output buffer (may be > out_w * 4 due to alignment).
    bool process(void* input_cuda_ptr, void** output_cuda_ptr,
                 int* out_w, int* out_h, int* out_pitch = nullptr);

    /// Reconfigure for new dimensions or quality.
    bool reconfigure(int out_w, int out_h, int quality);

    /// Whether the VSR processor is initialized and ready.
    bool is_ready() const { return vsr_handle_ != nullptr; }

    /// Set external CUDA stream to use (must be called before init()).
    void set_stream(void* stream) { cuda_stream_ = stream; own_stream_ = false; }

    /// Release VSR resources.
    void release();

private:
    NvVFX_Handle vsr_handle_ = nullptr;
    NvCVImage in_img_{};
    NvCVImage out_img_{};
    NvCVImage tmp_img_{};       // temp buffer for NvCVImage_Transfer
    void* cuda_stream_ = nullptr;
    bool own_stream_ = false;
    bool input_allocated_ = false;
    int in_w_ = 0, in_h_ = 0;
    int out_w_ = 0, out_h_ = 0;
    int quality_ = 3;
};

}  // namespace vsr
