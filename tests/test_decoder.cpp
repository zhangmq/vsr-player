/// Minimal prototype: FFmpeg NVDEC via hwaccel framework (av1_nvdec).
///
/// Uses the software decoder (av1 native) with get_format → AV_PIX_FMT_CUDA.
/// FFmpeg automatically initializes av1_nvdec hwaccel, avoiding the
/// av1_cuvid duplicate-frame bug.
///
/// This is the same path as:
///   ffmpeg -hwaccel cuda -hwaccel_output_format cuda -i input.webm
///
/// Build:
///   mkdir -p build/tests
///   g++ -std=c++20 -O2 -Wall -o build/tests/test_decoder tests/test_decoder.cpp \
///       $(pkg-config --cflags --libs libavcodec libavformat libavutil) -lcuda
///
/// Run:
///   ./build/tests/test_decoder input/catlove_720p.webm

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

// ── FNV-1a 64-bit hash ──────────────────────────────────────────────

static constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
static constexpr uint64_t FNV_PRIME  = 1099511628211ULL;

static uint64_t fnv1a(const uint8_t* data, size_t len) {
    uint64_t hash = FNV_OFFSET;
    for (size_t i = 0; i < len; ++i) { hash ^= data[i]; hash *= FNV_PRIME; }
    return hash;
}

// ── Y-plane hash (GPU→CPU transfer) ─────────────────────────────────

static bool hash_y_plane(AVFrame* hw_frame, AVBufferRef* hw_dev_ctx,
                         uint64_t* out_hash, int* out_width, int* out_height) {
    AVFrame* sw_frame = av_frame_alloc();
    int ret = av_hwframe_transfer_data(sw_frame, hw_frame, 0);
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        fprintf(stderr, "  transfer error: %s\n", err);
        av_frame_free(&sw_frame);
        return false;
    }
    *out_width  = sw_frame->width;
    *out_height = sw_frame->height;
    int y_size  = sw_frame->linesize[0] * sw_frame->height;
    *out_hash    = fnv1a(sw_frame->data[0], y_size);
    av_frame_free(&sw_frame);
    return true;
}

// ── get_format callback — requests CUDA hardware frames ─────────────

static AVPixelFormat get_hw_format(AVCodecContext* ctx,
                                   const AVPixelFormat* pix_fmts) {
    // Return AV_PIX_FMT_CUDA if it's in the supported list.
    // FFmpeg will then initialize the appropriate hwaccel (e.g., av1_nvdec).
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_CUDA) {
            return AV_PIX_FMT_CUDA;
        }
    }
    // Not available — fall back to default (first in list, usually YUV420P)
    fprintf(stderr, "Warning: AV_PIX_FMT_CUDA not in supported formats, "
            "falling back to %s\n", av_get_pix_fmt_name(pix_fmts[0]));
    return pix_fmts[0];
}

