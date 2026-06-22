#include "NV12ToRGB.h"

namespace vsr {

NV12ToRGB::NV12ToRGB() = default;
NV12ToRGB::~NV12ToRGB() = default;

bool NV12ToRGB::convert(uint8_t*, int, uint8_t*, int, int, int, float*) {
    // TODO: CUDA kernel — YUV→RGB BT.601
    return false;
}

}  // namespace vsr
