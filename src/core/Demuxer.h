#pragma once

#include <cstdint>
#include <string>

namespace vsr {

/// FFmpeg-based demuxer. Opens a media container and provides stream access.
class Demuxer {
public:
    Demuxer();
    ~Demuxer();

    /// Open a media file. Returns false on failure.
    bool open(const std::string& path);

    /// Close current file.
    void close();

    /// Video stream properties.
    int video_width() const { return width_; }
    int video_height() const { return height_; }
    double fps() const { return fps_; }
    int64_t duration_ms() const { return duration_ms_; }

    /// Seek to position in milliseconds.
    bool seek(int64_t ms);

private:
    void* format_ctx_ = nullptr;  // AVFormatContext*
    int video_stream_idx_ = -1;
    int audio_stream_idx_ = -1;
    int width_ = 0;
    int height_ = 0;
    double fps_ = 0.0;
    int64_t duration_ms_ = 0;
};

}  // namespace vsr
