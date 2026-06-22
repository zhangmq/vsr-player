#pragma once

#include <cstdint>
#include <memory>

struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;

namespace vsr {

/// FFmpeg NVDEC hardware decoder via hwaccel framework.
///
/// Uses the native decoder + get_format → AV_PIX_FMT_CUDA callback to trigger
/// NVDEC hwaccel initialization (e.g., av1_nvdec, h264_nvdec, hevc_nvdec).
/// This avoids the av1_cuvid duplicate-frame bug — the hwaccel path uses
/// proper NVDEC surface management through FFmpeg's hwcontext framework.
///
/// Decoded frames are AV_PIX_FMT_CUDA (NV12 on GPU, CUDA device pointers).
class Decoder {
public:
    Decoder();
    ~Decoder();

    /// Open decoder for a codec. Uses hwaccel when available, falls
    /// back to software decode.
    bool open(int codec_id, int width, int height);

    /// Feed a compressed packet. Returns true if frames are available.
    bool send_packet(const uint8_t* data, int size, int64_t pts);

    /// Receive a decoded frame. Returns nullptr when no more frames.
    /// Caller must call release_frame() after processing.
    AVFrame* receive_frame();

    /// Release a frame obtained from receive_frame().
    void release_frame(AVFrame* frame);

    /// Flush decoder (after seek).
    void flush();

    /// Close decoder and free resources.
    void close();

    /// True if hardware decode (NVDEC) is active.
    bool is_hardware() const;

private:
    bool try_open_hwaccel(int codec_id, int width, int height);
    bool try_open_software(int codec_id, int width, int height);

    AVCodecContext* codec_ctx_ = nullptr;
    AVBufferRef* hw_device_ctx_ = nullptr;
    int width_ = 0, height_ = 0;
    bool is_hw_ = false;
};

}  // namespace vsr
