# Deferred Texture Destruction — Design Spec

**Goal:** Prevent NVIDIA driver crash (SIGSEGV/abort) caused by rapid
InteropTexture destroy→create cycles with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`.
Defer destruction of old textures to a future mutation cycle, after the GPU has
provably finished with them.

**Root cause:** NVIDIA driver 535+ has a bug where rapid `vkDestroyImage`→`vkCreateImage`
cycles on external-memory (OPAQUE_FD) images cause memory corruption. A single
`reconfigure_all_textures` call does destroy→create of 3 textures (RGBA+Y+UV), and
the apply loop can execute multiple reconfigure_all_textures back-to-back.

**Evidence:**
```
begin_mutation: waitIdle done    ← GPU drain OK
[CRASH — no end_mutation]        ← destroy old textures → create new → driver bug
```

---

## 1. Current Flow (Crashes)

```
execute(snapshot):
  cuStreamSynchronize
  begin_mutation()
    mutation_gate_=1
    spin-wait render_in_frame_
    vkDeviceWaitIdle
  // destroy old textures, create new textures  ← CRASH here
  reconfigure_all_textures:
    rgba.release() + rgba.init()   ← OPAQUE_FD destroy+create
    y.release()   + y.init()      ← OPAQUE_FD destroy+create
    uv.release()  + uv.init()     ← OPAQUE_FD destroy+create
  end_mutation()
    pipelines_ready_=true, mutation_gate_=0
```

## 2. Deferred Destruction — Design

### Core idea

Destroy textures from cycle N-1 only during cycle N's `begin_mutation` —
after at least one frame has been rendered with cycle N-1's new textures.
This guarantees the GPU is done with the old textures, and separates
destroy from create in time.

### Data flow

```
begin_mutation():
  1. // Destroy textures retired in a PREVIOUS cycle (now safe)
     if frames_since_mutation_ >= 2:
       for tex in retired_: tex.release()
       retired_.clear()

  2. // Move CURRENT textures to retired list (don't destroy yet)
     for tex in active_textures:
       retired_.push_back(std::move(tex))

  3. mutation_gate_ = 1
  4. pipelines_ready_ = false
  5. spin-wait render_in_frame_ → 0
  6. vkDeviceWaitIdle   ← still needed for in-flight GPU work

  [caller creates new textures into active_textures]

end_mutation():
  7. pipelines_ready_ = true
  8. mutation_gate_ = 0
  9. frames_since_mutation_ = 0

record_to_cb (successful frame):
  frames_since_mutation_++    ← render thread signals
```

### Why 2 frames?

GPU queue is FIFO, swapchain is double-buffered (max 2 frames in-flight).
After 2 frames rendered with new textures, all pre-mutation frames have
completed. `vkDeviceWaitIdle` in begin_mutation handles the edge case where
the render thread hasn't produced 2 frames yet.

### Cycle timeline

```
Cycle 1 (first load):
  begin_mutation:
    retired_ = []           ← empty, nothing to destroy
    retired_ = [old_v0]     ← move default-constructed textures
    vkDeviceWaitIdle
  create v1 → active
  end_mutation: frames=0

Frame A: record_to_cb → frames=1
Frame B: record_to_cb → frames=2

Cycle 2 (rapid switch):
  begin_mutation:
    frames >= 2 → destroy [old_v0]  ← safe: 2 frames since cycle 1
    retired_ = [v1]                 ← move current to retired
    vkDeviceWaitIdle
  create v2 → active               ← OPAQUE_FD create ONLY, no destroy
  end_mutation: frames=0
  // No crash — destroy and create are in different cycles
```

---

## 3. Storage for Retired Textures

Each `VideoPipeline` holds its active textures. We add a deferred list per pipeline:

```cpp
// In VideoPipeline or VulkanRenderer:
std::vector<InteropTexture> retired_textures_;  // move-only, destroyed next cycle
std::atomic<int> frames_since_mutation_{0};
```

`InteropTexture` needs to be movable (it currently has only default move semantics
since it holds raw Vulkan/CUDA handles — copying would double-free).

Current InteropTexture members are raw pointers (`VkImage_T*`, etc.) — default
move is fine but we need to reset source to null after move. Add move constructor
and move assignment, or wrap in `std::unique_ptr<InteropTexture>`.

---

## 4. Memory Impact

Each retired texture set: RGBA(~32MB for 1440p) + Y(~2MB) + UV(~1MB) ≈ 35MB.
With 1 set in active + 1 set in retired, peak is double baseline (~70MB extra).
Acceptable for 8GB+ VRAM GPUs.

During rapid switching with no frames rendered (frames=0):
- Cycle 1: retired=[v0], active=v1
- Cycle 2: retired=[v0, v1], active=v2  ← v0+v1 both kept
- Cycle 3: retired=[v0, v1, v2], active=v3  ← 3 sets retired

Worst case: apply loop 4+ cycles with no frames → ~140MB extra. But:
- GPU has 8GB+, this is 1.7% of VRAM
- Cleared as soon as 2 frames render

---

## 5. File Changes

| File | Change |
|---|---|
| `src/core/utils/VideoPipeline.h` | Add `std::vector<InteropTexture> retired_` and `std::atomic<int> frames_since_mutation_` |
| `src/core/utils/VideoPipeline.cpp` | `reconfigure_textures`: move old to `retired_`, create new; `begin_mutation`: destroy retired if frames>=2 |
| `src/core/utils/InteropTexture.h` | Add move constructor/assignment (reset source handles to null) |
| `src/core/utils/VulkanRenderer.h` | Move `frames_since_mutation_` counter to renderer level (shared across pipelines) |
| `src/core/utils/VulkanRenderer.cpp` | `begin_mutation`: destroy retired; `end_mutation`: reset counter; `record_to_cb`: increment counter |

Or simpler: keep it all in `VulkanRenderer` — one retired list for all textures,
one frame counter. Less granular but easier to implement.

---

## 6. Edge Cases

- **VSR disabled:** Same flow — NOVSR path uses Y+UV textures, same OPAQUE_FD interop
- **Scale change only (Path B):** Only RGBA texture changes. Y+UV stay in active.
  Only retired RGBA needs deferred destruction. Per-pipeline retired lists handle this.
- **App shutdown:** Destroy active + retired textures — no frame counter needed
- **apply loop (multiple executes):** Each execute calls begin→reconfigure→end.
  First cycle has retired=[] → creates v1. Second cycle has retired=[v0] → frames=0,
  can't destroy yet → retired=[v0, v1], creates v2. Third cycle same. Eventually
  frames hit 2+ and everything drains.
