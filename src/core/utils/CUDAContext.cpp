#include "CUDAContext.h"

namespace vsr {

CUDAContext::CUDAContext() = default;
CUDAContext::~CUDAContext() = default;

bool CUDAContext::init(int) { return false; }
bool CUDAContext::push() { return false; }
bool CUDAContext::pop() { return false; }

}  // namespace vsr
