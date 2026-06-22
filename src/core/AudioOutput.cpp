#include "AudioOutput.h"

#include <cstdio>
#include <cstring>

#include <portaudio.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace vsr {

// ── Constructor / Destructor ──────────────────────────────────────────

AudioOutput::AudioOutput() = default;

AudioOutput::~AudioOutput() {
    stop();
}

// ── Open (probe audio stream) ─────────────────────────────────────────

bool AudioOutput::open(const char* file_path) {
    file_path_ = file_path;

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, file_path, nullptr, nullptr) < 0) {
        fprintf(stderr, "Audio: cannot open file\n");
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return false;
    }

    // Find first audio stream
    int audio_idx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_idx = i;
            break;
        }
    }

    if (audio_idx < 0) {
        fprintf(stderr, "Audio: no audio stream found\n");
        avformat_close_input(&fmt);
        return false;
    }

    AVCodecParameters* par = fmt->streams[audio_idx]->codecpar;
    sample_rate_ = par->sample_rate ? par->sample_rate : 48000;
    channels_    = par->ch_layout.nb_channels ? (int)par->ch_layout.nb_channels : 2;

    fprintf(stderr, "Audio: probed — %d Hz, %d ch\n", sample_rate_, channels_);

    avformat_close_input(&fmt);

    // Init ring buffer (~0.5 seconds)
    ring_capacity_ = static_cast<int>(sample_rate_ * kBufferSec);
    ring_buf_.resize(ring_capacity_ * channels_);
    clear_ring();

    has_audio_ = true;
    return true;
}

// ── Ring buffer ───────────────────────────────────────────────────────

void AudioOutput::clear_ring() {
    ring_read_ = 0;
    ring_write_ = 0;
    ring_filled_ = 0;
}

int AudioOutput::write_ring(const float* data, int num_samples) {
    int avail = ring_capacity_ - ring_filled_.load(std::memory_order_relaxed);
    int n = std::min(num_samples, avail);
    if (n <= 0) return 0;

    int w = ring_write_.load(std::memory_order_relaxed);
    int ch = channels_;
    float* buf = ring_buf_.data();
    int cap = ring_capacity_;

    if (w + n <= cap) {
        memcpy(buf + w * ch, data, n * ch * sizeof(float));
    } else {
        int first = cap - w;
        memcpy(buf + w * ch, data, first * ch * sizeof(float));
        memcpy(buf, data + first * ch, (n - first) * ch * sizeof(float));
    }
    ring_write_.store((w + n) % cap, std::memory_order_release);
    ring_filled_.fetch_add(n, std::memory_order_release);
    return n;
}

int AudioOutput::read_ring(float* dst, int num_samples) {
    int filled = ring_filled_.load(std::memory_order_acquire);
    int n = std::min(num_samples, filled);
    if (n <= 0) return 0;

    int r = ring_read_.load(std::memory_order_relaxed);
    int ch = channels_;
    float* buf = ring_buf_.data();
    int cap = ring_capacity_;

    if (r + n <= cap) {
        memcpy(dst, buf + r * ch, n * ch * sizeof(float));
    } else {
        int first = cap - r;
        memcpy(dst, buf + r * ch, first * ch * sizeof(float));
        memcpy(dst + first * ch, buf, (n - first) * ch * sizeof(float));
    }
    ring_read_.store((r + n) % cap, std::memory_order_release);
    ring_filled_.fetch_sub(n, std::memory_order_release);
    return n;
}

// ── PortAudio callback ────────────────────────────────────────────────

int AudioOutput::pa_callback(const void*, void* output,
                              unsigned long frame_count,
                              const PaStreamCallbackTimeInfo*,
                              PaStreamCallbackFlags,
                              void* user_data) {
    auto* self = static_cast<AudioOutput*>(user_data);
    float* out = static_cast<float*>(output);
    int read = self->read_ring(out, static_cast<int>(frame_count));
    if (read < static_cast<int>(frame_count)) {
        // Underrun — fill remainder with silence
        memset(out + read * self->channels_, 0,
               (frame_count - read) * self->channels_ * sizeof(float));
    }
    return paContinue;
}

