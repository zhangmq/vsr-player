# Core Command Coalescing v2 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace command-driven coalescing with a state-driven TargetState model. All heavy events merge into a target state snapshot; `apply()` diffs current vs target and executes mutations, looping until target stabilizes. Fix `vkDeviceWaitIdle` blocking Qt UI rendering.

**Architecture:** Worker thread owns `TargetState` + `busy_` flag. `dispatch()` routes light commands for immediate execution, heavy commands merge into `target_state_`. RESIZE has an 800ms pre-filter debounce in `run_loop`. `apply()` loops: snapshot target → clear → execute snapshot → check if new events arrived during execution. Client receives full target state via `OPERATION_PENDING` for optimistic UI.

**Tech Stack:** C++20, Vulkan, CUDA, Qt 6

---

## Task 1: Player.h — Add OPERATION_PENDING + queueWaitIdle

**Files:**
- Modify: `src/core/api/Player.h`

- [ ] **Step 1: Add OPERATION_PENDING event type**

In `PlayerEvent::Type` enum, add the new type:

```cpp
struct PlayerEvent {
    enum Type {
        STATE_CHANGED,
        POSITION_CHANGED,
        VIDEO_INFO,
        TRACKS_INFO,
        FRAME_INFO,
        ERROR,
        END_OF_FILE,
        FRAME_CAPTURED,
        OPERATION_PENDING,   // ← new
    };
    Type type = STATE_CHANGED;

    // ... existing fields unchanged ...

    // OPERATION_PENDING fields (full target state)
    std::string pending_file;
    int pending_scale = -1;
    int pending_quality = -1;
    int pending_phys_w = 0;
    int pending_phys_h = 0;
};
```

- [ ] **Step 2: Add queueWaitIdle() to IVulkanContext**

```cpp
class IVulkanContext {
public:
    virtual ~IVulkanContext() = default;

    virtual void* vkInstance()       const = 0;
    virtual void* vkPhysicalDevice() const = 0;
    virtual void* vkDevice()         const = 0;
    virtual void* vkQueue()          const = 0;
    virtual int   vkQueueFamily()    const = 0;
    virtual void* vkCommandPool()    const = 0;
    virtual void* vkRenderPass()     const = 0;

    virtual void waitIdle() const = 0;
    virtual void queueWaitIdle() const = 0;   // ← new: vkQueueWaitIdle, NOT vkDeviceWaitIdle
    virtual void submitAndWait(void* commandBuffer) const = 0;
};
```

- [ ] **Step 3: Verify build still compiles**

Run: `cd build && cmake .. && make -j$(nproc)`
Expected: success. (headers used by all files, but new virtual method needs QtVulkanContext override in Task 2.)

- [ ] **Step 4: Commit**

```bash
git add src/core/api/Player.h
git commit -m "feat: add OPERATION_PENDING event type and queueWaitIdle to IVulkanContext"
```

---

## Task 2: QtVulkanContext — Implement queueWaitIdle

**Files:**
- Modify: `src/client/QtVulkanContext.h`
- Modify: `src/client/QtVulkanContext.cpp`

- [ ] **Step 1: Declare queueWaitIdle in header**

Add to `QtVulkanContext.h` after the `waitIdle()` declaration:

```cpp
void queueWaitIdle() const override;
```

- [ ] **Step 2: Implement queueWaitIdle in .cpp**

Add to `QtVulkanContext.cpp` after `waitIdle()` (line ~58):

```cpp
void QtVulkanContext::queueWaitIdle() const {
    VkQueue queue = (VkQueue)vkQueue();
    if (!queue) return;

    auto* rif = view_->rendererInterface();
    void* rdev = rif->getResource(view_, QSGRendererInterface::DeviceResource);
    auto* vi = static_cast<QVulkanInstance*>(
        rif->getResource(view_, QSGRendererInterface::VulkanInstanceResource));
    auto* vkdf = vi->deviceFunctions(*static_cast<VkDevice*>(rdev));
    vkdf->vkQueueWaitIdle(queue);
}
```

