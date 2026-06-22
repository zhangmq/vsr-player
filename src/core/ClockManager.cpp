#include "ClockManager.h"

namespace vsr {

ClockManager::ClockManager() = default;
ClockManager::~ClockManager() = default;

int64_t ClockManager::master_us() const {
    return audio_clock_us_;
}

void ClockManager::set_audio_clock(int64_t us) {
    audio_clock_us_ = us;
}

int64_t ClockManager::video_delta_us(int64_t video_pts_us) const {
    return video_pts_us - audio_clock_us_;
}

void ClockManager::reset() {
    audio_clock_us_ = 0;
}

}  // namespace vsr