// ── Start ─────────────────────────────────────────────────────────────

bool AudioOutput::start() {
    if (!has_audio_ || running_.load()) return false;

    running_.store(true);

    // Spawn decode thread
    decode_thread_ = std::thread(&AudioOutput::decode_loop, this, -1.0);

    // Pre-buffer (~250ms)
    while (ring_filled_.load() < sample_rate_ / 4) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Open PortAudio stream
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "Audio: Pa_Initialize failed: %s\n", Pa_GetErrorText(err));
        running_.store(false);
        return false;
    }

    PaStreamParameters params;
    params.device = Pa_GetDefaultOutputDevice();
    params.channelCount = channels_;
    params.sampleFormat = paFloat32;
    params.suggestedLatency = 0.02;
    params.hostApiSpecificStreamInfo = nullptr;

    err = Pa_OpenStream((PaStream**)&pa_stream_, nullptr, &params,
                         sample_rate_, 1024, paNoFlag,
                         pa_callback, this);
    if (err != paNoError) {
        fprintf(stderr, "Audio: Pa_OpenStream failed: %s\n", Pa_GetErrorText(err));
        Pa_Terminate();
        running_.store(false);
        return false;
    }

    err = Pa_StartStream((PaStream*)pa_stream_);
    if (err != paNoError) {
        fprintf(stderr, "Audio: Pa_StartStream failed: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream((PaStream*)pa_stream_);
        Pa_Terminate();
        pa_stream_ = nullptr;
        running_.store(false);
        return false;
    }

    start_time_ = Pa_GetStreamTime((PaStream*)pa_stream_);
    paused_.store(false);
    fprintf(stderr, "Audio: playback started\n");
    return true;
}

// ── Stop ──────────────────────────────────────────────────────────────

void AudioOutput::stop() { stop_internal(); }

void AudioOutput::stop_internal() {
    running_.store(false);
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }
    if (pa_stream_) {
        Pa_StopStream((PaStream*)pa_stream_);
        Pa_CloseStream((PaStream*)pa_stream_);
        Pa_Terminate();
        pa_stream_ = nullptr;
    }
    clear_ring();
}

// ── Pause / Resume ────────────────────────────────────────────────────

void AudioOutput::pause() {
    if (!pa_stream_ || paused_.load()) return;
    frozen_clock_ = clock_sec();
    paused_.store(true);
    Pa_StopStream((PaStream*)pa_stream_);
}

void AudioOutput::resume() {
    if (!pa_stream_ || !paused_.load()) return;
    Pa_StartStream((PaStream*)pa_stream_);
    start_time_ = Pa_GetStreamTime((PaStream*)pa_stream_) - frozen_clock_;
    paused_.store(false);
}

// ── Seek ──────────────────────────────────────────────────────────────

