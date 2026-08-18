# mpv Reference Index — vsr-player Cross-Reference

> mpv source → vsr-player 交叉引用（现行架构，2026-08）。
> **patch 方案**：`third_party/mpv/` 是完整 mpv 0.41 基座（只读），
> `src/mpv/` 是覆盖层（镜像 mpv 树，只含修改文件），`scripts/build_mpv.sh`
> 合并构建。**修改 mpv 代码 = 改 `src/mpv` 对应文件后跑 `scripts/build_mpv.sh`。**

## 1. 覆盖层索引（src/mpv ↔ third_party/mpv）

### 新增文件（上游无对应，全部在 `src/mpv/`）

| 文件 | 职责 |
|------|------|
| `video/filter/vf_vsr.c` | **VSR filter**。`mp_filter_info` name="vsr"（L1071）；`vsr_command` 热更新 scale/quality/denoise（L944，不重建 filter 链）；自定义 `m_option_type` "Scale"（L146，mpv 0.41 无 OPT_CHOICE_OR_FLOAT，支持 off/auto/数字/ratio） |
| `video/filter/vsr_proc.c` | VSRProcessor 封装（CUDA + VFX SDK 推理管线；VFX 库搜索路径 `VSR_INSTALL_LIBDIR` → `~/.local/lib/vsr-player/` → dev 路径） |
| `video/filter/vf_hwup.c` | **SW→HW 一致化**：软解帧上传 CUDA（NV12/P010 位深匹配），下游 rife/vsr 只处理 HW 帧；输出 hwctx = max(RT, 视频)（软解场景 VO 下载路径防越界） |
| `video/filter/vf_rife.c` | **RIFE 插帧 filter**。name="rife"；选项 fps（off/auto/1-120，自定义 m_option_type "rife-fps"）/scale（benchmark 倍率）/adaptive；绝对时间网格调度 + 预取；场景切换直通；fruc-status 状态行（~0.5s）供 OSD |
| `video/filter/rife_proc.c` | RIFE 推理封装：engine 文件搜索（`RIFE_LIBDIR` → `~/.local/lib/vsr-player/` → dev 路径）；nvrtc 内核（assemble/convert，7ch 输入构造）；动态 shape set_shape |
| `video/filter/rife_trt.cpp/.h` | TensorRT engine wrapper：dlopen `libnvinfer.so.11`（RTLD_LOCAL 隔离 VFX 的 TRT10）；动态引擎 [1,7,PH,PW] in/[1,3,PH,PW] out FP16；固定 shape 引擎拒绝加载 |
| `video/filter/rife_internal.h` | vf_rife 内部接口 |
| `video/filter/yuv_to_rgba.c/.h` | YUV→RGBA 转换（CPU avfilter graph，scale_cuda 不可用）；`matrix_from_repr`/`range_from_levels` 共享 inline（单一事实源，vf_vsr/vf_rife 共用） |
| `video/filter/vsr_internal.h` | vf_vsr 内部接口 |
| `video/out/vulkan/libmpv_vk.c` | libmpv render API 的 Vulkan 交换链（`pl_vulkan_import` 共享 Qt VkDevice）——Qt 侧 device 由 `src/client/VulkanContext.cpp` 创建 |
| `include/mpv/render_vk.h` | render API Vulkan 参数扩展 |

### 修改文件（与上游 diff）

| 文件 | 上游对应 | patch 主题 |
|------|---------|-----------|
| `video/out/vo_libmpv.c` | 同名 | RT 尺寸记录（SKIP_RENDERING 也记录，供自适应倍率首帧决策）+ `VOCTRL_GET_RENDER_TARGET_SIZE` 实现 + render/report_swap 等待超时诊断（DBG） |
| `video/out/vulkan/context.c` | 同名 | libmpv_vk 交换链兼容：`get_vk` 访问两条路径（vulkan_swapchain / libmpv_vk_swapchain_fns） |
| `video/out/wayland_common.c` | 同名 | `VOCTRL_GET_RENDER_TARGET_SIZE` |
| `video/out/x11_common.c` | 同名 | `VOCTRL_GET_RENDER_TARGET_SIZE` |
| `video/out/gpu/libmpv_gpu.c/.h` | 同名 | renderer `get_target_size` 回调（render API 扩展） |
| `video/out/vo.h` | 同名 | `VOCTRL_GET_RENDER_TARGET_SIZE` 声明 |
| `filters/user_filters.c/.h` | 同名 | vf_vsr 注册 |
| `filters/f_output_chain.c` | 同名 | vf 链配置（VSR 插入） |
| `filters/filter.h` | 同名 | filter API 补丁 |
| `include/mpv/render.h` | 同名 | render API 扩展（target size 查询） |
| `options/options.c/.h` | 同名 | 无 diff（保留镜像占位） |

