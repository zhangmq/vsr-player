# CUDA-Vulkan Interop — Zero-PCIe Pipeline Design

> **Date:** 2026-06-23
> **Status:** Approved

## Goal

Eliminate the two PCIe round-trips in the current rendering pipeline by introducing GPU-side
CUDA-Vulkan interop textures. Both NO-VSR and VSR paths go from GPU decode all the way to
Vulkan presentation without touching system memory.

Secondary: add NV12→RGB conversion in a Vulkan shader for the NO-VSR path (removing the
CUDA kernel from that path), and size the swapchain to physical pixels.

---

## Current Pain

```
NO-VSR:  NVDEC(NV12 GPU) → CUDA NV12→RGB f32 → DtoH(PCIe↓) → CPU float→uint8 → vkMapMemory(PCIe↑) → Vulkan
VSR:     NVDEC(NV12 GPU) → CUDA NV12→RGB f32 → VSR(RGBA8) → DtoH(PCIe↓) → vkMapMemory(PCIe↑) → Vulkan
```

Two PCIe round-trips per frame. Data leaves the GPU only to come right back.

## Target

```
NO-VSR:  NVDEC(NV12 GPU) ─┬→ D2D→ InteropTexture_Y(R8_UNORM)  ─┐
            (HW)           └→ D2D→ InteropTexture_UV(R8G8_UNORM) ┤→ Vulkan NV12 shader → swapchain
            (SW)           ┌→ H2D→ InteropTexture_Y              ┤
                           └→ H2D→ InteropTexture_UV             ┘

VSR:     NVDEC(NV12 GPU) → CUDA NV12→RGB f32 → VSR → RGBA8 ─→ D2D→ InteropTexture_RGBA → Vulkan pass-through → swapchain
            (SW)           → H2D→ temp GPU buf → CUDA NV12→RGB → VSR → D2D→ InteropTexture_RGBA → ...
```

Zero PCIe for HW decode. One H2D (write-only) for SW decode VSR path.

---

## Module Split

```
src/core/utils/
├── VulkanContext.h/cpp        ← extracted from VulkanRenderer (~200 lines)
├── InteropTexture.h/cpp       ← NEW (~120 lines)
├── SwapchainManager.h/cpp     ← extracted from VulkanRenderer (~180 lines)
├── VideoPipeline.h/cpp        ← NEW (~150 lines)
├── VulkanRenderer.h/cpp       ← coordinator, simplified (~80 lines)
```

### Responsibility & Interface

| Class | Responsibility | Key Interface |
|-------|---------------|---------------|
| `VulkanContext` | Instance, device, queue, command pool, sync primitives, sampler | `init(display, surface) → bool` |
| `InteropTexture` | Single Vulkan image with external fd export + CUDA import, co-visible to both APIs | `init(dev, pd, w, h, format) → bool`; `cudaPtr() → CUdeviceptr`; `cudaPitch() → size_t`; `imageView() → VkImageView` |
| `SwapchainManager` | Swapchain, render pass, framebuffers, image acquire, physical-pixel sizing | `create(w, h) → bool`; `acquire() → uint32_t`; `fb(idx) → VkFramebuffer`; uses `caps.currentExtent` directly |
| `VideoPipeline` | Descriptor set layout, pool, set, pipeline layout, pipeline, and its InteropTexture(s). Config-driven, not subclassed. | `init(config) → bool`; `bind(cb)`; `interopTexture(i) → InteropTexture&` |
| `VulkanRenderer` | Coordinator: holds the above, orchestrates draw + present | `render_frame(path) → bool` where `Path ∈ {VSR, NOVSR}` |

### VideoPipeline Config

Two instances, same class, different `PipelineConfig`:

```cpp
struct PipelineConfig {
    std::vector<VkFormat> textureFormats;  // {R8G8B8A8_UNORM} or {R8_UNORM, R8G8_UNORM}
    const uint32_t* fragSpv;
    size_t fragSpvLen;
};
```

