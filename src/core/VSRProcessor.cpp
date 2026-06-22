#include "VSRProcessor.h"

namespace vsr {

VSRProcessor::VSRProcessor() = default;
VSRProcessor::~VSRProcessor() { release(); }

bool VSRProcessor::init(int in_w, int in_h, int out_w, int out_h, Quality quality) {
    in_w_ = in_w;
    in_h_ = in_h;
    out_w_ = out_w;
    out_h_ = out_h;
    quality_ = quality;
    // TODO: NvVFX create effect, set params, load()
    return false;
}

bool VSRProcessor::process(void*, void**, int*, int*) {
    // TODO: NvVFX run()
    return false;
}

bool VSRProcessor::reconfigure(int out_w, int out_h, Quality quality) {
    release();
    return init(in_w_, in_h_, out_w, out_h, quality);
}

void VSRProcessor::release() {
    // TODO: NvVFX destroy
}

}  // namespace vsr