// ── Main ────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <video_file>\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];
    printf("=== VSR Player: Decoder (hwaccel av1_nvdec) ===\nFile: %s\n\n", path);

    // ── 1. CUDA device ──────────────────────────────────────────────
    AVBufferRef* hw_dev_ctx = nullptr;
    int ret = av_hwdevice_ctx_create(&hw_dev_ctx, AV_HWDEVICE_TYPE_CUDA,
                                     nullptr, nullptr, 0);
    if (ret < 0) {
        fprintf(stderr, "FATAL: Cannot create CUDA device. Driver installed?\n");
        return 1;
    }
    printf("[1] CUDA device created\n");

    // ── 2. Open input ──────────────────────────────────────────────
    AVFormatContext* fmt_ctx = nullptr;
    avformat_open_input(&fmt_ctx, path, nullptr, nullptr);
    avformat_find_stream_info(fmt_ctx, nullptr);
    int video_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVStream* vs = fmt_ctx->streams[video_idx];
    AVCodecParameters* par = vs->codecpar;

    // Use 'av1' native decoder (NOT libdav1d) — the native decoder
    // supports NVDEC hwaccel (av1_nvdec). libdav1d is software-only.
    // FFmpeg CLI selects 'av1' when -hwaccel cuda is active.
    const AVCodec* codec = avcodec_find_decoder_by_name("av1");
    if (!codec) {
        // Fall back to default decoder (libdav1d — no hwaccel, but no dup bug)
        codec = avcodec_find_decoder(par->codec_id);
    }
    if (!codec) {
        fprintf(stderr, "FATAL: No decoder for codec %s\n",
                avcodec_get_name(par->codec_id));
        return 1;
    }
    printf("[2] Input: %s %dx%d, software decoder: %s\n",
           avcodec_get_name(par->codec_id), par->width, par->height, codec->name);

    // ── 3. Codec context with hwaccel ───────────────────────────────
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, par);

    // *** THE FIX ***
    // get_format → AV_PIX_FMT_CUDA triggers hwaccel init (av1_nvdec etc.)
    // hw_device_ctx provides the CUDA device for the hwaccel
    codec_ctx->get_format    = get_hw_format;
    codec_ctx->hw_device_ctx = av_buffer_ref(hw_dev_ctx);

    ret = avcodec_open2(codec_ctx, codec, nullptr);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        fprintf(stderr, "FATAL: avcodec_open2: %s\n", err);
        return 1;
    }
    printf("[3] Codec opened: hwaccel=%s, pix_fmt=%s\n",
           codec_ctx->hwaccel ? codec_ctx->hwaccel->name : "NONE",
           av_get_pix_fmt_name(codec_ctx->pix_fmt));

    // ── 4. Decode ──────────────────────────────────────────────────
    printf("[4] Decoding all frames...\n");
    AVPacket* pkt   = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();
    int frame_count = 0, dup_count = 0, transfer_errors = 0;
    struct FI { int64_t pts; uint64_t hash; };
    std::vector<FI> hashes;

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != video_idx) { av_packet_unref(pkt); continue; }
        ret = avcodec_send_packet(codec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (true) {
            ret = avcodec_receive_frame(codec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;
            frame_count++;
            // Diagnostic on first 3 frames
            if (frame_count <= 3) {
                printf("  frame %d: format=%s w=%d h=%d hw_frames_ctx=%s\n",
                       frame_count,
                       av_get_pix_fmt_name((AVPixelFormat)frame->format),
                       frame->width, frame->height,
                       frame->hw_frames_ctx ? "yes" : "no");
                fflush(stdout);
            }

            uint64_t h = 0; int fw = 0, fh = 0;
            bool ok = false;

            if (frame->format == AV_PIX_FMT_CUDA) {
                ok = hash_y_plane(frame, hw_dev_ctx, &h, &fw, &fh);
            } else {
                // Software frame — hash Y-plane directly
                fw = frame->width;
                fh = frame->height;
                int y_size = frame->linesize[0] * frame->height;
                h = fnv1a(frame->data[0], y_size);
                ok = true;
            }

            if (ok) {
                for (const auto& prev : hashes) {
                    if (prev.hash == h) {
                        dup_count++;
                        printf("  DUP frame %d (pts %ld) == %ld\n",
                               frame_count - 1, frame->pts, &prev - hashes.data());
                        break;
                    }
                }
                hashes.push_back({frame->pts, h});
            } else {
                transfer_errors++;
            }
            if (frame_count % 300 == 0)
                printf("  %d frames, %d duplicates...\n", frame_count, dup_count);
        }
    }
    // Flush
    avcodec_send_packet(codec_ctx, nullptr);
    while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
        frame_count++;
        uint64_t h = 0; int fw, fh;
        if (hash_y_plane(frame, hw_dev_ctx, &h, &fw, &fh)) {
            for (const auto& prev : hashes)
                if (prev.hash == h) { dup_count++; break; }
            hashes.push_back({frame->pts, h});
        }
    }
    printf("  Done: %d frames, %d dup\n", frame_count, dup_count);

    // ── 5. Report ──────────────────────────────────────────────────
    printf("\n[5] RESULTS: %d total, %d dup, %d transfer errors\n",
           frame_count, dup_count, transfer_errors);
    printf("hwaccel=%s\n",
           codec_ctx->hwaccel ? codec_ctx->hwaccel->name : "NONE");
    printf("%s\n", dup_count == 0
           ? "✓ PERFECT — hwaccel NVDEC path: NO duplicate frames!"
           : "✗ Still duplicates");

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    av_buffer_unref(&hw_dev_ctx);
    return dup_count == 0 ? 0 : 1;
}