- [ ] **Step 3: Build and verify**

```bash
cd build && make -j$(nproc)
```
Expected: success. `queueWaitIdle` override satisfies the new pure virtual.

- [ ] **Step 4: Commit**

```bash
git add src/client/QtVulkanContext.h src/client/QtVulkanContext.cpp
git commit -m "feat: implement queueWaitIdle via vkQueueWaitIdle"
```

---

## Task 3: VulkanRenderer — vkDeviceWaitIdle → vkQueueWaitIdle

**Files:**
- Modify: `src/core/utils/VulkanRenderer.cpp`

- [ ] **Step 1: Fix begin_mutation sync call**

Replace `vk.waitIdle()` with `vk.queueWaitIdle()` at line 156:

```cpp
void VulkanRenderer::begin_mutation(IVulkanContext& vk) {
    mutation_gate_.store(1, std::memory_order_release);
    pipelines_ready_ = false;

    while (render_in_frame_.load(std::memory_order_acquire) > 0) {
        if (running_flag_ && !running_flag_->load(std::memory_order_acquire))
            return;
        std::this_thread::yield();
    }

    vk.queueWaitIdle();  // was: vk.waitIdle() — only drain our queue, not Qt's
}
```

- [ ] **Step 2: Fix release() sync call**

Replace `vk.waitIdle()` with `vk.queueWaitIdle()` at line 130:

```cpp
void VulkanRenderer::release(IVulkanContext& vk) {
    VkDevice dev = (VkDevice)vk.vkDevice();
    if (!dev) return;

    pipelines_ready_ = false;
    vk.queueWaitIdle();  // was: vk.waitIdle()

    rgbaPipeline_.release(dev);
    nv12Pipeline_.release(dev);
    // ...
}
```

- [ ] **Step 3: Build**

```bash
cd build && make -j$(nproc)
```
Expected: success.

- [ ] **Step 4: Commit**

```bash
git add src/core/utils/VulkanRenderer.cpp
git commit -m "fix: replace vkDeviceWaitIdle with vkQueueWaitIdle — don't block Qt rendering"
```

---

## Task 4: InteropTexture — Remove redundant vkDeviceWaitIdle

**Files:**
- Modify: `src/core/utils/InteropTexture.cpp`

- [ ] **Step 1: Remove vkDeviceWaitIdle from release()**

Delete lines 229-230:

```cpp
void InteropTexture::release() {
    VkDevice device = (VkDevice)device_;

    // REMOVED: vkDeviceWaitIdle(device) — redundant, begin_mutation already drains

    if (extMem_) {
        cuDestroyExternalMemory((CUexternalMemory)extMem_);
        extMem_ = nullptr;
    }
    // ... rest unchanged
```

- [ ] **Step 2: Build**

```bash
cd build && make -j$(nproc)
```
Expected: success.

- [ ] **Step 3: Commit**

```bash
git add src/core/utils/InteropTexture.cpp
git commit -m "fix: remove redundant vkDeviceWaitIdle from InteropTexture::release()"
```

---

## Task 5: PlayerCore.h — Add TargetState + New Members

**Files:**
- Modify: `src/core/PlayerCore.h`

- [ ] **Step 1: Add TargetState struct before PlayerCore class**

Insert after the forward declarations (line ~22):

```cpp
// ── TargetState — merged user intent snapshot ──
struct TargetState {
    std::string file;
    int phys_w = 0;
    int phys_h = 0;
    int scale = -1;    // -1 = unset, 0=auto, 1-4=locked
    int quality = -1;  // -1 = unset

    bool active() const {
        return !file.empty() || phys_w > 0 || scale >= 0 || quality >= 0;
    }

    void clear() { file.clear(); phys_w = phys_h = 0; scale = quality = -1; }

    void merge(const PlayerCommand& cmd) {
        std::visit([this](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, CmdLoadFile>) {
                file = arg.path;
                phys_w = phys_h = 0;
                scale = quality = -1;
            } else if constexpr (std::is_same_v<T, CmdResize>) {
                phys_w = arg.phys_w; phys_h = arg.phys_h;
            } else if constexpr (std::is_same_v<T, CmdSetScale>) {
                scale = arg.scale;
            } else if constexpr (std::is_same_v<T, CmdSetQuality>) {
                quality = (int)arg.level;
            }
        }, cmd);
    }
};
```