void AudioOutput::seek(double target_sec) {
    bool was_paused = paused_.load();
    stop_internal();
    clear_ring();
    running_.store(true);
    paused_.store(false);
    frozen_clock_ = 0.0;

    decode_thread_ = std::thread(&AudioOutput::decode_loop, this, target_sec);

    // Pre-buffer with timeout
    for (int waited = 0;
         ring_filled_.load() < sample_rate_ / 4 && waited < 300;
         waited++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    PaError err = Pa_Initialize();
    if (err != paNoError) return;

    PaStreamParameters params;
    params.device = Pa_GetDefaultOutputDevice();
    params.channelCount = channels_;
    params.sampleFormat = paFloat32;
    params.suggestedLatency = 0.02;
    params.hostApiSpecificStreamInfo = nullptr;

    err = Pa_OpenStream((PaStream**)&pa_stream_, &params, nullptr,
                         sample_rate_, 1024, paNoFlag, pa_callback, this);
    if (err == paNoError) {
        Pa_StartStream((PaStream*)pa_stream_);
        start_time_ = Pa_GetStreamTime((PaStream*)pa_stream_) - target_sec;
    }

    if (was_paused) pause();
}

// ── Master clock ──────────────────────────────────────────────────────

double AudioOutput::clock_sec() const {
    if (!pa_stream_ || !has_audio_) return 0.0;
    if (paused_.load()) return frozen_clock_;
    return Pa_GetStreamTime((PaStream*)pa_stream_) - start_time_;
}

// ── Decode loop (background thread) ───────────────────────────────────

void AudioOutput::decode_loop(double seek_target) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, file_path_.c_str(), nullptr, nullptr) < 0)
        return;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return;
    }

    int audio_idx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_idx = i;
            break;
        }
    }
    if (audio_idx < 0) {
        avformat_close_input(&fmt);
        return;
    }

    AVCodecParameters* par = fmt->streams[audio_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        avformat_close_input(&fmt);
        return;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, par);
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return;
    }

    // Seek if requested
    if (seek_target > 0.0) {
        AVRational tb = fmt->streams[audio_idx]->time_base;
        int64_t target_ts = av_rescale_q(static_cast<int64_t>(seek_target * AV_TIME_BASE),
                                         AV_TIME_BASE_Q, tb);
        av_seek_frame(fmt, audio_idx, target_ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(ctx);
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    while (running_.load()) {
        int ret = av_read_frame(fmt, pkt);
        if (ret < 0) break;  // EOF or error

        if (pkt->stream_index != audio_idx) {
            av_packet_unref(pkt);
            continue;
        }

        ret = avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (ret >= 0) {
            ret = avcodec_receive_frame(ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            // Convert to float32 interleaved
            int samples = frame->nb_samples;
            std::vector<float> pcm(samples * channels_, 0.0f);

            if (frame->format == AV_SAMPLE_FMT_FLTP) {
                // Planar float → interleaved float
                for (int ch = 0; ch < channels_ && ch < frame->ch_layout.nb_channels; ch++) {
                    const float* src = reinterpret_cast<float*>(frame->data[ch]);
                    for (int s = 0; s < samples; s++)
                        pcm[s * channels_ + ch] = src[s];
                }
            } else if (frame->format == AV_SAMPLE_FMT_S16P) {
                for (int ch = 0; ch < channels_ && ch < frame->ch_layout.nb_channels; ch++) {
                    const int16_t* src = reinterpret_cast<int16_t*>(frame->data[ch]);
                    for (int s = 0; s < samples; s++)
                        pcm[s * channels_ + ch] = src[s] / 32768.0f;
                }
            } else if (frame->format == AV_SAMPLE_FMT_S16) {
                const int16_t* src = reinterpret_cast<int16_t*>(frame->data[0]);
                for (int s = 0; s < samples; s++) {
                    for (int ch = 0; ch < channels_ && ch < frame->ch_layout.nb_channels; ch++)
                        pcm[s * channels_ + ch] = src[s * channels_ + ch] / 32768.0f;
                }
            } else if (frame->format == AV_SAMPLE_FMT_FLT) {
                const float* src = reinterpret_cast<float*>(frame->data[0]);
                for (int s = 0; s < samples; s++) {
                    for (int ch = 0; ch < channels_ && ch < frame->ch_layout.nb_channels; ch++)
                        pcm[s * channels_ + ch] = src[s * channels_ + ch];
                }
            }

            // Write to ring buffer, throttle if full
            int written = write_ring(pcm.data(), samples);
            while (written < samples && running_.load()) {
                // Ring buffer nearly full — wait
                if (ring_filled_.load() > ring_capacity_ * 8 / 10) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                written += write_ring(pcm.data() + written * channels_,
                                      samples - written);
            }
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
}

}  // namespace vsr
