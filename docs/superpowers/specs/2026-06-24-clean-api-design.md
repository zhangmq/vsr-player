# Clean API & Resource Lifecycle — Design Spec

**Date:** 2026-06-24
**Status:** Draft
**Branch:** feature/clean-api

## Overview

Refactor the core API for clarity, type safety, and correct resource ownership. The core
must be client-agnostic — Qt Quick is one client; future clients (SDL, GTK, headless)
should integrate without touching core code. The central design rule: **the core owns
playback resources; the client owns Vulkan infrastructure.**

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│ CLIENT (Qt Quick, SDL, GTK, …)                               │
│                                                              │
│ Owns: IVulkanContext impl (VkInstance, VkDevice, Queue, RP)  │
│ Owns: Swapchain + presentation                               │
│                                                              │
│ Per frame (render thread):                                   │
│   1. Begin render pass                                       │
│   2. core->record_frame(cb, w, h)                            │
│   3. End render pass, present                                │
│                                                              │
│ On event (main thread):                                      │
│   core->send_command(CmdResize{...})                         │
│   core->send_command(CmdLoadFile{...})                       │
└──────────────────┬───────────────────────────────────────────┘
                   │  PlayerCommand (variant) ──→
                   │  ←── PlayerEvent (callback)
                   │
┌──────────────────▼───────────────────────────────────────────┐
│ CORE (PlayerCore, worker thread)                             │
│                                                              │
│ Borrows: IVulkanContext* (client-owned, immutable after init) │
│ Owns:   Demuxer, Decoder, VSRProcessor, NV12ToRGB,          │
│         AudioOutput, VulkanRenderer (pipelines + textures),  │
│         CUDAContext, CUDA stream, GPU buffers                │
│                                                              │
│ All playback state lives here. Client sends commands,        │
│ receives events. Core never touches client's swapchain.      │
└──────────────────────────────────────────────────────────────┘
```

## Resource Ownership Contract

### Client owns (core never creates/destroys)

| Resource | Lifetime | Notes |
|----------|----------|-------|
| VkInstance | App lifetime | Created once at startup |
| VkPhysicalDevice | App lifetime | Selected once |
| VkDevice | App lifetime | Created once |
| VkQueue | App lifetime | Graphics queue, same as VkDevice |
| VkCommandPool | App lifetime | Core allocates transient CBs from it |
| VkRenderPass | App lifetime | Compatible with swapchain images |
| Swapchain | App lifetime | Client recreates on resize |
| Window / surface | App lifetime | Client manages |

**Client constraint:** The Vulkan context (all of the above) must be valid for the
entire lifetime of PlayerCore. No rebuilding. No re-creation. If the client needs to
rebuild the swapchain (e.g. window resize), it must do so transparently — core
never knows about it.

### Core owns (client never touches)

| Resource | Lifetime | Notes |
|----------|----------|-------|
| Demuxer | Per-file | Destroyed on LOAD_FILE or QUIT |
| Decoder | Per-file | Destroyed on LOAD_FILE or QUIT |
| AudioOutput | Per-file | Destroyed on LOAD_FILE or QUIT |
| VSRProcessor | Per-session | Reconfigured, not rebuilt, on scale/quality change |
| NV12ToRGB | Per-session | CUDA kernel, compiled once |
| CUDAContext | Per-session | Wraps client's CUDA context |
| CUDA stream | Per-session | Created once, reused |
| `rgb_gpu_` buffer | Per-file | Size depends on video dimensions |
| VulkanRenderer | Per-session | Pipelines + InteropTextures, reconfigured not rebuilt |
| VideoPipeline (×2) | Per-session | Shaders + pipeline layout stable; textures reconfigured |
| InteropTextures | Per-session | RGBA resized on scale change; NV12 resized on video dims change |
| Descriptor sets | Per-session | Updated via vkUpdateDescriptorSets on texture change |

### Thread boundary

| Thread | Access |
|--------|--------|
| **Worker** (run_loop) | Read/write all core-owned resources. Send events. |
| **Render** (Qt) | Read-only: `record_frame()` → `record_to_cb()` → bind + draw. Must never mutate core state. |
| **Main** (Qt event loop) | Send commands via `send_command()` (thread-safe). Receive events via callback (marshaled by client). |

## IVulkanContext — Client-Provided Interface

```cpp
// Implemented by client. All methods return borrows — core never
// stores these pointers beyond the duration of a single call.
// The underlying Vulkan objects live for the app's lifetime.