- [ ] **Step 2: Add new members to PlayerCore**

Add these private members after the existing `current_path_` field:

```cpp
// ── Command coalescing ──
bool busy_ = false;
TargetState target_state_;

// RESIZE debounce — core-side 800ms timer (worker thread)
int stored_resize_w_ = 0;
int stored_resize_h_ = 0;
std::chrono::steady_clock::time_point last_resize_cmd_;

// ── New methods ──
bool is_light(const PlayerCommand& cmd) const;
void emit_optimistic();
void apply();
bool execute(const TargetState& snapshot);
```

- [ ] **Step 3: Build**

```bash
cd build && make -j$(nproc)
```
Expected: success for header changes, link errors for unimplemented methods (expected — implemented in next tasks).

- [ ] **Step 4: Commit**

```bash
git add src/core/PlayerCore.h
git commit -m "feat: add TargetState struct and coalescing members to PlayerCore"
```

---

## Task 6: PlayerCore.cpp — dispatch + is_light Rewrite

**Files:**
- Modify: `src/core/PlayerCore.cpp`

- [ ] **Step 1: Implement is_light()**

Add after `emit_event()` (after line ~105):

```cpp
bool PlayerCore::is_light(const PlayerCommand& cmd) const {
    return std::visit([](auto&& arg) -> bool {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, CmdPlay>)       return true;
        if constexpr (std::is_same_v<T, CmdPause>)      return true;
        if constexpr (std::is_same_v<T, CmdStop>)       return true;
        if constexpr (std::is_same_v<T, CmdQuit>)       return true;
        if constexpr (std::is_same_v<T, CmdSeek>)       return true;
        if constexpr (std::is_same_v<T, CmdSetVolume>)  return true;
        if constexpr (std::is_same_v<T, CmdSetMute>)    return true;
        if constexpr (std::is_same_v<T, CmdSetVsr>)     return true;
        if constexpr (std::is_same_v<T, CmdSetHwaccel>) return true;
        // Heavy:
        //   CmdLoadFile, CmdResize, CmdSetScale, CmdSetQuality
        return false;
    }, cmd);
}
```

- [ ] **Step 2: Rewrite dispatch()**

Replace the existing `dispatch()` (lines 137-168) with:

```cpp
void PlayerCore::dispatch(const PlayerCommand& cmd) {
    // ── Light: execute immediately ──
    if (is_light(cmd)) {
        std::visit([this](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, CmdPlay>)      cmd_play();
            else if constexpr (std::is_same_v<T, CmdPause>)    cmd_pause();
            else if constexpr (std::is_same_v<T, CmdStop>)     cmd_stop();
            else if constexpr (std::is_same_v<T, CmdQuit>)     cmd_quit();
            else if constexpr (std::is_same_v<T, CmdSeek>)     cmd_seek(arg.position_ms);
            else if constexpr (std::is_same_v<T, CmdSetVolume>) cmd_set_volume(arg.value);
            else if constexpr (std::is_same_v<T, CmdSetMute>)   cmd_set_mute(arg.muted);
            else if constexpr (std::is_same_v<T, CmdSetVsr>)    cmd_set_vsr(arg.enabled);
            else if constexpr (std::is_same_v<T, CmdSetHwaccel>) cmd_set_hwaccel(arg.enabled);
        }, cmd);
        return;
    }

    // ── Heavy: RESIZE pre-filter debounce ──
    if (std::holds_alternative<CmdResize>(cmd)) {
        auto& r = std::get<CmdResize>(cmd);
        stored_resize_w_ = r.phys_w;
        stored_resize_h_ = r.phys_h;
        last_resize_cmd_ = std::chrono::steady_clock::now();
        last_phys_w_ = r.phys_w;
        last_phys_h_ = r.phys_h;
        return;  // leave to run_loop to merge after 800ms
    }

    // ── Heavy: merge into target state ──
    target_state_.merge(cmd);
    emit_optimistic();

    if (!busy_) {
        apply();
    }
}
```