| Instance | Formats | Shader | InteropTextures | Used by |
|----------|---------|--------|-----------------|---------|
| `rgbaPipeline_` | `RGBA8_UNORM` ×1 | pass-through | 1 × RGBA | VSR path |
| `nv12Pipeline_` | `R8_UNORM` + `R8G8_UNORM` ×2 | NV12→RGB BT.601 | 1 × Y + 1 × UV | NO-VSR path |

---

## InteropTexture — Core Mechanism

### Creation Flow

1. `vkCreateImage` with `VkExternalMemoryImageCreateInfo{handleTypes=OPAQUE_FD}`
   - `VK_IMAGE_TILING_LINEAR` (CUDA needs row-linear layout)
   - `VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`
2. `vkGetImageMemoryRequirements` → size, typeBits
3. `vkAllocateMemory` with `VkExportMemoryAllocateInfo{handleTypes=OPAQUE_FD}`
   - Memory type: `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`
4. `vkBindImageMemory`
5. `vkGetImageSubresourceLayout` → `rowPitch` (this is the CUDA-side pitch)
6. `vkGetMemoryFdKHR` → POSIX fd
7. `cuImportExternalMemory` → `extMem`
8. `cuExternalMemoryGetMappedBuffer` → `CUdeviceptr`
9. Create `VkImageView` for descriptor binding

### Why LINEAR not OPTIMAL

OPTIMAL tiling uses implementation-defined swizzle that CUDA cannot write correctly.
LINEAR is row-linear, identical layout to what CUDA expects — `cuMemcpy2DAsync` writes
directly into it. For video playback (a single textured quad, linear sampling), the
performance difference between LINEAR and OPTIMAL is negligible.

### InteropTexture Configs

| Instance | Format | Size | Usage |
|----------|--------|------|-------|
| Y plane | `VK_FORMAT_R8_UNORM` | W × H | `TRANSFER_DST \| SAMPLED` |
| UV plane | `VK_FORMAT_R8G8_UNORM` | W/2 × H/2 | `TRANSFER_DST \| SAMPLED` |
| RGBA | `VK_FORMAT_R8G8B8A8_UNORM` | W × H | `TRANSFER_DST \| SAMPLED` |

`vkGetImageSubresourceLayout` returns the actual `rowPitch` for each image. CUDA copies
use this pitch via `cuMemcpy2DAsync`.

---

## Shaders

### Vertex Shader (shared)

Fullscreen triangle, no vertex buffer. Identical to current `video.vert`.

### Fragment Shader: RGBA pass-through (VSR path)

Identical to current `video.frag`:

```glsl
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = texture(tex, frag_texcoord);
}
```

### Fragment Shader: NV12→RGB (NO-VSR path, NEW)

```glsl
layout(binding = 0) uniform sampler2D texY;
layout(binding = 1) uniform sampler2D texUV;
layout(location = 0) out vec4 out_color;

void main() {
    float y  = texture(texY, frag_texcoord).r;
    vec2  uv = texture(texUV, frag_texcoord).rg;
    float u = uv.r - 0.5;
    float v = uv.g - 0.5;
    // BT.601 full-range
    float r = clamp(y + 1.402 * v, 0.0, 1.0);
    float g = clamp(y - 0.34414 * u - 0.71414 * v, 0.0, 1.0);
    float b = clamp(y + 1.772 * u, 0.0, 1.0);
    out_color = vec4(r, g, b, 1.0);
}
```

---

## MainWindow Changes

`on_timer_tick()` no longer does GPU→CPU→GPU. Instead:

