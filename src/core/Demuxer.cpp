#include "Demuxer.h"

namespace vsr {

Demuxer::Demuxer() = default;
Demuxer::~Demuxer() { close(); }

bool Demuxer::open(const std::string&) {
    // TODO: FFmpeg avformat_open_input
    return false;
}

void Demuxer::close() {
    // TODO: avformat_close_input
}

bool Demuxer::seek(int64_t) {
    // TODO
    return false;
}

}  // namespace vsr