- [ ] **Step 3: Build — will have undefined references**

```bash
cd build && make -j$(nproc)
```
Expected: link errors for `emit_optimistic()`, `apply()`, `execute()` — defined in next tasks.

- [ ] **Step 4: Commit**

```bash
git add src/core/PlayerCore.cpp
git commit -m "feat: rewrite dispatch with light/heavy split and RESIZE pre-filter"
```

---

## Task 7: PlayerCore.cpp — emit_optimistic

**Files:**
- Modify: `src/core/PlayerCore.cpp`

- [ ] **Step 1: Implement emit_optimistic()**

Add after `is_light()`:

```cpp
void PlayerCore::emit_optimistic() {
    PlayerEvent e;
    e.type = PlayerEvent::OPERATION_PENDING;
    e.pending_file = target_state_.file;
    e.pending_scale = target_state_.scale;
    e.pending_quality = target_state_.quality;
    e.pending_phys_w = target_state_.phys_w;
    e.pending_phys_h = target_state_.phys_h;
    emit_event(e);
}
```

- [ ] **Step 2: Commit**

```bash
git add src/core/PlayerCore.cpp
git commit -m "feat: implement emit_optimistic — push full target state to client"
```

---

## Task 8: PlayerCore.cpp — run_loop + apply + RESIZE Debounce

**Files:**
- Modify: `src/core/PlayerCore.cpp`

- [ ] **Step 1: Rewrite run_loop()**

Replace the existing `run_loop()` (lines 109-133):

```cpp
void PlayerCore::run_loop() {
    while (running_) {
        // Drain ALL commands into dispatch
        PlayerCommand cmd;
        while (cmd_queue_.try_pop(cmd)) {
            dispatch(cmd);
        }

        // RESIZE debounce: merge ripe stored dimensions
        auto now = std::chrono::steady_clock::now();
        if (stored_resize_w_ > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_resize_cmd_).count() >= 800) {
            target_state_.merge(CmdResize{stored_resize_w_, stored_resize_h_});
            emit_optimistic();
            stored_resize_w_ = stored_resize_h_ = 0;
        }

        // Execute if idle and work pending
        if (!busy_ && target_state_.active()) {
            apply();
        }

        if (state_ == PlaybackState::PLAYING && demuxer_) {
            if (!process_one_frame()) {
                state_ = PlaybackState::STOPPED;
                if (audio_) audio_->stop();
                emit_event({PlayerEvent::END_OF_FILE});
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    fprintf(stderr, "[run_loop] exiting, tearing down pipeline\n");
    teardown_pipeline();
    emit_event({PlayerEvent::STATE_CHANGED, PlaybackState::STOPPED});
    fprintf(stderr, "[run_loop] worker thread done\n");
}
```

- [ ] **Step 2: Implement apply()**

Add `apply()` after `emit_optimistic()`:

```cpp
void PlayerCore::apply() {
    busy_ = true;
    while (target_state_.active()) {
        TargetState snapshot = target_state_;
        target_state_.clear();  // clear BEFORE execute — allows new events in
        bool success = execute(snapshot);
        // If failed and no new events arrived → same target would fail → exit
        if (!success && !target_state_.active()) {
            break;
        }
    }
    busy_ = false;
}
```

- [ ] **Step 3: Build — execute() still undefined**

```bash
cd build && make -j$(nproc)
```
Expected: link error for `execute()` — defined in next task.

- [ ] **Step 4: Commit**

