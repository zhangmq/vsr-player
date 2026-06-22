#pragma once

#include <cstdint>
#include <string>

struct AVFormatContext;
struct AVCodecParameters;
struct AVPacket;

namespace vsr {

/// FFmpeg-based demuxer. Opens a media container, discovers streams,
/// and provides packet-level access to video and audio streams.
class Demuxer {
public:
    Demuxer();
    ~Demuxer();

    /// Open a media file. Returns false on failure.
    bool open(const std::string& path);

    /// Close current file and release resources.
    void close();

    /// Read next packet. Caller must free with av_packet_unref().
    /// Returns nullptr on EOF or error.
    AVPacket* read_packet();

    // ── Stream info ─────────────────────────────────────────────────

    int video_stream_index() const { return video_idx_; }
    int audio_stream_index() const { return audio_idx_; }

    int video_codec_id() const { return video_codec_id_; }
    int video_width() const { return video_width_; }
    int video_height() const { return video_height_; }
    double video_fps() const { return video_fps_; }

    int audio_sample_rate() const { return audio_sample_rate_; }
    int audio_channels() const { return audio_channels_; }

    int64_t duration_ms() const { return duration_ms_; }

    /// Time base for PTS conversion (seconds per tick).
    double video_time_base() const { return video_time_base_; }

    /// Video codec parameters (AVCodecParameters*) for decoder init.
    void* video_codecpar() const;

private:
    AVFormatContext* fmt_ctx_ = nullptr;
    int video_idx_ = -1;
    int audio_idx_ = -1;
    int video_codec_id_ = 0;
    int video_width_ = 0;
    int video_height_ = 0;
    double video_fps_ = 0.0;
    double video_time_base_ = 0.0;
    int audio_sample_rate_ = 0;
    int audio_channels_ = 0;
    int64_t duration_ms_ = 0;
};

}  // namespace vsr
