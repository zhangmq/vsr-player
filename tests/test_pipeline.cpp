/// Full pipeline prototype: Demux → NVDEC → NV12→RGB → save frames.
///
/// Validates the complete decode pipeline end-to-end:
///   1. Demuxer reads packets from container
///   2. Decoder uses av1_nvdec hwaccel (or equivalent for H.264/HEVC)
///   3. GPU frames transferred to CPU via av_hwframe_transfer_data
///   4. NV12→RGB24 conversion via libswscale
///   5. First N frames saved as PPM for visual verification
///
/// Build:
///   mkdir -p build/tests build/output
///   g++ -std=c++20 -O2 -Wall -o build/tests/test_pipeline tests/test_pipeline.cpp \
///       src/core/Demuxer.cpp src/core/Decoder.cpp \
///       $(pkg-config --cflags --libs libavcodec libavformat libavutil libswscale) -lcuda \
///       -Isrc/core -Isrc/core/api
///
/// Run:
///   ./build/tests/test_pipeline input/catlove_720p.webm

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "Demuxer.h"
#include "Decoder.h"

// ── Save RGB24 frame as PPM (binary) ────────────────────────────────

static bool save_ppm(const std::string& path, const uint8_t* rgb,
                     int width, int height, int stride) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror("fopen"); return false; }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; y++) {
        fwrite(rgb + y * stride, 1, width * 3, f);
    }
    fclose(f);
    return true;
}

// ── Main ────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <video_file> [num_frames_to_save]\n", argv[0]);
        return 1;
    }

    const char* path = argv[1];
    int max_save = (argc > 2) ? atoi(argv[2]) : 5;

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  VSR Player — Full Pipeline Prototype       ║\n");
    printf("║  Demux → NVDEC → NV12→RGB                   ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");
    printf("File: %s\n", path);

    // ── 1. Demux ────────────────────────────────────────────────────
    vsr::Demuxer demuxer;
    if (!demuxer.open(path)) {
        fprintf(stderr, "FATAL: Cannot open file\n");
        return 1;
    }
    printf("\n[1/4] Demuxer ready — %dx%d @ %.1ffps\n",
           demuxer.video_width(), demuxer.video_height(), demuxer.video_fps());

    // ── 2. Decoder ─────────────────────────────────────────────────
    vsr::Decoder decoder;
    if (!decoder.open(demuxer.video_codecpar())) {
        fprintf(stderr, "FATAL: Cannot open decoder\n");
        return 1;
    }
    printf("[2/4] Decoder ready — %s\n",
           decoder.is_hardware() ? "NVDEC hwaccel" : "software");

    // ── 3. SwsContext for NV12→RGB24 ───────────────────────────────
    int width  = demuxer.video_width();
    int height = demuxer.video_height();

    SwsContext* sws = sws_getContext(
        width, height, AV_PIX_FMT_NV12,    // src
        width, height, AV_PIX_FMT_RGB24,   // dst
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        fprintf(stderr, "FATAL: Cannot create sws context\n");
        return 1;
    }
    printf("[3/4] NV12→RGB24 converter ready (libswscale)\n");

    // ── 4. Pipeline loop ───────────────────────────────────────────
    printf("[4/4] Running pipeline...\n\n");

    int frame_count   = 0;
    int saved_count   = 0;
    int decode_errors = 0;
    double total_decode_us = 0;
    double total_convert_us = 0;

    // RGB buffer for CPU NV12→RGB conversion
    int rgb_stride = width * 3;
    std::vector<uint8_t> rgb_buf(rgb_stride * height);

    while (true) {
        AVPacket* pkt = demuxer.read_packet();
        if (!pkt) break;  // EOF

        if (pkt->stream_index != demuxer.video_stream_index()) {
            av_packet_free(&pkt);
            continue;
        }

        // Feed packet to decoder
        int64_t pts = pkt->pts;
        if (!decoder.send_packet(pkt->data, pkt->size, pts)) {
            av_packet_free(&pkt);
            decode_errors++;
            continue;
        }
        av_packet_free(&pkt);

        // Receive decoded frames
        while (true) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);

            AVFrame* hw_frame = decoder.receive_frame();
            if (!hw_frame) break;

            clock_gettime(CLOCK_MONOTONIC, &t1);
            double decode_us = (t1.tv_sec - t0.tv_sec) * 1e6 +
                              (t1.tv_nsec - t0.tv_nsec) / 1e3;
            total_decode_us += decode_us;

            frame_count++;
            bool is_cuda = (hw_frame->format == AV_PIX_FMT_CUDA);

            if (frame_count <= 3) {
                printf("  frame %d: format=%s hw_frames=%s\n", frame_count,
                       av_get_pix_fmt_name((AVPixelFormat)hw_frame->format),
                       hw_frame->hw_frames_ctx ? "yes" : "no");
            }

            // GPU→CPU transfer + NV12→RGB conversion
            AVFrame* sw_frame = nullptr;
            if (is_cuda) {
                sw_frame = av_frame_alloc();
                av_hwframe_transfer_data(sw_frame, hw_frame, 0);
            } else {
                sw_frame = av_frame_clone(hw_frame);
            }

            struct timespec t2;
            clock_gettime(CLOCK_MONOTONIC, &t2);

            // NV12→RGB24 via swscale
            uint8_t* dst[1] = { rgb_buf.data() };
            int dst_stride[1] = { rgb_stride };
            sws_scale(sws, sw_frame->data, sw_frame->linesize, 0, height,
                      dst, dst_stride);

            struct timespec t3;
            clock_gettime(CLOCK_MONOTONIC, &t3);
            double convert_us = (t3.tv_sec - t2.tv_sec) * 1e6 +
                               (t3.tv_nsec - t2.tv_nsec) / 1e3;
            total_convert_us += convert_us;

            // Save first N frames as PPM
            if (saved_count < max_save) {
                char fname[256];
                snprintf(fname, sizeof(fname), "build/output/frame_%04d.ppm",
                         saved_count + 1);
                save_ppm(fname, rgb_buf.data(), width, height, rgb_stride);
                printf("  Saved: %s (decode: %.1fms, convert: %.1fms)\n",
                       fname, decode_us / 1000.0, convert_us / 1000.0);
                saved_count++;
            }

            av_frame_free(&sw_frame);
            decoder.release_frame(hw_frame);

            if (frame_count % 300 == 0) {
                double avg_decode = total_decode_us / frame_count / 1000.0;
                printf("  %d frames — avg decode %.2fms, convert %.2fms\n",
                       frame_count, avg_decode,
                       total_convert_us / frame_count / 1000.0);
            }
        }
    }

    // Flush decoder
    decoder.flush();

    // ── 5. Report ──────────────────────────────────────────────────
    double avg_decode  = total_decode_us / frame_count / 1000.0;
    double avg_convert = total_convert_us / frame_count / 1000.0;

    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║  Pipeline Results                            ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  Total frames:    %6d                      ║\n", frame_count);
    printf("║  Saved frames:    %6d                      ║\n", saved_count);
    printf("║  Decode errors:   %6d                      ║\n", decode_errors);
    printf("║  Avg decode:      %6.2f ms                 ║\n", avg_decode);
    printf("║  Avg convert:     %6.2f ms                 ║\n", avg_convert);
    printf("║  Total pipeline:  %6.2f ms                 ║\n",
           avg_decode + avg_convert);
    printf("║  Decoder:         %s                        ║\n",
           decoder.is_hardware() ? "NVDEC hwaccel" : "software");
    printf("╚══════════════════════════════════════════════╝\n");

    // Cleanup
    sws_freeContext(sws);
    decoder.close();

    return 0;
}
