#include "CudaProbe.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <vector>

// CUDA driver API 的最小声明（避免客户端构建依赖 cuda.h；只用到 5 个入口）。
namespace {

typedef int CUresult;                 // CUDA_SUCCESS == 0
typedef int CUdevice;                 // 不透明句柄
struct CUuuid { char bytes[16]; };

constexpr CUresult kCudaSuccess = 0;

struct CudaApi {
    void *lib = nullptr;
    CUresult (*cuInit)(unsigned) = nullptr;
    CUresult (*cuDeviceGetCount)(int *) = nullptr;
    CUresult (*cuDeviceGet)(CUdevice *, int) = nullptr;
    // CUDA 11.4+ 有 cuDeviceGetUuid_v2（MIG 感知）；老驱动只有 cuDeviceGetUuid。
    // 两个都试：能用 v2 就用 v2。
    CUresult (*cuDeviceGetUuid)(CUuuid *, CUdevice) = nullptr;
    CUresult (*cuDeviceGetName)(char *, int, CUdevice) = nullptr;

    bool load() {
        if (lib) return true;
        // 无 NVIDIA 驱动的机器上失败是**正常情况**，不打印。
        lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) lib = dlopen("libcuda.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return false;
        cuInit = (CUresult (*)(unsigned))dlsym(lib, "cuInit");
        cuDeviceGetCount = (CUresult (*)(int *))dlsym(lib, "cuDeviceGetCount");
        cuDeviceGet = (CUresult (*)(CUdevice *, int))dlsym(lib, "cuDeviceGet");
        cuDeviceGetUuid = (CUresult (*)(CUuuid *, CUdevice))dlsym(lib, "cuDeviceGetUuid_v2");
        if (!cuDeviceGetUuid)
            cuDeviceGetUuid = (CUresult (*)(CUuuid *, CUdevice))dlsym(lib, "cuDeviceGetUuid");
        cuDeviceGetName = (CUresult (*)(char *, int, CUdevice))dlsym(lib, "cuDeviceGetName");
        return cuInit && cuDeviceGetCount && cuDeviceGet && cuDeviceGetUuid;
    }
};

std::string hex_uuid(const unsigned char *b) {
    char buf[40];
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return buf;
}

}  // namespace

CudaProbeResult cuda_probe_for_vulkan_uuid(const uint8_t uuid[16]) {
    CudaProbeResult r;
    CudaApi api;
    if (!api.load()) {
        r.detail = "libcuda 不可加载（无 NVIDIA 驱动？）";
        return r;
    }
    CUresult cu = api.cuInit(0);
    if (cu != kCudaSuccess) {
        r.detail = "cuInit 失败（" + std::to_string(cu) + "）";
        return r;
    }
    int n = 0;
    if (api.cuDeviceGetCount(&n) != kCudaSuccess || n <= 0) {
        r.detail = "CUDA 可见设备数为 0（CUDA_VISIBLE_DEVICES？）";
        return r;
    }
    r.deviceCount = n;
    r.driverOk = true;

    std::vector<std::string> names;
    for (int i = 0; i < n; i++) {
        CUdevice dev = 0;
        if (api.cuDeviceGet(&dev, i) != kCudaSuccess)
            continue;
        char name[128] = {0};
        if (api.cuDeviceGetName && api.cuDeviceGetName(name, sizeof(name) - 1, dev) != kCudaSuccess)
            name[0] = '\0';
        std::string nm = name[0] ? name : "(unknown)";
        names.push_back(nm);

        CUuuid id{};
        if (api.cuDeviceGetUuid(&id, dev) != kCudaSuccess)
            continue;
        if (memcmp(id.bytes, uuid, 16) == 0) {
            r.matched = true;
            r.matchedName = nm;
        }
    }

    std::string list;
    for (size_t i = 0; i < names.size(); i++) {
        if (i) list += ", ";
        list += names[i];
    }
    r.detail = "CUDA 可见 " + std::to_string(r.deviceCount) + " 张：" + list;
    if (!r.matched)
        r.detail += "（Vulkan 设备 uuid=" + hex_uuid(uuid) + " 不在其中）";
    return r;
}
