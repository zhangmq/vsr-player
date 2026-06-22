#include "Demuxer.h"

#include <cstdio>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace vsr {

Demuxer::Demuxer() = default;

Demuxer::~Demuxer() { close(); }

bool Demuxer::open(const std::string& path) {
    close();

    int ret = avformat_open_input(&fmt_ctx_, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        fprintf(stderr, "Demuxer: cannot open %s: %s\n", path.c_str(), err);
        return false;
    }

    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) {
        fprintf(stderr, "Demuxer: cannot find stream info\n");
        close();
        return false;
    }

    // Find best video and audio streams
    video_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    // Extract video info
    if (video_idx_ >= 0) {
        AVStream* vs = fmt_ctx_->streams[video_idx_];
        AVCodecParameters* vpar = vs->codecpar;
        video_codec_id_ = static_cast<int>(vpar->codec_id);
        video_width_    = vpar->width;
        video_height_   = vpar->height;
        video_fps_      = av_q2d(vs->avg_frame_rate);
        if (video_fps_ <= 0) video_fps_ = av_q2d(vs->r_frame_rate);
        if (video_fps_ <= 0) video_fps_ = 30.0;
        video_time_base_ = av_q2d(vs->time_base);
    }

    // Extract audio info
    if (audio_idx_ >= 0) {
        AVCodecParameters* par = fmt_ctx_->streams[audio_idx_]->codecpar;
        audio_sample_rate_ = par->sample_rate;
        audio_channels_    = par->ch_layout.nb_channels;
    }

    // Duration
    if (fmt_ctx_->duration > 0) {
        duration_ms_ = fmt_ctx_->duration / 1000;
    }

    printf("Demuxer: %s opened\n", path.c_str());
    if (video_idx_ >= 0) {
        printf("  Video: #%d %s %dx%d %.1ffps\n",
               video_idx_, avcodec_get_name(static_cast<AVCodecID>(video_codec_id_)),
               video_width_, video_height_, video_fps_);
    }
    if (audio_idx_ >= 0) {
        printf("  Audio: #%d %dHz %dch\n",
               audio_idx_, audio_sample_rate_, audio_channels_);
    }
    printf("  Duration: %ldms\n", duration_ms_);

    return true;
}

void Demuxer::close() {
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }
    video_idx_ = -1;
    audio_idx_ = -1;
}

AVPacket* Demuxer::read_packet() {
    if (!fmt_ctx_) return nullptr;
    AVPacket* pkt = av_packet_alloc();
    int ret = av_read_frame(fmt_ctx_, pkt);
    if (ret < 0) {
        av_packet_free(&pkt);
        return nullptr;
    }
    return pkt;
}

void* Demuxer::video_codecpar() const {
    if (video_idx_ < 0 || !fmt_ctx_) return nullptr;
    return fmt_ctx_->streams[video_idx_]->codecpar;
}

}  // namespace vsr