## 2. client ↔ mpv API 对照（src/client/）

| client 文件 | 调用的 mpv API | 说明 |
|------------|---------------|------|
| `MpvController.cpp` | `mpv_create` / `mpv_set_option_string` / `mpv_initialize` | `vo=libmpv`；`msg-level all=info`（benchmark `all=no`）；`mpv_request_log_messages("info"/"no")` → 事件线程 `MPV_EVENT_LOG_MESSAGE` 转发 stderr（**勿用 log-file**，其下限 MSGL_DEBUG 绕过过滤） |
| `MpvController.cpp` | `mpv_render_context_create`（Vulkan + ADVANCED_CONTROL） | 共享 Qt VkDevice（`VulkanContext.cpp`） |
| `MpvController.cpp` | `mpv_observe_property` / `mpv_get_property` | 事件线程读属性 → post 主线程更新 viewModel |
| `MpvController.cpp` | `mpv_set_property_async` / `mpv_command_async` | 主线程异步投递，零阻塞 |
| `Video.cpp` | `mpv_render_context_render` / `report_swap` / `set_update_callback` | Qt 场景图驱动渲染循环（请求驱动模型，见 AGENTS.md） |
| `PlayerViewModel.cpp` | 属性观察器 | video-params / playlist / volume / speed / hwdec-current / path / loop-* 等 |
| `RpcServer.cpp` | `mpv_command` | JSON IPC（Unix socket）透传命令 |

## 3. 滤镜链内部结构

```
demux → decode → [vf_hwup] → [vf_rife] → [vf_vsr] → VO (libmpv)
                    ↑             ↑            ↑
              SW→CUDA 上传   rife_proc.c:   vsr_proc.c:
              （软解路径）    TRT 引擎 +    CUDA 上下文 + VFX SDK
                            nvrtc 内核     yuv_to_rgba.c: NV12/P010
                                          → RGBA（scale_cuda 本机不可用）
```

- 链序固定：rife（源分辨率插帧）在 vsr（超分）之前——GUI 由 PlayerViewModel::vfOption 拼接（`@hwup,@rife,@vsr`）
- vf_vsr 输入：`mp_image`（HW 帧解码后仍在显存）；输出：超分后的 `mp_image`
- 热更新：`vf-command @vsr <param> <value>`（scale/quality/denoise，不重建链）
- scale=auto：按显示区域（viewport）驱动倍率，`VOCTRL_GET_RENDER_TARGET_SIZE` 提供目标尺寸
- RIFE 引擎：`rife_full_fp16.engine`（动态 shape + `--hardwareCompatibilityLevel=ampere+`，30/40/50 系通用一份；构建见 tests/fruc/build_rife_full_engine.sh）

## 4. 快速查询

| 想找什么 | 位置 |
|---------|------|
| mpv filter API（mp_filter_info/command） | `third_party/mpv/filters/filter.h` + `filters/f_output_chain.c` |
| render API（libmpv 客户端渲染） | `third_party/mpv/include/mpv/render.h`（+ patch `render_vk.h`） |
| VOCTRL 定义 | `third_party/mpv/video/out/vo.h` |
| 属性观察/事件线程 | `third_party/mpv/include/mpv/client.h`（mpv_observe_property / mpv_event） |
| 日志级别语义 | `third_party/mpv/common/msg.c`（MSGL_*；log-file 下限 MSGL_DEBUG 在 L453-456） |
| playlist 状态机（stop/current） | `third_party/mpv/player/loadfile.c`（mp_play_files / PT_STOP） |
| 解码器 hwaccel（get_format） | `third_party/mpv/video/decode/vd_lavc.c` |
| 色彩转换（mp_image_params_guess_csp） | `third_party/mpv/video/mp_image.c` |
