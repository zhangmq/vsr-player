#include "Decoder.h"

namespace vsr {

Decoder::Decoder() = default;
Decoder::~Decoder() = default;

bool Decoder::init(void*, int width, int height) {
    width_ = width;
    height_ = height;
    // TODO: create AVCodecContext with hw_device_ctx
    return false;
}

bool Decoder::decode(void*) {
    // TODO: avcodec_send_packet / avcodec_receive_frame
    return false;
}

uint8_t* Decoder::plane_data(int) const { return nullptr; }
int Decoder::plane_pitch(int) const { return 0; }
int64_t Decoder::frame_pts_us() const { return 0; }
void Decoder::flush() {}

}  // namespace vsr
