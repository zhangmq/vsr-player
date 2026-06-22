#include "Decoder.h"

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace vsr {

// ── get_format callback — requests CUDA hardware frames ─────────────

static AVPixelFormat decoder_get_hw_format(AVCodecContext*,
                                           const AVPixelFormat* pix_fmts) {
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_CUDA) return AV_PIX_FMT_CUDA;
    }
    return pix_fmts[0];
}

// ── Constructor / Destructor ────────────────────────────────────────

Decoder::Decoder() = default;

Decoder::~Decoder() { close(); }

// ── Open ────────────────────────────────────────────────────────────

bool Decoder::open(void* codecpar_ptr) {
    AVCodecParameters* codecpar = static_cast<AVCodecParameters*>(codecpar_ptr);
    width_  = codecpar->width;
    height_ = codecpar->height;

    // Create CUDA device context (shared across all codecs)
    int ret = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_CUDA,
                                     nullptr, nullptr, 0);
    if (ret < 0) {
        fprintf(stderr, "Decoder: CUDA device not available, using SW decode\n");
    }

    // Try hwaccel path first
    if (hw_device_ctx_ && try_open_hwaccel(codecpar)) {
        is_hw_ = true;
        fprintf(stderr, "Decoder: open ok — %dx%d, hwaccel (NVDEC)\n",
                width_, height_);
        return true;
    }

    // Fall back to software
    if (try_open_software(codecpar)) {
        fprintf(stderr, "Decoder: open ok — %dx%d, software\n",
                width_, height_);
        return true;
    }
    return false;
}

bool Decoder::try_open_hwaccel(void* codecpar_ptr) {
    AVCodecParameters* codecpar = static_cast<AVCodecParameters*>(codecpar_ptr);
    int codec_id = static_cast<int>(codecpar->codec_id);
    // Build a list of decoder names to try. The first one with CUDA
    // hwaccel support wins. Order: native name → default decoder.
    // For AV1: "av1" has av1_nvdec, but "libdav1d" (the default) doesn't.
    std::vector<const char*> decoders_to_try;

    // Native decoder name (e.g., "av1", "h264", "hevc", "vp9")
    const char* native_name = avcodec_get_name(static_cast<AVCodecID>(codec_id));
    decoders_to_try.push_back(native_name);

    // Default decoder for this codec_id (may differ, e.g., libdav1d for AV1)
    const AVCodec* default_codec =
        avcodec_find_decoder(static_cast<AVCodecID>(codec_id));
    if (default_codec && strcmp(default_codec->name, native_name) != 0) {
        decoders_to_try.push_back(default_codec->name);
    }

    const AVCodec* codec = nullptr;
    const char* hwaccel_name = nullptr;
    for (const char* name : decoders_to_try) {
        const AVCodec* c = avcodec_find_decoder_by_name(name);
        if (!c) continue;
        for (int i = 0;; i++) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(c, i);
            if (!cfg) break;
            if (cfg->device_type == AV_HWDEVICE_TYPE_CUDA) {
                codec = c;
                hwaccel_name = av_hwdevice_get_type_name(AV_HWDEVICE_TYPE_CUDA);
                break;
            }
        }
        if (codec) break;
    }

    if (!codec) return false;

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;

    // Copy codec parameters (extradata etc. — REQUIRED for hwaccel init)
    avcodec_parameters_to_context(codec_ctx_, codecpar);
    codec_ctx_->get_format = decoder_get_hw_format;
    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);

    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        fprintf(stderr, "Decoder: avcodec_open2 failed: %s\n", err);
        avcodec_free_context(&codec_ctx_);
        return false;
    }

    // Note: hwaccel is set after the first frame is decoded, not here.
    // get_format was called — CUDA was requested — hwaccel will activate.
    fprintf(stderr, "Decoder: hwaccel init — decoder=%s hwaccel=%s "
            "(activates on first frame)\n",
            codec->name, hwaccel_name);
    return true;
}

bool Decoder::try_open_software(void* codecpar_ptr) {
    AVCodecParameters* codecpar = static_cast<AVCodecParameters*>(codecpar_ptr);
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) return false;

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;
    avcodec_parameters_to_context(codec_ctx_, codecpar);

    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        avcodec_free_context(&codec_ctx_);
        return false;
    }
    fprintf(stderr, "Decoder: software codec=%s\n", codec->name);
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

    // Log hwaccel activation on first decoded frame
    if (codec_ctx_->hwaccel && is_hw_) {
        static bool hwaccel_logged = false;
        if (!hwaccel_logged) {
            hwaccel_logged = true;
            fprintf(stderr, "Decoder: hwaccel active — %s, pix_fmt=%s (%d)\n",
                    codec_ctx_->hwaccel->name,
                    av_get_pix_fmt_name((AVPixelFormat)frame->format),
                    frame->format);
        }
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