class IVulkanContext {
public:
    virtual ~IVulkanContext() = default;

    // ── Resource handles — core uses as factory, never modifies ──
    virtual void* vkInstance()       const = 0;
    virtual void* vkPhysicalDevice() const = 0;
    virtual void* vkDevice()         const = 0;
    virtual void* vkQueue()          const = 0;
    virtual int   vkQueueFamily()    const = 0;
    virtual void* vkCommandPool()    const = 0;
    virtual void* vkRenderPass()     const = 0;

    // ── Synchronization — affects client's shared queue/device ──
    virtual void waitIdle() const = 0;
    virtual void submitAndWait(void* commandBuffer) const = 0;
};
```

**Design note:** `void*` is intentional — avoids `<vulkan/vulkan.h>` in the public
header. Core internally casts to `VkDevice` etc. Client never includes Vulkan headers
unless it wants to.

## PlayerCommand — Typed Variant

```cpp
// ── Zero-argument commands ──
struct CmdPlay  {};
struct CmdPause {};
struct CmdStop  {};   // VLC-style: stops playback, seeks to position 0,
                       // resets clock_bias_ and PTS sync state,
                       // keeps demuxer/decoder/audio loaded — play resumes from start.
struct CmdQuit  {};

// ── Commands with payload ──
struct CmdLoadFile   { std::string path; };
struct CmdSeek       { int64_t position_ms; };
struct CmdResize     { int phys_w; int phys_h; };
struct CmdSetQuality { Quality level; };
struct CmdSetScale   { int scale; };          // 0=auto, 1-4=locked
struct CmdSetVolume  { double value; };       // 0.0 - 1.0
struct CmdSetMute    { bool muted; };
struct CmdSetVsr     { bool enabled; };       // toggle AI super-resolution
struct CmdSetHwaccel { bool enabled; };       // toggle NVDEC hardware decoding

using PlayerCommand = std::variant<
    CmdPlay, CmdPause, CmdStop, CmdQuit,
    CmdLoadFile, CmdSeek, CmdResize,
    CmdSetQuality, CmdSetScale, CmdSetVolume, CmdSetMute,
    CmdSetVsr, CmdSetHwaccel
>;
```

**No field overloading.** Each command carries exactly the parameters it needs.
`CmdResize` has dedicated `phys_w`/`phys_h` — no more repurposing `seek_ms`/`volume`.

## PlayerEvent — Unchanged Structure, Cleaned Up

```cpp
// Events are unchanged in structure but emitted more precisely.

struct PlayerEvent {
    enum Type {
        STATE_CHANGED,      // playing → paused → stopped
        POSITION_CHANGED,   // time update (~4 Hz)
        VIDEO_INFO,         // after successful load: dimensions, fps, codec
        ERROR,              // load/open/decode failure
        END_OF_FILE,        // natural EOF (not after STOP)
    };
    Type type;

    PlaybackState state;    // STATE_CHANGED
    int64_t time_ms = 0;    // POSITION_CHANGED
    int64_t duration_ms = 0;

    int in_width = 0, in_height = 0;   // VIDEO_INFO
    int out_width = 0, out_height = 0;
    double fps = 0.0;
    int scale = 1;
    Quality quality = Quality::HIGH;
    bool hw_decoding = false;
    bool vsr_active = false;
    bool has_audio = false;
    bool seekable = true;

    std::string error_msg;  // ERROR
};

using EventCallback = std::function<void(const PlayerEvent&)>;
```

## Player — Public Interface

```cpp
class Player {
public:
    virtual ~Player() = default;

    /// Initialize with a client-provided Vulkan context.
    /// Context must outlive the Player. Called once.
    /// Configures VSR/quality/hwaccel preferences.
    virtual bool initialize(
        IVulkanContext* vk,
        bool use_vsr = true,
        Quality quality = Quality::HIGH,
        bool no_hwaccel = false) = 0;

    /// Enqueue a command. Thread-safe.
    virtual void send_command(PlayerCommand cmd) = 0;

    /// Record video draw commands into an active command buffer.
    /// Called from client's render thread, inside an already-begun render pass.
    /// @param cb  VkCommandBuffer (void* to avoid header dep)
    /// @param w   Render area width (physical pixels)
    /// @param h   Render area height (physical pixels)
    virtual void record_frame(void* cb, int w, int h) = 0;

