#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <portaudio.h>

namespace soundtouch { class SoundTouch; }

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

    /// Open for PCM playback (no file — data fed via write_pcm).
    /// @param sample_rate  Audio sample rate (e.g. 48000)
    /// @param channels     Channel count (1 = mono, 2 = stereo)
    bool open(int sample_rate, int channels);

    /// Write interleaved float32 PCM to the ring buffer.
    /// Thread-safe. Non-blocking; data is dropped if buffer is full.
    /// @param data         Interleaved float32 PCM ([-1.0, 1.0])
    /// @param num_samples  Frame count (not float count!)
    void write_pcm(const float* data, int num_samples);

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

    /// Set volume (0.0 = silence, 1.0 = full). Applied in PortAudio callback
    /// with zero latency — takes effect on the next callback (~10ms).
    void set_volume(double vol);

    /// Set playback speed. Resamples PCM in decode thread — clock
    /// naturally advances at the speed-adjusted rate (0.5x = half,
    /// 2x = double). Speed change takes effect within one decode frame.
    void set_speed(double speed);

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

    // Clock state — returns CONTENT time, not real time.
    // clock = clock_base_ + (Pa_GetStreamTime - stream_start_) * clock_speed_
    // On speed change, clock_base_ and stream_start_ are reset so the clock
    // never jumps — preserving continuity while the rate changes.
    mutable std::mutex clock_mutex_;
    double clock_base_ = 0.0;     // content-time at segment start
    double stream_start_ = 0.0;   // Pa_GetStreamTime when segment started
    std::atomic<double> clock_speed_{1.0};  // effective rate: real→content
    double frozen_clock_ = 0.0;   // clock value when paused

    // Seek state (lightweight — no Pa_Terminate/Pa_Initialize)
    std::atomic<bool> seek_requested_{false};
    double seek_target_ = 0.0;

    // Playback speed (read by decode thread for PCM resampling)
    std::atomic<double> playback_speed_{1.0};

    // Volume: applied in PortAudio callback. 1.0 = full, 0.0 = silence.
    std::atomic<double> volume_{1.0};

    // Pitch-preserving time-stretch (SoundTouch WSOLA)
    std::unique_ptr<soundtouch::SoundTouch> stretcher_;
    double stretch_speed_ = 1.0;  // last tempo set on stretcher

    // File path (decode thread reopens per seek)
    std::string file_path_;
};

}  // namespace vsr
