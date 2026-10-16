// src/mpv/video/filter/vfx_loader.c
// VFX 库定位与加载层的唯一实现（接口见 vfx_loader.h）——纯搬移自
// vsr_proc.c：搜索顺序、日志文本、dlopen 顺序与标志、dlsym 符号集合、
// 失败分支语义均逐字未改。
#include "vfx_loader.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ── 库句柄与符号表（进程内单例）──────────────────────────────────────────

void *g_vfx_lib = NULL;
void *g_nvcv_lib = NULL;


PFN_NvVFX_CreateEffect    pfn_NvVFX_CreateEffect;
PFN_NvVFX_DestroyEffect   pfn_NvVFX_DestroyEffect;
PFN_NvVFX_SetU32          pfn_NvVFX_SetU32;
PFN_NvVFX_SetImage        pfn_NvVFX_SetImage;
PFN_NvVFX_GetImage        pfn_NvVFX_GetImage;
PFN_NvVFX_SetCudaStream   pfn_NvVFX_SetCudaStream;
PFN_NvVFX_Load            pfn_NvVFX_Load;
PFN_NvVFX_Run             pfn_NvVFX_Run;
PFN_NvCVImage_Alloc       pfn_NvCVImage_Alloc;
PFN_NvCVImage_Dealloc     pfn_NvCVImage_Dealloc;
PFN_NvCVImage_Transfer    pfn_NvCVImage_Transfer;

// ── Library loading ──────────────────────────────────────────────────────

#define LOAD_SYM(LIB, NAME)                                      \
    do {                                                          \
        pfn_##NAME = (PFN_##NAME)dlsym(LIB, #NAME);             \
        if (!pfn_##NAME) {                                        \
            fprintf(stderr, "VSR: dlsym(" #NAME ") failed: %s\n", \
                    dlerror());                                   \
            return false;                                         \
        }                                                         \
    } while (0)

