#pragma once

#include <cstdint>

namespace vsr {

/// Master clock for audio/video synchronization.
/// Audio clock is the master; video timestamps are compared against it.
class ClockManager {
public:
    ClockManager();
    ~ClockManager();

    /// Get current master clock time in microseconds.
    int64_t master_us() const;

    /// Set the audio clock reference (called by AudioOutput).
    void set_audio_clock(int64_t us);

    /// Get the delta between a video PTS and the audio clock.
    /// Positive = video ahead of audio.
    int64_t video_delta_us(int64_t video_pts_us) const;

    /// Reset clock to zero.
    void reset();

private:
    int64_t audio_clock_us_ = 0;
};

}  // namespace vsr
