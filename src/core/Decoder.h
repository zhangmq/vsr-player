#pragma once

#include <cstdint>
#include <memory>

struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;

namespace vsr {

/// Dual-context decoder — NVDEC (hwaccel) and software AVCodecContext
/// coexist. Live switching between them without closing/reopening.
///
/// Uses the native decoder + get_format → AV_PIX_FMT_CUDA callback to
/// trigger NVDEC hwaccel (e.g., av1_nvdec, h264_nvdec, hevc_nvdec).
/// Software fallback is always available.
class Decoder {
public:
    Decoder();
    ~Decoder();

    /// Open both hwaccel and software contexts from codec parameters.
    /// @param codecpar  AVCodecParameters* from the demuxer stream
    bool open(void* codecpar);

    /// Switch active decoder. Returns true if the switch is valid.
    /// Both contexts must be open. Caller must seek demuxer afterwards.
    bool switch_to_hw(bool hw);

    /// Feed a compressed packet to the active decoder.
    bool send_packet(const uint8_t* data, int size, int64_t pts);

    /// Receive a decoded frame from the active decoder.
    AVFrame* receive_frame();

    /// Release a frame obtained from receive_frame().
    void release_frame(AVFrame* frame);

    /// Flush active decoder (after seek).
    void flush();

    /// Close both contexts and free resources.
    void close();

    /// True if hardware decode (NVDEC) is currently active.
    bool is_hardware() const;

    /// HW context is open and available (even if not active).
    bool has_hardware() const;

private:
    bool try_open_hwaccel(void* codecpar);
    bool try_open_software(void* codecpar);

    AVCodecContext* hw_ctx_ = nullptr;
    AVCodecContext* sw_ctx_ = nullptr;
    AVBufferRef* hw_device_ctx_ = nullptr;
    bool active_hw_ = true;
    int width_ = 0, height_ = 0;

    static bool hwaccel_logged_;
};

}  // namespace vsr
