#pragma once

#include <cstddef>
#include <cstdint>

namespace vsr {

/// GPU-side NV12 → RGB float32 conversion via CUDA kernel.
///
/// Input: NV12 planes as CUDA device pointers (from NVDEC output).
/// Output: float32 RGB (3, H, W) normalized [0, 1] on CUDA device.
class NV12ToRGB {
public:
    NV12ToRGB();
    ~NV12ToRGB();

    /// Convert one frame. All pointers are CUDA device pointers.
    bool convert(uint8_t* y_plane, int y_pitch,
                 uint8_t* uv_plane, int uv_pitch,
                 int width, int height,
                 float* rgb_output);

    /// Get the output buffer size in bytes.
    static size_t output_size(int width, int height) {
        return static_cast<size_t>(width) * height * 3 * sizeof(float);
    }
};

}  // namespace vsr
