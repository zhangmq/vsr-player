#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <portaudio.h>

namespace vsr {

/// PortAudio-based audio player with background FFmpeg decode thread.
///
/// Self-contained: opens the file, decodes audio in a background thread,
/// feeds PCM to PortAudio via a ring buffer, and provides the master clock
/// for A/V sync (mpv/VLC model — audio is the master).
///
/// API inspired by the Python reference (archive/python-v1/vsr_player/audio.py).
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    /// Open and probe the audio stream. Must call before start().
    bool open(const char* file_path);

    /// Start playback: spawns decode thread and opens PortAudio stream.
    bool start();

    /// Stop playback and join decode thread.
    void stop();

    /// Pause audio (stream stops, clock freezes, decode thread continues).
    void pause();

    /// Resume from pause (stream restarts, clock continues from freeze point).
    void resume();

    /// Seek to target position in seconds.
    void seek(double target_sec);

    /// Master clock in seconds. Drives A/V sync.
    double clock_sec() const;

    /// Whether the audio stream is active (has audio track).
    bool is_active() const { return has_audio_; }

    /// Sample rate and channels (from probed stream).
    int sample_rate() const { return sample_rate_; }
    int channels() const { return channels_; }

private:
    void decode_loop(double seek_target);
    void stop_internal();

    // Ring buffer (SPSC, float32 interleaved)
    std::vector<float> ring_buf_;
    static constexpr double kBufferSec = 0.5;
    int ring_capacity_ = 0;
    std::atomic<int> ring_read_{0};
    std::atomic<int> ring_write_{0};
    std::atomic<int> ring_filled_{0};

    int write_ring(const float* data, int num_samples);
    int read_ring(float* dst, int num_samples);
    void clear_ring();

    // PortAudio
    void* pa_stream_ = nullptr;
    static int pa_callback(const void* input, void* output,
                           unsigned long frame_count,
                           const PaStreamCallbackTimeInfo* time_info,
                           PaStreamCallbackFlags status_flags,
                           void* user_data);

    // Thread
    std::thread decode_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};

    // Audio stream info
    bool has_audio_ = false;
    int sample_rate_ = 48000;
    int channels_ = 2;

    // Clock state
    mutable std::mutex clock_mutex_;
    double start_time_ = 0.0;    // Pa_GetStreamTime() at last start/resume
    double frozen_clock_ = 0.0;  // clock value when paused

    // File path (for decode thread to reopen)
    std::string file_path_;
};

}  // namespace vsr
