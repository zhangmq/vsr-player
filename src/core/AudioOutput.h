#pragma once

#include <cstdint>

namespace vsr {

/// PortAudio-based audio output. Feeds PCM to system audio device.
/// Provides the master clock reference for A/V sync via ClockManager.
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    /// Open audio stream with given sample rate and channels.
    bool open(int sample_rate, int channels);

    /// Start playback.
    bool start();

    /// Stop playback.
    bool stop();

    /// Pause / resume.
    void pause(bool paused);

    /// Push a decoded audio frame (PCM float32 planar).
    void push_frame(const float* data, int num_samples, int64_t pts_us);

    /// Current playback position in microseconds.
    int64_t current_us() const;

    /// Set volume (0.0 - 1.0).
    void set_volume(double vol);

    /// Close the audio stream.
    void close();

private:
    void* stream_ = nullptr;  // PaStream*
    int sample_rate_ = 0;
    int channels_ = 0;
    double volume_ = 1.0;
    int64_t position_us_ = 0;
};

}  // namespace vsr