```bash
git add src/core/PlayerCore.cpp
git commit -m "feat: implement run_loop with RESIZE debounce, apply loop"
```

---

## Task 9: PlayerCore.cpp — execute() Two Paths

**Files:**
- Modify: `src/core/PlayerCore.cpp`

- [ ] **Step 1: Implement execute() — Path A (file changed) and Path B (reconfigure)**

Add `execute()` after `apply()`:

```cpp
bool PlayerCore::execute(const TargetState& snapshot) {
    // ── Determine if file changed ──
    bool file_changed = !snapshot.file.empty();

    if (file_changed) {
        // ═══ Path A: Full load ═══

        // Validate new file
        auto new_demuxer = std::make_unique<Demuxer>();
        if (!new_demuxer->open(snapshot.file)) {
            PlayerEvent err;
            err.type = PlayerEvent::ERROR;
            err.error_msg = "Failed to open: " + snapshot.file;
            emit_event(err);
            return false;
        }

        int new_vw = new_demuxer->video_width();
        int new_vh = new_demuxer->video_height();
        if (new_vw <= 0 || new_vh <= 0) {
            PlayerEvent err;
            err.type = PlayerEvent::ERROR;
            err.error_msg = "No video stream in: " + snapshot.file;
            emit_event(err);
            return false;
        }

        auto new_decoder = std::make_unique<Decoder>();
        if (!new_decoder->open(new_demuxer->video_codecpar(), no_hwaccel_)) {
            PlayerEvent err;
            err.type = PlayerEvent::ERROR;
            err.error_msg = "Decoder open failed: " + snapshot.file;
            emit_event(err);
            return false;
        }

        // CUDA context (first load only)
        if (!cuda_ctx_) {
            cuda_ctx_ = std::make_unique<CUDAContext>();
            if (!cuda_ctx_->capture_current()) {
                if (!cuda_ctx_->init(0)) {
                    PlayerEvent err;
                    err.type = PlayerEvent::ERROR;
                    err.error_msg = "CUDA init failed";
                    emit_event(err);
                    return false;
                }
            }
        }
        cuda_ctx_->push();

        if (!cuda_stream_) {
            cuStreamCreate((CUstream*)&cuda_stream_, CU_STREAM_NON_BLOCKING);
        }

        if (!nv12_to_rgb_) {
            nv12_to_rgb_ = std::make_unique<NV12ToRGB>();
            if (!nv12_to_rgb_->compile()) {
                cuda_ctx_->pop();
                PlayerEvent err;
                err.type = PlayerEvent::ERROR;
                err.error_msg = "NV12ToRGB compile failed";
                emit_event(err);
                return false;
            }
        }

        // Compute scale
        int scale = current_scale_;
        if (user_scale_ == 0) {
            int pw = snapshot.phys_w > 0 ? snapshot.phys_w : last_phys_w_;
            int ph = snapshot.phys_h > 0 ? snapshot.phys_h : last_phys_h_;
            if (pw > 0 && ph > 0)
                scale = compute_adaptive_scale(pw, ph, new_vw, new_vh);
        } else {
            scale = user_scale_;
        }
        Quality quality = snapshot.quality >= 0 ? (Quality)snapshot.quality : quality_;

        // GPU buffer
        if (rgb_gpu_) {
            cuMemFree((CUdeviceptr)rgb_gpu_);
            rgb_gpu_ = nullptr;
        }
        size_t rgb_bytes = NV12ToRGB::output_size(new_vw, new_vh);
        cuMemAlloc((CUdeviceptr*)&rgb_gpu_, rgb_bytes);

        // VSR init
        if (vsr_ && use_vsr_) {
            vsr_.reset();
        }
        if (use_vsr_ && !vsr_) {
            vsr_ = std::make_unique<VSRProcessor>();
            vsr_->set_stream(cuda_stream_);
            int vsr_ow = new_vw * scale;
            int vsr_oh = new_vh * scale;
            if (!vsr_->init(new_vw, new_vh, vsr_ow, vsr_oh, quality)) {
                fprintf(stderr, "PlayerCore: VSR init failed\n");
                vsr_.reset();
            }
        }

        // Sync CUDA before texture mutation
        cuStreamSynchronize((CUstream)cuda_stream_);

        // Textures
        renderer_->reconfigure_all_textures(new_vw, new_vh, scale, *vk_);

        // Audio
        auto new_audio = std::make_unique<AudioOutput>();
        if (!new_audio->open(snapshot.file.c_str())) {
            new_audio.reset();
        }

        // Commit
        audio_.reset();
        if (new_audio) audio_ = std::move(new_audio);
        audio_started_ = false;
        decoder_.reset();
        decoder_ = std::move(new_decoder);
        demuxer_.reset();
        demuxer_ = std::move(new_demuxer);

        video_w_ = new_vw; video_h_ = new_vh;
        video_fps_ = demuxer_->video_fps();
        duration_ms_ = demuxer_->duration_ms();
        seekable_ = duration_ms_ > 0;
        frame_count_ = 0;
        last_position_emit_ms_ = 0;
        current_scale_ = scale;
        quality_ = quality;
        vsr_w_ = new_vw * scale; vsr_h_ = new_vh * scale;

        if (snapshot.phys_w > 0) {
            last_phys_w_ = snapshot.phys_w;
            last_phys_h_ = snapshot.phys_h;
        }
        if (last_phys_w_ > 0) {
            renderer_->resize(last_phys_w_, last_phys_h_);
        }

        cuda_ctx_->pop();

        PlayerEvent info;
        info.type = PlayerEvent::VIDEO_INFO;
        info.in_width = video_w_;
        info.in_height = video_h_;
        info.out_width = vsr_w_;
        info.out_height = vsr_h_;
        info.fps = video_fps_;
        info.scale = current_scale_;
        info.quality = quality_;
        info.duration_ms = duration_ms_;
        info.hw_decoding = decoder_ ? decoder_->is_hardware() : false;
        info.vsr_active = (vsr_ && vsr_->is_ready());
        info.has_audio = (audio_ && audio_->is_active());
        info.seekable = seekable_;
        emit_event(info);
        return true;

    } else {
        // ═══ Path B: Reconfigure only (file unchanged) ═══

        if (!cuda_ctx_ || !renderer_) return false;
        cuda_ctx_->push();

        int scale = current_scale_;
        if (user_scale_ == 0) {
            int pw = snapshot.phys_w > 0 ? snapshot.phys_w : last_phys_w_;
            int ph = snapshot.phys_h > 0 ? snapshot.phys_h : last_phys_h_;
            if (pw > 0 && ph > 0 && video_w_ > 0 && video_h_ > 0)
                scale = compute_adaptive_scale(pw, ph, video_w_, video_h_);
        } else {
            scale = snapshot.scale >= 0 ? snapshot.scale : user_scale_;
        }
        Quality quality = snapshot.quality >= 0 ? (Quality)snapshot.quality : quality_;

        if (scale == current_scale_ && quality == quality_) {
            if (snapshot.phys_w > 0) {
                last_phys_w_ = snapshot.phys_w;
                last_phys_h_ = snapshot.phys_h;
                renderer_->resize(last_phys_w_, last_phys_h_);
            }
            cuda_ctx_->pop();
            return true;  // no-op
        }

        vsr_w_ = video_w_ * scale;
        vsr_h_ = video_h_ * scale;
        current_scale_ = scale;
        quality_ = quality;

        reconfigure_vsr();

        cuStreamSynchronize((CUstream)cuda_stream_);
        renderer_->reconfigure_scale(video_w_, video_h_, scale, *vk_);

        if (snapshot.phys_w > 0) {
            last_phys_w_ = snapshot.phys_w;
            last_phys_h_ = snapshot.phys_h;
        }
        renderer_->resize(last_phys_w_, last_phys_h_);

        cuda_ctx_->pop();

        PlayerEvent fi;
        fi.type = PlayerEvent::FRAME_INFO;
        fi.scale = current_scale_;
        fi.quality = quality_;
        emit_event(fi);
        return true;
    }
}
```

