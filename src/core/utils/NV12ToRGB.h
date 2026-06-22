#pragma once

#include <cstddef>
#include <cstdint>

namespace vsr {

/// GPU-side NV12 → RGB float32 conversion via CUDA kernel (NVRTC).
///
/// The kernel is compiled at runtime via NVRTC on first use.
/// Input:  NV12 planes as CUDA device pointers (from NVDEC output).
/// Output: float32 RGB in CHW layout (3 planes of H*W floats), [0, 1] range.
class NV12ToRGB {
public:
    NV12ToRGB();
    ~NV12ToRGB();

    /// Compile the CUDA kernel via NVRTC (called automatically on first convert).
    bool compile();

    /// Convert one frame. All pointers are CUDA device pointers.
    /// @param stream  CUstream (nullptr = default stream)
    bool convert(uint8_t* y_plane, int y_pitch,
                 uint8_t* uv_plane, int uv_pitch,
                 int width, int height,
                 float* rgb_output,
                 void* stream = nullptr);

    /// Get the output buffer size in bytes (3 × H × W × sizeof(float)).
    static size_t output_size(int width, int height) {
        return static_cast<size_t>(width) * height * 3 * sizeof(float);
    }

    bool is_ready() const { return ready_; }

private:
    void* module_ = nullptr;  // CUmodule
    void* kernel_ = nullptr;  // CUfunction
    bool ready_ = false;
};

}  // namespace vsr