```cpp
void MainWindow::on_timer_tick() {
    AVFrame* frame = decoder_->receive_frame();
    bool is_hw = (frame->format == AV_PIX_FMT_CUDA);

    cuda_ctx_->push();

    if (vsr_) {
        // VSR: NV12→RGB f32 → VSR → D2D → InteropTexture_RGBA
        nv12_to_rgb_->convert(y, yPitch, uv, uvPitch, w, h, rgb_gpu_, stream);
        vsr_->process(rgb_gpu_, &out_ptr, &out_w, &out_h, &out_pitch);
        // D2D into shared Vulkan image
        cuMemcpy2DAsync(rgbaInterop.cudaPtr(), rgbaInterop.cudaPitch(),
                        out_ptr, out_pitch, rowBytes, h, stream);
    } else {
        // NO-VSR: NV12 planes → D2D or H2D → InteropTexture_Y + InteropTexture_UV
        void* dstY  = yInterop.cudaPtr();
        void* dstUV = uvInterop.cudaPtr();
        if (is_hw) {
            cuMemcpy2DAsync(dstY,  yPitch,  frame->data[0], frame->linesize[0], w, h, stream);
            cuMemcpy2DAsync(dstUV, uvPitch, frame->data[1], frame->linesize[1], w/2, h/2, stream);
        } else {
            cuMemcpyHtoDAsync(dstY,  frame->data[0], yBytes, stream);
            cuMemcpyHtoDAsync(dstUV, frame->data[1], uvBytes, stream);
        }
    }
    cuStreamSynchronize(stream);

    cuda_ctx_->pop();
    decoder_->release_frame(frame);

    vulkan_widget_->present_frame(vsr_ ? Path::VSR : Path::NOVSR);
}
```

Key: `present_frame()` no longer takes `data/w/h/is_rgba`. The data is already in the
InteropTextures. It only selects which pipeline to bind and draw.

### Software decode support

`--no-hwaccel` CLI flag forces FFmpeg software decode. NVDEC is not used; frames arrive
as CPU NV12. The NO-VSR path does H2D into InteropTextures. The VSR path does H2D to a
temp CUDA buffer, runs NV12→RGB kernel, then VSR, then D2D to InteropTexture (1 PCIe
write instead of 2 round-trips).

---

## Swapchain Physical Pixels

`SwapchainManager::create()` queries the compositor directly instead of trusting widget
logical dimensions:

```cpp
VkSurfaceCapabilitiesKHR caps;
vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
uint32_t sw = caps.currentExtent.width;
uint32_t sh = caps.currentExtent.height;
// If currentExtent is 0xFFFFFFFF (Wayland "you choose" sentinel),
// fall back to the window size clamped to min/max.
if (sw == 0xFFFFFFFF) {
    sw = clamp(window_w, caps.minImageExtent.width, caps.maxImageExtent.width);
    sh = clamp(window_h, caps.minImageExtent.height, caps.maxImageExtent.height);
}
```

On HiDPI, the compositor reports physical pixel dimensions, so the swapchain renders at
native panel resolution with no compositor-side scale-up.

---

## VSR Path Note

The VSR path retains the CUDA NV12→RGB kernel AND the VSR processor (both run on CUDA).
Only the final RGBA output is D2D'd into the Vulkan InteropTexture. The NV12 shader
approach is only for the NO-VSR path — removing the CUDA kernel from THAT path
specifically. The VSR path needs the CUDA kernel because VSR requires float32 RGB input
on CUDA.

---

## Verification

1. **NO-VSR, HW decode**: `--no-vsr` on a GPU-decodable file → no DtoH in MainWindow → NV12 shader renders correctly, no shear
2. **NO-VSR, SW decode**: `--no-vsr --no-hwaccel` → H2D path works, same visual output
3. **VSR, HW decode**: default mode → D2D only, no DtoH → correct upscaled output
4. **VSR, SW decode**: `--no-hwaccel` → 1 H2D (NV12→GPU) then D2D (VSR output→Vulkan)
5. **Resize**: window resize → swapchain rebuilds at physical pixel extent → letterboxing still correct
6. **Regression**: row pitch handling preserved (LINEAR tiling pitch query + cuMemcpy2DAsync with srcPitch/dstPitch)