Note: the `cmd_load_file` and `cmd_resize` handler methods still exist (called from `execute` indirectly via the existing code). These become dead code after this change, but keep them for now — the Path A/B logic in `execute` replaces their direct invocation from `dispatch`.

- [ ] **Step 2: Build and verify**

```bash
cd build && make -j$(nproc)
```
Expected: success, all symbols resolved.

- [ ] **Step 3: Commit**

```bash
git add src/core/PlayerCore.cpp
git commit -m "feat: implement execute() — two-path diff from TargetState"
```

---

## Task 10: main.cpp — Remove Client Debounce, Wire OPERATION_PENDING

**Files:**
- Modify: `src/client/main.cpp`

- [ ] **Step 1: Remove QTimer resizeDebounce block**

Delete lines 138-157 (the `QTimer resizeDebounce` and its three connections):

```cpp
    // DELETE this block:
    // Resize debounce — everything on main thread
    // QTimer resizeDebounce;
    // ...
    // QObject::connect(&resizeDebounce, &QTimer::timeout, ...);
```

- [ ] **Step 2: Add raw CmdResize in beforeRenderPassRecording**

In the `beforeRenderPassRecording` lambda, after the `if (!ready) return;` block, add direct resize:

```cpp
        if (!ready) return;

        // Send raw resize every frame — core debounces
        int w = view.size().width() * view.devicePixelRatio();
        int h = view.size().height() * view.devicePixelRatio();
        if (w != lastSentW || h != lastSentH) {
            lastSentW = w; lastSentH = h;
            player->send_command(vsr::CmdResize{w, h});
        }

        auto* p = player.get();
```

