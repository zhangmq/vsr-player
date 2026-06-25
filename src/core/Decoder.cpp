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

bool Decoder::hwaccel_logged_ = false;

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

// ── Open (both contexts) ────────────────────────────────────────────

bool Decoder::open(void* codecpar_ptr) {
    AVCodecParameters* codecpar = static_cast<AVCodecParameters*>(codecpar_ptr);
    width_  = codecpar->width;
    height_ = codecpar->height;

    // Create CUDA device context (shared across both contexts)
    if (av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_CUDA,
                               nullptr, nullptr, 0) < 0) {
        fprintf(stderr, "Decoder: CUDA device not available\n");
        hw_device_ctx_ = nullptr;
    }

    // Always try to open software context
    if (!try_open_software(codecpar)) {
        fprintf(stderr, "Decoder: software open failed\n");
        return false;
    }

    // Try hwaccel if CUDA is available
    if (hw_device_ctx_ && try_open_hwaccel(codecpar)) {
        active_hw_ = true;
        fprintf(stderr, "Decoder: dual-context open — %dx%d, hwaccel (NVDEC) active\n",
                width_, height_);
    } else {
        active_hw_ = false;
        fprintf(stderr, "Decoder: dual-context open — %dx%d, SW only (no hwaccel)\n",
                width_, height_);
    }
    return true;
}

// ── Switch active decoder ───────────────────────────────────────────

bool Decoder::switch_to_hw(bool hw) {
    if (active_hw_ == hw) return true;  // no-op

    AVCodecContext* target = hw ? hw_ctx_ : sw_ctx_;
    if (!target) return false;

    // Flush old context (discard buffered frames)
    AVCodecContext* old = active_hw_ ? hw_ctx_ : sw_ctx_;
    if (old) avcodec_flush_buffers(old);

    // Flush new context (in case it has stale data)
    avcodec_flush_buffers(target);

    active_hw_ = hw;
    fprintf(stderr, "Decoder: switched to %s\n", hw ? "NVDEC" : "software");
    return true;
}

// ── HW context helpers ──────────────────────────────────────────────

bool Decoder::try_open_hwaccel(void* codecpar_ptr) {
    AVCodecParameters* codecpar = static_cast<AVCodecParameters*>(codecpar_ptr);
    int codec_id = static_cast<int>(codecpar->codec_id);

    std::vector<const char*> decoders_to_try;
    const char* native_name = avcodec_get_name(static_cast<AVCodecID>(codec_id));
    decoders_to_try.push_back(native_name);

    const AVCodec* default_codec =
        avcodec_find_decoder(static_cast<AVCodecID>(codec_id));
    if (default_codec && strcmp(default_codec->name, native_name) != 0)
        decoders_to_try.push_back(default_codec->name);

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

    hw_ctx_ = avcodec_alloc_context3(codec);
    if (!hw_ctx_) return false;

    avcodec_parameters_to_context(hw_ctx_, codecpar);
    hw_ctx_->get_format = decoder_get_hw_format;
    hw_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);

    int ret = avcodec_open2(hw_ctx_, codec, nullptr);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        fprintf(stderr, "Decoder: hw avcodec_open2 failed: %s\n", err);
        avcodec_free_context(&hw_ctx_);
        return false;
    }

    fprintf(stderr, "Decoder: hwaccel init — decoder=%s hwaccel=%s\n",
            codec->name, hwaccel_name);
    return true;
}

bool Decoder::try_open_software(void* codecpar_ptr) {
    AVCodecParameters* codecpar = static_cast<AVCodecParameters*>(codecpar_ptr);
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) return false;

    sw_ctx_ = avcodec_alloc_context3(codec);
    if (!sw_ctx_) return false;
    avcodec_parameters_to_context(sw_ctx_, codecpar);

    int ret = avcodec_open2(sw_ctx_, codec, nullptr);
    if (ret < 0) {
        avcodec_free_context(&sw_ctx_);
        return false;
    }
    fprintf(stderr, "Decoder: software codec=%s\n", codec->name);
    return true;
}

// ── Send / Receive (active context) ─────────────────────────────────

bool Decoder::send_packet(const uint8_t* data, int size, int64_t pts) {
    AVCodecContext* ctx = active_hw_ ? hw_ctx_ : sw_ctx_;
    if (!ctx) return false;
    AVPacket* pkt = av_packet_alloc();
    pkt->data = const_cast<uint8_t*>(data);
    pkt->size = size;
    pkt->pts  = pts;
    int ret = avcodec_send_packet(ctx, pkt);
    av_packet_free(&pkt);
    return ret >= 0;
}

AVFrame* Decoder::receive_frame() {
    AVCodecContext* ctx = active_hw_ ? hw_ctx_ : sw_ctx_;
    if (!ctx) return nullptr;
    AVFrame* frame = av_frame_alloc();
    int ret = avcodec_receive_frame(ctx, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return nullptr;
    }

    // Log hwaccel activation on first decoded frame
    if (active_hw_ && ctx->hwaccel && !hwaccel_logged_) {
        hwaccel_logged_ = true;
        fprintf(stderr, "Decoder: hwaccel active — %s, pix_fmt=%s (%d)\n",
                ctx->hwaccel->name,
                av_get_pix_fmt_name((AVPixelFormat)frame->format),
                frame->format);
    }

    return frame;
}

void Decoder::release_frame(AVFrame* frame) {
    av_frame_free(&frame);
}

// ── Flush / Close ───────────────────────────────────────────────────

void Decoder::flush() {
    AVCodecContext* ctx = active_hw_ ? hw_ctx_ : sw_ctx_;
    if (ctx) avcodec_flush_buffers(ctx);
}

void Decoder::close() {
    if (hw_ctx_) { avcodec_free_context(&hw_ctx_); hw_ctx_ = nullptr; }
    if (sw_ctx_) { avcodec_free_context(&sw_ctx_); sw_ctx_ = nullptr; }
    if (hw_device_ctx_) { av_buffer_unref(&hw_device_ctx_); hw_device_ctx_ = nullptr; }
    active_hw_ = false;
    hwaccel_logged_ = false;
}

bool Decoder::is_hardware() const { return active_hw_ && hw_ctx_ != nullptr; }
bool Decoder::has_hardware() const { return hw_ctx_ != nullptr; }

int Decoder::active_codec_id() const {
    AVCodecContext* ctx = active_hw_ ? hw_ctx_ : sw_ctx_;
    return ctx ? ctx->codec_id : 0;
}

const char* Decoder::pix_fmt_name() const {
    if (active_hw_) return "cuda";
    return sw_ctx_ ? av_get_pix_fmt_name(sw_ctx_->pix_fmt) : nullptr;
}

}  // namespace vsr
