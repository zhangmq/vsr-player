---
name: mpv-render-context-create-18
description: mpv_render_context_create returns -18 — root cause and fix checklist

# mpv_render_context_create 返回 -18 (MPV_ERROR_UNSUPPORTED)

## 根因

`pl_vulkan_import` (libplacebo) 的 `check_required_features` 要求 `VkPhysicalDeviceVulkan12Features` pNext 链中以下两项均为 VK_TRUE：

- `hostQueryReset`
- `timelineSemaphore`

当 `mpv_vulkan_init_params.features = NULL` 时，`vk_features_normalize` 产出一个空的 features 链 → `vk_find_struct(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)` 返回 NULL → 检查失败 → `pl_vulkan_import` 返回 NULL → `mpv_render_context_create` 返回 -18。

## 修复（必做检查清单）

在调用 `mpv_render_context_create` 之前，确保：

```cpp
// 1. 查询 supported features（必须包含 VkPhysicalDeviceVulkan12Features pNext）
VkPhysicalDeviceVulkan12Features vk12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &vk12};
vkGetPhysicalDeviceFeatures2(pd, &f2);

// 2. 传递给 mpv
mpv_vulkan_init_params vkp = {};
// ... instance, device, queue_family ...
vkp.features = &f2;  // ← 必须是 non-NULL，且包含 vk12 链

// 3. 如果 vkGetDeviceFeatures2 可用，用它替代 vkGetPhysicalDeviceFeatures2
//    （前者返回实际已启用的 features，后者返回支持的 features）
//    但 pl_vulkan_create 会启用所有请求的 features，所以用 supported 也安全。
```

## 诊断陷阱：libplacebo 静默日志

`pl_log_create(PL_API_VER, &(struct pl_log_params){.log_level = PL_LOG_TRACE})` — 如果未设置 `.log_cb`，`pl_msg_test` 会因 `log_cb == NULL` 返回 false，导致**所有**日志（包括 PL_LOG_FATAL）被静默丢弃。正确做法：

```c
struct pl_log_params lp = {.log_level = PL_LOG_TRACE, .log_cb = pl_log_simple};
pl_log log = pl_log_create(PL_API_VER, &lp);
```

## 相关 libplacebo 源码位置

- `src/vulkan/context.c:1654-1660` — `pl_vulkan_import` features 检查
- `src/vulkan/context.c:324-341` — `check_required_features`（硬编码要求 hostQueryReset + timelineSemaphore）
- `src/log.c:31-34` — `pl_msg_test`（log_cb NULL 时静默丢弃）
- `src/log.c:102-117` — `pl_log_simple`（可用作默认 log_cb）

## 历史记录

- 2026-07-02：在 test-client (GLFW+Vulkan+mpv) 中首次遇到，花费数小时排查。原因是 features=NULL + 日志静默双重陷阱。
- Qt 客户端 (main_phase1.cpp) 从第一天就传递了正确的 features（`&f2`），所以从未遇到此问题。