Note: remove the old resize block that was after the initAttempted=false section — the above replaces both the old debounce timer and the old `if (ready)` resize logic.

- [ ] **Step 3: Add OPERATION_PENDING handler in event callback**

In the event callback switch statement, add:

```cpp
                    case vsr::PlayerEvent::OPERATION_PENDING:
                        ctrl.onPending(e);
                        break;
```

- [ ] **Step 4: Build and verify**

```bash
cd build && make -j$(nproc)
```
Expected: success. (QuickController::onPending not yet implemented — link error until Task 11.)

- [ ] **Step 5: Commit**

```bash
git add src/client/main.cpp
git commit -m "feat: remove client resize debounce, send raw resize, wire OPERATION_PENDING"
```

---

## Task 11: QuickController — Pending Properties + onPending

**Files:**
- Modify: `src/client/QuickController.h`
- Modify: `src/client/QuickController.cpp`

- [ ] **Step 1: Add Q_PROPERTYs and slot to header**

Add to `QuickController.h`:

```cpp
    Q_PROPERTY(QString pendingFile READ pendingFile NOTIFY pendingChanged)
    Q_PROPERTY(int pendingScale READ pendingScale NOTIFY pendingChanged)

public:
    QString pendingFile() const { return pending_file_; }
    int pendingScale() const { return pending_scale_; }

    // Called from event callback (via QueuedConnection)
    void onPending(const PlayerEvent& e);

signals:
    void pendingChanged();

private:
    QString pending_file_;
    int pending_scale_ = -1;
    int pending_quality_ = -1;
```

- [ ] **Step 2: Implement onPending() in .cpp**

Add to `QuickController.cpp`:

```cpp
void QuickController::onPending(const PlayerEvent& e) {
    bool changed = false;

    if (!e.pending_file.empty()) {
        QString f = QString::fromStdString(e.pending_file);
        if (pending_file_ != f) { pending_file_ = f; changed = true; }
    }
    if (e.pending_scale >= 0 && pending_scale_ != e.pending_scale) {
        pending_scale_ = e.pending_scale;
        changed = true;
    }
    if (e.pending_quality >= 0) {
        pending_quality_ = e.pending_quality;
    }

    if (changed) {
        fprintf(stderr, "[ui] optimistic: file=%s scale=%d\n",
                qPrintable(pending_file_), pending_scale_);
        emit pendingChanged();
    }
}
```