bool vfx_load_libraries(void) {
    if (g_vfx_lib) return true;

    // Find nvvfx lib directory — try common locations
    const char *home = getenv("HOME");
    char homedir_path[1024] = "";
    if (home) {
        snprintf(homedir_path, sizeof(homedir_path), "%s/.local/lib/vsr-player/", home);
    }
    // dev 路径按**可执行文件目录**推导，且仅当可执行文件位于构建树（路径含
    // "/build/"）时启用——不用 CWD 相对路径：分发包在任意目录（例如视频所在
    // 目录）运行时，CWD 里的 libNVCVImage.so 会被加载，构成库劫持面。
    // dev 布局：<repo>/build/src/client/vsr-player → ../../../third_party/…
    char dev_vfx[1024] = "", dev_build_lib[1024] = "";
    {
        char exe[1024];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            char *slash = strrchr(exe, '/');
            if (slash && strstr(exe, "/build/")) {
                *slash = '\0';
                snprintf(dev_vfx, sizeof(dev_vfx),
                         "%s/../../../third_party/nvvfx/lib/", exe);
                snprintf(dev_build_lib, sizeof(dev_build_lib),
                         "%s/../../../build/lib/", exe);
            }
        }
    }
    // 查找顺序（第一个含 libNVCVImage.so 的目录胜出）：compile-time 打包路径
    // → dev 树 → HOME 用户安装 → build/lib。dev 树必须排在 HOME **之前**：
    // HOME 下的安装可能是旧代 SDK（例如 1.1 没有 libnvVFXVideoFrameGeneration.so），
    // 若它先命中，开发树里的新库会被静默遮蔽——跑的是旧版却没有任何提示
    // （已发生过：整轮 VSR 性能 A/B 两次都跑的是 HOME 的旧库，结论作废）。
    const char *search[] = {
#ifdef VSR_INSTALL_LIBDIR
        VSR_INSTALL_LIBDIR,           // compile-time override (packaging)
#endif
        dev_vfx,                       // dev only（构建树内运行时；优先于 HOME）
        homedir_path,                  // user-local default
        dev_build_lib,                 // dev only
    };
    const char *nvvfx_dir = NULL;
    int n_search = sizeof(search) / sizeof(search[0]);
    for (int i = 0; i < n_search; i++) {
        if (!search[i] || !search[i][0]) continue;       // skip NULL or empty
        char path[1024];
        snprintf(path, sizeof(path), "%s%s", search[i], "libNVCVImage.so");
        if (access(path, R_OK) == 0) { nvvfx_dir = search[i]; break; }
    }
    if (!nvvfx_dir) {
        fprintf(stderr, "VSR: nvvfx libs not found\n");
        return false;
    }

    // VFX ≤0.1.0.1 捆绑的 TensorRT 10 链（onnxparser → nvinfer → plugin）：
    // RTLD_LOCAL 预加载，必须在 libVideoFXLocal.so 之前（其 NEEDED
    // libnvonnxparser.so.10 靠 SONAME 匹配复用）。RTLD_GLOBAL 会把 TRT 10
    // 符号注入全局命名空间 → 系统 TRT 11（vf_rife 链接）符号绑错 → ABI 崩溃；
    // LOCAL 仍满足 NEEDED。
    // 1.3（wheel 0.2.0.0）起该链已消失：wheel 不再捆绑 libnvinfer*，且
    // libVideoFXLocal.so 已无 libnvonnxparser NEEDED（objdump -x 核实）——
    // 所以在 1.3 下本段是**正常空操作**（文件不存在即跳过，不再打误导性的
    // dlopen 失败日志）。保留它是为了 ≤0.1.0.1 的旧安装兼容：那里这条链
    // 仍是硬需求，缺了它 libVideoFXLocal.so 加载即失败。
    {
        const char *trt10[] = {
            "libnvinfer.so.10", "libnvinfer_plugin.so.10", "libnvonnxparser.so.10",
        };
        for (int i = 0; i < 3; i++) {
            char path[1024];
            snprintf(path, sizeof(path), "%s%s", nvvfx_dir, trt10[i]);
            if (access(path, R_OK) != 0)
                continue;   // 1.3+：目录中无 TRT10 链，正常跳过
            void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (!h)
                fprintf(stderr, "VSR: dlopen(%s): %s\n", trt10[i], dlerror());
        }
    }

    // Load all nvvfx .so files (order matters for deps)
    // Topological order: deps before dependents
    const char *libs[] = {
        "libnppc.so", "libnppial.so", "libnppicc.so", "libnppidei.so",
        "libnppig.so", "libnppif.so", "libnppim.so", "libnppist.so",
        "libnppitc.so", "libcudnn.so",
        "libNVCVImage.so",    // before libnvngxruntime which needs it
        "libnvngxruntime.so", "libnvidia-ngx-vsr.so",
        "libVideoFXLocal.so", "libVideoFX.so",
        "libnvVFXVideoSuperRes.so",
        // VFG（1.3 新增的 AI 插帧 effect）——VFX 1.1 目录内没有这个库，
        // 1.2 之前同样缺（1.3 wheel 0.2.0.0 才有）。存在性守卫见下：
        // 无守卫时每次启动都会多打一条误导性的 dlopen 失败。
        "libnvVFXVideoFrameGeneration.so",
    };
    int nlibs = sizeof(libs) / sizeof(libs[0]);
    for (int i = 0; i < nlibs; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s%s", nvvfx_dir, libs[i]);
        // VFG 库先做可达性检查：旧代 SDK 没有它，缺失属正常（VSR 照常
        // 工作），一条清晰说明即可——不能让 dlopen 的失败文本刷屏。
        // vfx_load_libraries 进程内幂等（g_vfx_lib 缓存）→ 只打一次。
        if (strcmp(libs[i], "libnvVFXVideoFrameGeneration.so") == 0 &&
            access(path, R_OK) != 0) {
            fprintf(stderr, "VSR: VFG 不可用（此 VFX SDK 无 "
                            "libnvVFXVideoFrameGeneration.so）\n");
            continue;
        }
        void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            fprintf(stderr, "VSR: dlopen(%s): %s\n", libs[i], dlerror());
            // Continue — some libs might be missing but others work
        }
    }

    // Retrieve handles for the two we need symbols from
    char path[1024];
    snprintf(path, sizeof(path), "%s%s", nvvfx_dir, "libVideoFX.so");
    g_vfx_lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    snprintf(path, sizeof(path), "%s%s", nvvfx_dir, "libNVCVImage.so");
    g_nvcv_lib = dlopen(path, RTLD_NOW);
    if (!g_vfx_lib || !g_nvcv_lib) {
        fprintf(stderr, "VSR: core libs failed\n");
        if (g_vfx_lib) dlclose(g_vfx_lib);
        if (g_nvcv_lib) dlclose(g_nvcv_lib);
        g_vfx_lib = NULL;
        g_nvcv_lib = NULL;
        return false;
    }

    LOAD_SYM(g_vfx_lib,  NvVFX_CreateEffect);
    LOAD_SYM(g_vfx_lib,  NvVFX_DestroyEffect);
    LOAD_SYM(g_vfx_lib,  NvVFX_SetU32);
    LOAD_SYM(g_vfx_lib,  NvVFX_SetImage);
    LOAD_SYM(g_vfx_lib,  NvVFX_GetImage);
    LOAD_SYM(g_vfx_lib,  NvVFX_SetCudaStream);
    LOAD_SYM(g_vfx_lib,  NvVFX_Load);
    LOAD_SYM(g_vfx_lib,  NvVFX_Run);
    LOAD_SYM(g_nvcv_lib, NvCVImage_Alloc);
    LOAD_SYM(g_nvcv_lib, NvCVImage_Dealloc);
    LOAD_SYM(g_nvcv_lib, NvCVImage_Transfer);

    // 汇报实际生效的目录与版本代际（启动期一次，低频）：HOME 安装与 dev 树
    // 可能不同代，日志里必须能区分，否则"测的是哪版 SDK"无从追溯。
    // 代际判据：有 libnvVFXVideoFrameGeneration.so ⇒ 1.3+；无 ⇒ 旧版。
    bool has_vfg = false;
    char ngx_vsr_ver[256] = "";
    DIR *dir = opendir(nvvfx_dir);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, "libnvVFXVideoFrameGeneration.so") == 0)
                has_vfg = true;
            else if (strncmp(ent->d_name, "libnvidia-ngx-vsr.so.",
                             sizeof("libnvidia-ngx-vsr.so.") - 1) == 0)
                snprintf(ngx_vsr_ver, sizeof(ngx_vsr_ver), "%s", ent->d_name);
        }
        closedir(dir);
    }
    char *resolved_dir = realpath(nvvfx_dir, NULL);   // 打印规范化绝对路径（否则是拼出来的 ".." 路径）
    fprintf(stderr, "VSR: NvVFX libraries loaded (nvvfx dir=%s, gen=%s%s%s)\n",
            resolved_dir ? resolved_dir : nvvfx_dir,
            has_vfg ? "1.3+ (VideoFrameGeneration present)"
                    : "legacy (no VideoFrameGeneration)",
            ngx_vsr_ver[0] ? ", ngx-vsr=" : "", ngx_vsr_ver);
    free(resolved_dir);
    return true;
}

#undef LOAD_SYM