    /// Set event callback. Events fire from worker thread;
    /// client must marshal to its own event loop.
    virtual void set_event_callback(EventCallback cb) = 0;

    /// Shut down: stops playback, joins worker thread,
    /// releases all core-owned resources. Client-owned
    /// Vulkan resources are NOT touched.
    virtual void shutdown() = 0;
};
```

## Resource Lifecycle — State Machine

```
                    initialize()
                         │
                         ▼
          ┌──────────────────────────┐
          │       STOPPED             │
          │  (no file loaded)         │
          └──────────┬───────────────┘
                     │ LOAD_FILE
                     ▼
          ┌──────────────────────────┐
          │       LOADING             │
          │  build_pipeline()         │
          │  → Demuxer open           │
          │  → Decoder init           │
          │  → VSR init (adaptive     │
          │    scale from last        │
          │    known window size)     │
          │  → Renderer init_external │
          │    (if first load) or     │
          │    reconfigure (if switch)│
          └──────────┬───────────────┘
                     │ VIDEO_INFO event
                     ▼
          ┌──────────────────────────┐
          │       PLAYING             │◄──── PLAY
          │  process_one_frame()      │
          │  → decode → NV12→RGB     │────► PAUSE
          │  → VSR → D2D → frame_    │
          │    ready_ = true          │
          └──────────┬───────────────┘
                     │ END_OF_FILE
                     ▼
          ┌──────────────────────────┐
          │   END_OF_FILE             │
          │  emit event               │
          │  (client may LOAD_FILE    │
          │   for next playlist item) │
          └──────────┬───────────────┘
                     │ LOAD_FILE / STOP / QUIT
                     ▼
          ┌──────────────────────────┐
          │       STOPPED             │
          │  STOP: flush decoders     │
          │  QUIT: teardown all       │
          └──────────────────────────┘