- [ ] **Step 3: Build and verify**

```bash
cd build && make -j$(nproc)
```
Expected: success, all symbols resolved.

- [ ] **Step 4: Commit**

```bash
git add src/client/QuickController.h src/client/QuickController.cpp
git commit -m "feat: add pending properties and onPending slot for optimistic UI"
```

---

## Task 12: overlay.qml — Bind to Pending Properties

**Files:**
- Modify: `src/client/overlay.qml`

- [ ] **Step 1: Update top bar to show pending file**

Change the top bar `Text` element's `text` binding (line ~78) to show pending state when available:

```qml
        Text {
            anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            color: controller && controller.pendingFile ? "#ffcc00" : "#e0e0e0"
            font.pixelSize: 14
            elide: Text.ElideRight
            text: {
                if (controller && controller.pendingFile)
                    return "⟳ " + controller.pendingFile.split('/').pop()
                if (controller && controller.videoInfo)
                    return controller.videoInfo
                return "VSR Player"
            }
        }
```

- [ ] **Step 2: Update quality badge to show pending scale**

Change the quality badge (line ~246) to reflect pending state:

```qml
            Rectangle {
                width: 52; height: 24; radius: 4
                color: controller && controller.pendingScale >= 0 ? "#44ffcc00" : "#44ffffff"
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    anchors.centerIn: parent
                    text: {
                        if (controller && controller.pendingScale >= 0)
                            return controller.pendingScale + "x"
                        return "HIGH"
                    }
                    color: controller && controller.pendingScale >= 0 ? "#ffcc00" : "#e0e0e0"
                    font.pixelSize: 11; font.bold: true
                }
            }
```

- [ ] **Step 3: Commit**

```bash
git add src/client/overlay.qml
git commit -m "feat: bind QML to pendingFile/pendingScale for optimistic feedback"
```

---

## Task 13: Integration Test — Verify Coalescing

- [ ] **Step 1: Build release**

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
```
Expected: success.

- [ ] **Step 2: Rapid video switch test**

```bash
./build/vsr-player /path/to/video1.mp4
# Quickly switch between files using playlist or keyboard shortcuts
```
Expected:
- No crash, no freeze
- UI updates immediately (optimistic, yellow text) when target changes
- Video eventually renders the correct last-selected file
- `[ui] optimistic:` debug lines appear in stderr

- [ ] **Step 3: Resize while busy test**

```bash
./build/vsr-player /path/to/video.mp4
# Immediately start resizing window during initial load
```
Expected:
- QML UI never disappears or turns black
- RESIZE coalesces, applying only after the initial load completes
- No `vkDeviceWaitIdle` being called from worker thread (verify: add breakpoint or log)

- [ ] **Step 4: Continuous resize during playback test**

```bash
./build/vsr-player /path/to/video.mp4
# Wait for playback to stabilize, then drag resize continuously
```
Expected:
- Only one VSR reconfigure per resize drag (800ms debounce)
- Scale changes smoothly when debounce fires
- No intermediate reconfigure at every intermediate size

- [ ] **Step 5: Commit any final fixes**

---

## Verification Checklist

1. **Heavy merge:** Rapid LOAD_FILE(A→B→C) → only C loads
2. **RESIZE debounce:** Drag resize → one reconfigure after 800ms of inactivity
3. **Optimistic UI:** `OPERATION_PENDING` updates QML instantly, `VIDEO_INFO` overwrites
4. **Light passthrough:** PLAY/PAUSE/SEEK work immediately during busy period
5. **UI stability:** QML never turns black, `vkDeviceWaitIdle` never blocks Qt rendering
6. **Error recovery:** Invalid file → ERROR event, core continues accepting commands
7. **Retry:** Failed load + new target arrives → retries with new target
