#include "AudioOutput.h"

namespace vsr {

AudioOutput::AudioOutput() = default;
AudioOutput::~AudioOutput() { close(); }

bool AudioOutput::open(int sample_rate, int channels) {
    sample_rate_ = sample_rate;
    channels_ = channels;
    // TODO: Pa_OpenDefaultStream
    return false;
}

bool AudioOutput::start() { return false; }
bool AudioOutput::stop() { return false; }
void AudioOutput::pause(bool) {}
void AudioOutput::push_frame(const float*, int, int64_t) {}
int64_t AudioOutput::current_us() const { return position_us_; }
void AudioOutput::set_volume(double vol) { volume_ = vol; }
void AudioOutput::close() {}

}  // namespace vsr
