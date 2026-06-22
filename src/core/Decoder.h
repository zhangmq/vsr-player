#pragma once

#include <cstdint>

namespace vsr {

/// NVDEC hardware decoder via FFmpeg cuvid.
///
/// Uses hw_device_ctx for proper CUDA surface management — this is the fix
/// for the av1_cuvid duplicate-frame bug identified in the Python prototype.
class Decoder {
public:
    Decoder();
    ~Decoder();

    /// Initialize decoder with video dimensions and codec parameters.
    bool init(void* codec_params, int width, int height);

    /// Decode a packet. Returns true if a frame was produced.
    /// On success, frame data is on GPU as NV12 (CUDA device pointers).
    bool decode(void* packet);

    /// Get the decoded frame's CUDA device pointer and pitch for a plane.
    /// plane=0: Y, plane=1: interleaved UV.
    uint8_t* plane_data(int plane) const;
    int plane_pitch(int plane) const;

    /// Decoded frame PTS in microseconds.
    int64_t frame_pts_us() const;

    /// Flush decoder buffers (e.g., after seek).
    void flush();

private:
    void* codec_ctx_ = nullptr;     // AVCodecContext*
    void* hw_device_ctx_ = nullptr; // AVBufferRef* — CUDA device context
    void* hw_frames_ctx_ = nullptr; // AVBufferRef* — CUDA frames context

    int width_ = 0;
    int height_ = 0;
};

}  // namespace vsr