```

### Command effects on resources

| Command | Demuxer/Decoder | VSR | Renderer | Audio | Notes |
|---------|-----------------|-----|----------|-------|-------|
| PLAY | — | — | — | start/resume | State change only |
| PAUSE | — | — | — | pause | State change only |
| STOP | flush | — | — | stop | No resource destruction |
| LOAD_FILE (first) | create new | init | init_external | create | Full init (pipeline + textures) |
| LOAD_FILE (switch) | destroy old → create new | reconfigure | reconfigure_textures (both pipelines) | destroy old → create new | Textures mutated; pipeline untouched |
| RESIZE (scale Δ) | — | reconfigure | reconfigure_scale (RGBA only) | — | Only RGBA texture |
| RESIZE (scale =) | — | — | — | — | Viewport only |
| SET_QUALITY | — | reconfigure | — | — | VSR only |
| SET_SCALE (locked) | — | reconfigure | reconfigure_scale (RGBA only) | — | Ignores future RESIZE scale Δ |
| SET_SCALE (auto) | — | reconfigure | reconfigure_scale (RGBA only) | — | Unlocks; RESIZE resumes adaptive |
| SET_VSR | — | — | — | — | Flag only; VSR stays alive either way |
| SET_HWACCEL | — | — | — | — | Flag only; applies on next LOAD_FILE |
| SEEK | flush | — | — | seek | Decoder state only |
| QUIT | destroy | destroy | destroy | destroy | Everything |

### LOAD_FILE texture mutation principle

On video switch, InteropTextures are **always** rebuilt — the new file has different
dimensions, and even if they happen to match, the optimization isn't worth the
complexity. The Vulkan pipeline (shaders, pipeline layout, descriptor set layout,
sampler) is **never** rebuilt. This is a texture mutation, not a pipeline rebuild:

```
LOAD_FILE (switch):
  1. Destroy old: demuxer_, decoder_, audio_, rgb_gpu_
  2. Create new: demuxer, decoder, rgb_gpu_, audio
  3. VSR: if input dims changed (new video resolution) →
       vsr_.reset() + init(new_in_w, new_in_h, ...)
       (VSR input image is allocated at in_w×in_h — must recreate
        when dimensions change; reconfigure() preserves input dims)
     if input dims unchanged (same file re-load) →
       reconfigure()
  4. Renderer: destroy old InteropTextures (both pipelines) →
     create new at new sizes →
     vkUpdateDescriptorSets (update, don't recreate) →
     pipelines, shaders, sampler all stay
  5. Renderer resize (viewport update)
```

**VulkanRenderer API for texture mutation — two methods, two purposes:**

```cpp
// RESIZE / SET_SCALE: only RGBA texture changes (NV12 is scale-independent)
bool reconfigure_scale(int videoW, int videoH, int newScale);

// LOAD_FILE: both pipelines' textures change (new video dimensions)
bool reconfigure_all_textures(int videoW, int videoH, int scale);
```

Both mutate textures + update descriptors without touching pipeline/shader/sampler.
`init_external` (full pipeline creation) is called only once, on first load.

### VSR init scale — compute once, not twice

`build_pipeline()` currently inits VSR at 1x, then `cmd_resize()` immediately
reconfigures to the adaptive scale. This wastes a warmup cycle (~30ms).

Fix: `build_pipeline()` reads `pending_phys_w_/h_` (if RESIZE arrived first) or
`last_phys_w_/h_` (from previous session) to compute the correct scale before
calling `vsr_->init()`. If no window size is available, default to 1x.

## Per-Event Resource Handling & Atomicity

Each command handler must follow a strict pattern:

1. **Validate** all pre-conditions before any mutation
2. **Mutate** resources in dependency order
3. **Guarantee** a consistent final state — success or clean failure
4. **Never** leave resources partially updated

### Atomicity Model

The worker thread processes commands sequentially (single consumer of CommandQueue).
No two commands execute concurrently. This means atomicity is per-command:
either the entire handler succeeds, or it fails and leaves state unchanged.

For commands that mutate resources, the critical invariant is:

> After any command returns, `renderer_->is_ready()` implies all core resources
> (demuxer, decoder, VSR, renderer textures) are consistent with each other.
> If `is_ready()` is false, no playback can occur but the player is in a
> defined state awaiting LOAD_FILE.

### LOAD_FILE — atomic new-file transition

**Pre-conditions:** None (works from any state)

**Mutations (in order):**

```
Phase 1 — Acquire new file resources (no mutation to existing state):
  1. demuxer_->open(path) → validate file is readable, extract codec info
  2. decoder_->open(codecpar) → validate codec is decodable
  3. Compute scale from last_phys_w_/h_ (or pending_phys_w_/h_)
  4. Compute video_w_/h_ from demuxer

Phase 2 — Build new GPU resources (alongside existing if switching):
  5. CUDA context push + stream create (if first load)
  6. nv12_to_rgb_->compile() (if first load)
  7. cuMemAlloc rgb_gpu_ at new video dimensions
  8. VSR init or reconfigure at new dimensions + scale
  9. AudioOutput open new audio stream

Phase 3 — Renderer transition (mutate textures, not pipeline):
  10. If first load: renderer_->init_external(...)  — full pipeline creation
  11. If switching: renderer_->reconfigure_all_textures(...) — textures only

Phase 4 — Commit:
  12. Destroy old demuxer/decoder/audio/rgb_gpu_ (if switching)
  13. Swap new resources into members
  14. Set state_ = STOPPED
  15. Emit VIDEO_INFO event
```

**Failure recovery:** If any step in Phase 1-3 fails:
- Destroy all resources allocated in the current phase
- Old resources (if switching) remain intact — player continues with previous file
- Emit ERROR event
- Player is still in a valid state (old file loaded, or STOPPED if first load)

**Post-condition (success):** New file fully loaded. `demuxer_`, `decoder_`, `vsr_`, `renderer_`, `rgb_gpu_` all consistent. Ready to play.

**Post-condition (failure):** State unchanged from before LOAD_FILE. If switching, old file still loaded. If first load, clean STOPPED state.

**Key design choice:** Phase 1-3 resources are created in local variables first.
Only on success (Phase 4) are they moved into the player's member variables
and old resources destroyed. This ensures all-or-nothing atomicity.

### Scale Mode: Auto vs Locked

VSR scale has two modes controlled by `user_scale_`:

| `user_scale_` | Mode | Behavior |
|---------------|------|----------|
| 0 | Auto | RESIZE computes adaptive scale from window/video ratio |
| 1–4 | Locked | Scale fixed; RESIZE only updates viewport |

SET_SCALE with `scale == 0` unlocks auto mode and immediately re-computes
adaptive scale from the last known window size.

**Transitions:**
```
         SET_SCALE(n>0)           RESIZE
  AUTO ────────────────→ LOCKED    (only viewport)
                         ↓ SET_SCALE(0)
                         ↓
                         AUTO ←─── RESIZE computes & applies adaptive scale
```

This resolves the SET_SCALE/RESIZE conflict: user intent (explicit scale) always
wins over automatic adaptation. The mode survives LOAD_FILE — if user locked
scale at 2x, the next video also uses 2x.

### RESIZE — viewport or texture-only mutation

**Pre-conditions:** Demuxer and renderer exist (return early if not)

**Mutations (in order):**

```
1. Store phys_w/h as last_phys_w_/h_ (always)
2. If user_scale_ > 0 (locked):
     → renderer_->resize(phys_w, phys_h)  — viewport only
     → return
3. Compute adaptive scale (see Adaptive Scale Algorithm below)
4. If scale unchanged:
     → renderer_->resize(phys_w, phys_h)  — viewport only
     → return
5. If scale changed:
     → current_scale_ = new_scale
     → vsr_->reconfigure(vsr_w_, vsr_h_, quality_)   — VSR output size
     → renderer_->reconfigure_scale(...)              — RGBA texture only
     → renderer_->resize(phys_w, phys_h)             — viewport
```

**Failure recovery:** `reconfigure_scale` internally gates with `pipelines_ready_` +
`vkDeviceWaitIdle`. If it fails, RGBA texture is cleared (no dangling descriptor).
Render thread sees `pipelines_ready_ = false` and skips drawing.

**Atomicity boundary:** The `pipelines_ready_` flag acts as the atomicity gate
for the render thread. Steps 5a-5c execute sequentially without interruption.

### Adaptive Scale Algorithm

The viewport uses letterboxing: `sc = min(ww/vsr_w, wh/vsr_h)`. The constrained
dimension determines display resolution. VSR output in that dimension must be
≥ display pixels for 1:1 quality.

```
vw/vh ≥ ww/wh  →  width-constrained  (video wider than window)
vw/vh < ww/wh  →  height-constrained (video taller than window)

if vw/vh >= ww/wh:   s = clamp(ceil(ww / vw), 1, 4)
else:                s = clamp(ceil(wh / vh), 1, 4)
```

**Verification:**

| Video | Window | 约束 | s | VSR→显示比 |
|-------|--------|------|---|-----------|
| 480×854 (9:16) | 1920×1080 (16:9) | 高度 | 2 | 1.58× ✓ |
| 640×480 (4:3) | 1920×1080 (16:9) | 高度 | 3 | 1.33× ✓ |
| 1920×1080 (16:9) | 3840×2160 (16:9) | — | 2 | 1.0× ✓ |
| 1920×1080 (16:9) | 1024×768 (4:3) | 宽度 | 1 | 1.88× ✓ |
| 1080×1080 (1:1) | 1920×1080 (16:9) | 高度 | 1 | 1.0× ✓ |

Core holds `user_scale_`: 0 = auto (algorithm above), 1-4 = locked (skip RESIZE scale).

### SET_SCALE — lock/unlock user scale

**Pre-conditions:** VSR and renderer exist (return early if not)

**Mutations:**

```
1. Validate: scale in [0, 4], scale != user_scale_ (return if unchanged)
2. user_scale_ = scale
3. If scale > 0 (locked):
     → current_scale_ = scale
   Else (scale == 0, auto):
     → compute adaptive: clamp(max(ceil(phys_w/video_w), ceil(phys_h/video_h)), 1, 4)
     → current_scale_ = computed_scale
4. vsr_->reconfigure(vsr_w_, vsr_h_, quality_)       — VSR output
5. renderer_->reconfigure_scale(...)                  — RGBA texture only
6. Emit FRAME_INFO (new scale)
```

**Design note:** SET_SCALE with `scale == 0` restores auto mode. Any RESIZE
after this will resume computing adaptive scale. SET_SCALE with `scale ==
current_scale_` is a no-op (no resource mutation).

### SET_VSR — toggle AI super-resolution

**Pre-conditions:** None

**Mutations:**
```
1. use_vsr_ = enabled  (flag only — no resource creation or destruction)
2. VSR processor stays initialized regardless of toggle state.
   process_one_frame checks use_vsr_ to decide VSR vs NOVSR path.
```

**Design note:** VSR facility is session-level — created once on first LOAD_FILE,
destroyed only by QUIT. Toggling off just skips the VSR step in process_one_frame;
the processor and its GPU resources remain alive for instant re-enable.

### SET_HWACCEL — toggle hardware decoding

**Pre-conditions:** None

**Mutations:**
```
1. no_hwaccel_ = !enabled  (flag only)
2. Applies on next LOAD_FILE. No resource mutation.
```

**Design note:** Decoder (HW or SW) is a per-file resource. Changing hwaccel
requires reopening the codec, which is a LOAD_FILE-level operation. Flag-only
design keeps SET_HWACCEL trivial. Client can follow with LOAD_FILE to apply
immediately.

### SET_QUALITY — VSR-only mutation

**Pre-conditions:** VSR exists and is ready

**Mutations:**
```
1. quality_ = new_value
2. vsr_->reconfigure(vsr_w_, vsr_h_, quality_)
```

No renderer involvement. No `pipelines_ready_` gate needed — VSR state is only
consumed by the worker thread (in `process_one_frame`).

### SEEK — decoder flush only

**Pre-conditions:** Demuxer exists and file is seekable

**Mutations:**
```
1. decoder_->flush()
2. audio_->seek(position_seconds)
3. frame_count_ = estimated frame at target position
```

No resource creation or destruction. State is consistent — decoder and audio
are both positioned to the same target time.

### STOP — playback state only

**Pre-conditions:** None (idempotent)

**Mutations:**
```
1. state_ = STOPPED
2. audio_->stop()
3. Emit STATE_CHANGED event
```

No resource destruction. Pipeline, demuxer, decoder all stay alive.
If LOAD_FILE follows, the soft-switch reuses what it can.

### PLAY / PAUSE — state transition only

**Pre-conditions:** Demuxer exists (PLAY), none (PAUSE)

**Mutations:**
```
1. state_ = PLAYING / PAUSED
2. audio_->start() / resume() / pause()
3. Emit STATE_CHANGED event
```

### QUIT — full teardown

**Pre-conditions:** None

**Mutations:**
```
1. running_ = false  (exits run_loop)
2. run_loop exit path: teardown_pipeline()
   → cuda_ctx_->push()            ← CUDA context must be active for interop teardown
   → renderer_->release()         ← handles interop internally (see below)
        pipelines_ready_ = false
        vkDeviceWaitIdle          ← drain GPU first
        rgbaPipeline_.release()   → per-texture:
           cuDestroyExternalMemory  ← 1. CUDA (imported from Vulkan, must go first)
           vkDestroyImageView       ← 2. Vulkan view
           vkDestroyImage           ← 3. Vulkan image
           vkFreeMemory             ← 4. Vulkan memory (CUDA no longer references it)
        nv12Pipeline_.release()   → same per-texture order
   → audio_->stop()
   → vsr_.reset()
   → nv12_to_rgb_.reset()
   → cuMemFree(rgb_gpu_)          ← standalone CUDA
   → cuStreamDestroy(cuda_stream_)
   → decoder_.reset()
   → demuxer_.reset()
   → cuda_ctx_->pop()
   → cuda_ctx_.reset()            ← LAST — all CUDA operations complete
3. Emit STATE_CHANGED(STOPPED)
```

**Interop texture destroy constraint:** CUDA external memory imported the allocation
from Vulkan. If Vulkan memory is freed first, the CUDA handle becomes a dangling
reference → UB on `cuDestroyExternalMemory`. Both VkDevice and CUDA context must
be alive during InteropTexture release.

**Global destroy order:**
1. `vkDeviceWaitIdle` — drain GPU before touching any shared resources
2. InteropTextures — CUDA external → Vulkan image → Vulkan memory (per texture)
3. Standalone CUDA — `cuMemFree`, `cuStreamDestroy`
4. FFmpeg — decoder, demuxer
5. CUDA context — last, after all CUDA API calls

### `teardown_pipeline()` — single call site

```
teardown_pipeline() is called ONLY from:
  1. run_loop() exit path (after QUIT received)
  2. ~PlayerCore() destructor

It destroys ALL core-owned resources in reverse creation order:
  renderer_ → audio_ → vsr_ → nv12_to_rgb_ → rgb_gpu_ →
  cuda_stream_ → decoder_ → demuxer_ → cuda_ctx_
```

## File Change Matrix

| File | Action | Summary |
|------|--------|---------|
| `src/core/api/Player.h` | **Rewrite** | IVulkanContext, variant PlayerCommand, Player interface |
| `src/core/PlayerCore.h` | **Rewrite** | New resource lifecycle, variant dispatch |
| `src/core/PlayerCore.cpp` | **Rewrite** | Soft-switch cmd_load_file, no-teardown cmd_stop, cmd_set_scale fix |
| `src/core/utils/VulkanRenderer.h` | **Modify** | Remove VulkanContext ownership; take IVulkanContext& in methods |
| `src/core/utils/VulkanRenderer.cpp` | **Modify** | No ctx_ member; methods receive IVulkanContext& |
| `src/core/utils/VulkanContext.h` | **Delete** | Replaced by IVulkanContext in Player.h |
| `src/core/utils/VulkanContext.cpp` | **Delete** | Client implements IVulkanContext |
| `src/client/main.cpp` | **Modify** | Implement QtVulkanContext : IVulkanContext; use variant commands |
| `src/core/CommandQueue.h` | **No change** | Already generic |
| `src/core/utils/VideoPipeline.h/cpp` | **Modify** | init()/reconfigure_textures() take IVulkanContext&; sync via submitAndWait() |
| `src/core/utils/InteropTexture.h/cpp` | **No change** | Already clean |
| `src/core/utils/SwapchainManager.h/cpp` | **Dead code** | Compiles; core no longer uses |

## Thread Safety

- `send_command()`: locks CommandQueue mutex, notifies cv — thread-safe
- `record_frame()`: reads `frame_ready_` (atomic) + `renderer_` (pointer) + `pipelines_ready_` (bool). Guarded by `pipelines_ready_` check before accessing pipeline resources.
- `run_loop()`: single consumer of CommandQueue. Processes all pending commands before each frame.
- Event callback: fires from worker thread. Client must marshal (Qt: `QMetaObject::invokeMethod` with `Qt::QueuedConnection`).

## Design Notes

### Adaptive scale on first load

The core needs window dimensions to compute adaptive VSR scale. Ordering:

```
Client sends:  CmdResize{phys_w, phys_h}    ← before or after LOAD_FILE
Client sends:  CmdLoadFile{path}

If RESIZE arrives before demuxer is ready → stored as pending_phys_w/h_
If LOAD_FILE completes first → build_pipeline at scale=1 → then pending RESIZE applied
If RESIZE arrives first → stored → LOAD_FILE → build_pipeline → cmd_resize with stored size
```

Core holds `pending_phys_w_/h_` for the pre-demuxer window, and `last_phys_w_/h_` for
re-application on video switch. Client sends RESIZE whenever window size changes
(debounced). Core is tolerant of either ordering.

### SwapchainManager removal from core

`SwapchainManager` is only used in the old self-managed Vulkan mode (`init()` +
`render_frame()`). In the new design, core only supports external mode
(`IVulkanContext` provided by client). `SwapchainManager`:

- Remains in the codebase (compiles, no warnings) but is dead code
- `VulkanRenderer` no longer owns a `SwapchainManager` member
- `VulkanRenderer::init()`, `init_pipelines()`, `render_frame()`, `set_shader_data()`,
  `init_pipelines_with_saved_spv()` are removed
- Only `init_external()`, `record_to_cb()`, `release()`, `reconfigure_scale()` remain

### IVulkanContext: void* is intentional

Using `void*` instead of `VkDevice`/`VkInstance` in the public header avoids pulling
`<vulkan/vulkan.h>` into `Player.h`. The core internally casts; the client casts when
implementing the interface. This keeps the public header dependency-free.

## Verification

1. Build: `make -j$(nproc)` — zero warnings
2. Playback: `./build/vsr-player input --quality ULTRA` — plays, resizes, switches videos
3. API check: `grep -r 'VulkanContext' src/core/` returns nothing (only IVulkanContext)
4. Resource check: `grep -r 'teardown_pipeline' src/core/` returns only run_loop exit + destructor
5. Delete old paths: `grep -r 'init_pipelines_with_saved_spv\|set_shader_data\|swapchain_' src/core/` returns nothing
