#include "Decoder.h"

#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace vsr {

// ── get_format callback — requests CUDA hardware frames ─────────────

static AVPixelFormat decoder_get_hw_format(AVCodecContext* ctx,
                                           const AVPixelFormat* pix_fmts) {
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_CUDA) return AV_PIX_FMT_CUDA;
    }
    // CUDA not available — fall back to first offered format
    return pix_fmts[0];
}

// ── Constructor / Destructor ────────────────────────────────────────

Decoder::Decoder() = default;

Decoder::~Decoder() { close(); }

// ── Open ────────────────────────────────────────────────────────────

bool Decoder::open(int codec_id, int width, int height) {
    width_ = width;
    height_ = height;

    // Create CUDA device context (shared across all codecs)
    int ret = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_CUDA,
                                     nullptr, nullptr, 0);
    if (ret < 0) {
        fprintf(stderr, "Decoder: CUDA device not available, using SW decode\n");
    }

    // Try hwaccel path first
    if (hw_device_ctx_ && try_open_hwaccel(codec_id, width, height)) {
        is_hw_ = true;
        return true;
    }

    // Fall back to software
    return try_open_software(codec_id, width, height);
}

bool Decoder::try_open_hwaccel(int codec_id, int width, int height) {
    // Find the native decoder (NOT libdav1d, NOT _cuvid).
    // The native decoder supports hwaccel (e.g., av1 → av1_nvdec).
    const AVCodec* codec = avcodec_find_decoder(static_cast<AVCodecID>(codec_id));
    if (!codec) return false;

    // Check this codec actually supports CUDA hwaccel
    bool has_cuda_hwaccel = false;
    for (int i = 0;; i++) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) break;
        if (cfg->device_type == AV_HWDEVICE_TYPE_CUDA) {
            has_cuda_hwaccel = true;
            break;
        }
    }
    if (!has_cuda_hwaccel) return false;

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;

    codec_ctx_->width = width;
    codec_ctx_->height = height;
    codec_ctx_->get_format = decoder_get_hw_format;
    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);

    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        avcodec_free_context(&codec_ctx_);
        return false;
    }

    fprintf(stderr, "Decoder: hwaccel=%s active\n",
            codec_ctx_->hwaccel ? codec_ctx_->hwaccel->name : "NONE");
    return true;
}

bool Decoder::try_open_software(int codec_id, int, int) {
    const AVCodec* codec = avcodec_find_decoder(static_cast<AVCodecID>(codec_id));
    if (!codec) return false;

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;

    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        avcodec_free_context(&codec_ctx_);
        return false;
    }
    fprintf(stderr, "Decoder: software decode (codec=%s)\n", codec->name);
    return true;
}

// ── Send / Receive ──────────────────────────────────────────────────

bool Decoder::send_packet(const uint8_t* data, int size, int64_t pts) {
    AVPacket* pkt = av_packet_alloc();
    pkt->data = const_cast<uint8_t*>(data);
    pkt->size = size;
    pkt->pts  = pts;
    int ret = avcodec_send_packet(codec_ctx_, pkt);
    av_packet_free(&pkt);
    return ret >= 0;
}

AVFrame* Decoder::receive_frame() {
    AVFrame* frame = av_frame_alloc();
    int ret = avcodec_receive_frame(codec_ctx_, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    return frame;
}

void Decoder::release_frame(AVFrame* frame) {
    av_frame_free(&frame);
}

// ── Flush / Close ───────────────────────────────────────────────────

void Decoder::flush() {
    if (codec_ctx_) {
        avcodec_flush_buffers(codec_ctx_);
    }
}

void Decoder::close() {
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }
    is_hw_ = false;
}

bool Decoder::is_hardware() const { return is_hw_; }

}  // namespace vsr
