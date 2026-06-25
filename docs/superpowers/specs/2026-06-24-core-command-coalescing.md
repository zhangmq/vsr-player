# Core Command Coalescing — Design Spec (v2)

**Goal:** Prevent crashes from rapid heavy-operation requests (video switch, resize,
quality/scale change) by coalescing commands into a **target state** driven by user
intent, protected by a `busy_` flag. All heavy operations ultimately converge to VSR
reinit + InteropTexture recreation — the single expensive path that needs protection.

**Architecture:** State-driven, not command-driven. Events merge into a `TargetState`
snapshot representing "what the user ultimately wants." When the core is not busy,
`apply()` diffs current state against target state and executes a single batch of
mutations. New events arriving during execution update target state; `apply()` loops
until target stabilizes. RESIZE has an 800ms pre-filter debounce before entering
target state. `OPERATION_PENDING` pushes the full target state to the client for
optimistic UI.

---

## 1. State Model

### TargetState — user intent snapshot

```cpp
struct TargetState {
    std::string file;      // target file (empty = no pending)
    int phys_w = 0;        // target window width
    int phys_h = 0;        // target window height
    int scale = -1;        // target scale (-1 = unset, 0=auto, 1-4=locked)
    int quality = -1;      // target quality (-1 = unset)

    bool active() const {
        return !file.empty() || phys_w > 0 || scale >= 0 || quality >= 0;
    }

    void clear() { file.clear(); phys_w = phys_h = 0; scale = quality = -1; }
};
```

### Current state

Not a separate struct — the existing PlayerCore fields after a successful operation:
`demuxer_` (current file), `current_scale_`, `quality_`, `last_phys_w_/h_`.

### Merge semantics

All events update TargetState. Last write wins — no command queuing.

| Event | Fields updated | Notes |
|---|---|---|
| `LOAD_FILE` | file = path; phys_w/h = 0; scale = quality = -1 | New file recomputes scale from window size |
| `RESIZE` (after debounce) | phys_w, phys_h | |
| `SET_SCALE` | scale | |
| `SET_QUALITY` | quality | |

merge is a pure data operation — no side effects, no execution.

---

## 2. Control Flow

### Heavy vs Light

**Heavy** (enter target-state path):
`LOAD_FILE`, `RESIZE`, `SET_SCALE`, `SET_QUALITY`

**Light** (execute immediately, always):
`PLAY`, `PAUSE`, `STOP`, `QUIT`, `SEEK`, `SET_VOLUME`, `SET_MUTE`, `SET_VSR`, `SET_HWACCEL`

### STOP semantics (VLC-style)

`STOP` is a light command but has broad side effects:
- Sets `state_ = STOPPED`
- Stops audio: joins decode thread, closes PaStream, sets `audio_started_ = false`
- Resets all PTS/sync state: `clock_bias_ = 0`, `last_pts_sec_ = -1`, `current_pts_sec_ = 0`, `pts_fallback_counter_ = 0`
- `cmd_seek(0)` — flushes decoder, seeks demuxer to position 0, seeks audio
- **Does NOT** unload demuxer/decoder/audio — file stays loaded
- **Does NOT** emit END_OF_FILE
- Play after stop resumes from beginning with clean sync state

This prevents a critical bug: without resetting `clock_bias_`, speed-change bias from
before stop persists, causing `delay = video_time - (clock + stale_bias)` to be wildly
incorrect when the new audio clock starts from 0.

### dispatch — merge only, never execute

dispatch is called from run_loop's drain loop. It must NOT call apply() itself —
apply() runs after the entire command queue has been drained, so all rapid-fire
events have been merged into target_state_ before execution starts.

```
dispatch(cmd):
  if is_light(cmd):
    execute immediately
    return

  // Heavy
  if cmd is RESIZE:
    stored_resize_w_ = w; stored_resize_h_ = h
    last_resize_cmd_ = now
    last_phys_w_ = w; last_phys_h_ = h   // update immediately for viewport
    return  // pre-filter: debounce timer handles merge

  target_state_.merge(cmd)
  emit_optimistic(target_state_)          // push full target state
  // do NOT call apply() here — run_loop handles it after drain
```

### RESIZE debounce — run_loop timer check

Client sends raw `CmdResize` every frame during drag. Core stores dimensions
and waits 800ms of inactivity before merging into target state.

```
run_loop:
  while running_:
    // Drain queue — dispatch() does merge only, no apply()
    while cmd_queue_.try_pop(cmd):
      dispatch(cmd)

    // RESIZE debounce: merge ripe dimensions
    if stored_resize_w_ > 0 && (now - last_resize_cmd_) >= 800ms:
      target_state_.merge(CmdResize{stored_resize_w_, stored_resize_h_})
      emit_optimistic(target_state_)
      stored_resize_w_ = stored_resize_h_ = 0

    // Execute if idle and work pending
    // All events in the queue have been merged by this point
    if !busy_ && target_state_.active():
      apply()

    // Frame processing...
```

### apply — execute and check for new events after

```
apply():
  busy_ = true
  // After execute(), drain the queue again — new commands may have arrived
  // during execution. dispatch() merges them into target_state_.
  // Loop until no more pending work.
  do {
    snapshot = target_state_
    target_state_.clear()
    success = execute(snapshot)
    // Drain commands that arrived during execute
    while cmd_queue_.try_pop(cmd):
      dispatch(cmd)
    // If target has new content (merged during drain above), loop again
  } while (!success || target_state_.active())
  busy_ = false
```

Key design points:
- target_state_ cleared BEFORE execute — arriving events fill a fresh target
- After execute, drain cmd_queue_ via dispatch (merge only) — catches all
  commands that arrived during the 1-3s execution window
- Loop if target_state_ has new content after drain — handles rapid switches
- busy_ remains true across the entire loop — no gap for re-entry

---

## 3. execute(snapshot) — State Diff to Mutations

Two execution paths based on whether the file changed:

### Path A: file changed (full load)

```
execute(snapshot):
  // Validate new file
  new_demuxer = Demuxer::open(snapshot.file)
  if fail → emit ERROR, return false  // target consumed, no retry

  new_decoder = Decoder::open(codecpar)
  if fail → emit ERROR, return false

  // Compute VSR parameters
  scale = snapshot.scale >= 0 ? snapshot.scale
        : snapshot.phys_w > 0 ? compute_adaptive(snapshot.phys_w, snapshot.phys_h, vw, vh)
        : compute_adaptive(last_phys_w_, last_phys_h_, vw, vh)
  quality = snapshot.quality >= 0 ? snapshot.quality : current_quality_

  vsr_w = video_w * scale; vsr_h = video_h * scale

  // GPU resources
  cuMemFree(old_rgb_gpu_); cuMemAlloc(new_rgb_gpu_)

  // VSR init (if use_vsr_)
  VSR::init(video_w, video_h, vsr_w, vsr_h, quality)
  if fail → emit ERROR, return false

  // Sync CUDA before texture mutation (see Section 7)
  cuStreamSynchronize(cuda_stream_)

  // Textures (Render Gate protected)
  reconfigure_all_textures(video_w, video_h, scale)
  // includes: begin_mutation → rgba destroy/create + y destroy/create + uv destroy/create → end_mutation

  // Audio
  AudioOutput::open(snapshot.file)

  // Commit
  swap demuxer_, decoder_, audio_
  current_scale_ = scale; quality_ = quality
  update video_w_, video_h_, vsr_w_, vsr_h_
  emit VIDEO_INFO
  return true
```

### Path B: file unchanged (reconfigure only)

```
execute(snapshot):
  scale = snapshot.scale >= 0 ? snapshot.scale
        : compute_adaptive(snapshot.phys_w, snapshot.phys_h, video_w_, video_h_)
  quality = snapshot.quality >= 0 ? snapshot.quality : current_quality_

  if scale == current_scale_ && quality == current_quality_:
    return true  // no-op

  vsr_w = video_w_ * scale; vsr_h = video_h_ * scale

  // VSR reinit
  VSR::reconfigure(vsr_w, vsr_h, quality)  // or release()+init()
  if fail → emit ERROR, return false

  // Sync CUDA before texture mutation (see Section 7)
  cuStreamSynchronize(cuda_stream_)

  // Textures (Render Gate protected)
  reconfigure_scale(video_w_, video_h_, scale)
  // includes: begin_mutation → rgba destroy/create → end_mutation

  // Commit
  current_scale_ = scale; quality_ = quality
  emit FRAME_INFO
  return true
```

Both paths converge to VSR reinit + InteropTexture recreation — the expensive
operation that `busy_` protects.

---

## 4. Error Handling & Retry

After `execute()` returns false with `target_state_` already cleared:

| Failure | target_state_ has new events? | Behavior |
|---|---|---|
| File open | Yes (different file) | Loop continues — retry with new target |
| File open | No | Break — same file would fail again |
| Decoder open | Yes (hwaccel setting changed) | Loop continues |
| Decoder open | No (same file, same hwaccel) | Break |
| VSR init | Yes (scale/quality changed) | Loop continues |
| VSR init | No (same parameters) | Break |

Failed targets are consumed — the error is emitted, and the core waits for a
new external event to change the target state before retrying.

No failure counter needed — the loop's natural behavior handles it. If
target_state_ is empty after a failure, the same target would fail identically,
so we exit.

---

## 5. OPERATION_PENDING — Optimistic State Push

When the target state changes during `busy_=true`, the full target state is pushed
to the client — not just the triggering event.

```cpp
// Player.h — new event fields
struct PlayerEvent {
    enum Type {
        ...,
        OPERATION_PENDING,
    };

    // OPERATION_PENDING fields (full target state)
    std::string pending_file;    // target_state_.file
    int pending_scale = -1;      // target_state_.scale (-1 = unset)
    int pending_quality = -1;    // target_state_.quality (-1 = unset)
    int pending_phys_w = 0;      // target_state_.phys_w (0 = unset)
    int pending_phys_h = 0;      // target_state_.phys_h (0 = unset)
};
```

Client behavior:
- `OPERATION_PENDING` → update UI optimistically (file name, scale, quality) from full target state
- Subsequent `VIDEO_INFO` / `FRAME_INFO` → overwrite optimistic state with real state
- Client never blocks user interaction while waiting for core

### QuickController — target state mirror

QuickController maintains a local copy of the latest target state for UI display
and debugging:

```cpp
// QuickController.h
class QuickController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString pendingFile READ pendingFile NOTIFY pendingChanged)
    Q_PROPERTY(int pendingScale READ pendingScale NOTIFY pendingChanged)

public:
    QString pendingFile() const { return pending_file_; }
    int pendingScale() const { return pending_scale_; }

    // Called from main.cpp OPERATION_PENDING handler
    void onPending(const PlayerEvent& e);

signals:
    void pendingChanged();

private:
    QString pending_file_;
    int pending_scale_ = -1;
    int pending_quality_ = -1;
};
```

```cpp
// QuickController.cpp
void QuickController::onPending(const PlayerEvent& e) {
    // Update mirrored target state
    if (!e.pending_file.empty()) pending_file_ = QString::fromStdString(e.pending_file);
    if (e.pending_scale >= 0)    pending_scale_ = e.pending_scale;
    if (e.pending_quality >= 0)  // store quality
    // ...
    emit pendingChanged();
    fprintf(stderr, "[ui] optimistic: file=%s scale=%d\n",
            qPrintable(pending_file_), pending_scale_);
}
```

The QML overlay binds to `pendingFile` / `pendingScale` for instant visual
feedback. When `VIDEO_INFO` arrives, `updateVideoInfo()` overwrites these
same display fields with real data.

---

## 6. Client Changes

**main.cpp:**
- Delete `QTimer resizeDebounce`, `widthChanged`/`heightChanged` connections
- In `beforeRenderPassRecording`, send `CmdResize{w, h}` directly (no debounce)
- Wire `OPERATION_PENDING` event → `ctrl.onPending(e)`

**QuickController:**
- Add target state mirror properties (`pendingFile`, `pendingScale`, etc.)
- `onPending(PlayerEvent)` stores latest target state, exposes to QML
- Existing `updateVideoInfo()` / `updateState()` unchanged — real events overwrite

---

## 7. GPU Synchronization

`begin_mutation()` calls `vk.waitIdle()` (`vkDeviceWaitIdle`) to ensure the GPU
has finished all submitted command buffers before textures are destroyed. This
adds ~8ms to the mutation window and does NOT block Qt's main thread event loop.
Qt's scene graph continues to submit new frames after the drain completes.

The drain is synchronous and correct. No changes needed.

`InteropTexture::release()` previously had its own `vkDeviceWaitIdle` — removed
as redundant (begin_mutation already drains before any texture destruction).

`cuStreamSynchronize(cuda_stream_)` is called before texture reconfigure in
`execute()` for explicit CUDA-Vulkan synchronization.

---

## 8. File Change Matrix

| File | Change |
|---|---|
| `src/core/api/Player.h` | Add `OPERATION_PENDING` type, `pending_*` fields |
| `src/core/PlayerCore.h` | Add `TargetState`, `busy_`, `target_state_`, RESIZE debounce members (`stored_resize_w_/h_`, `last_resize_cmd_`), `apply()`, `is_light()` |
| `src/core/PlayerCore.cpp` | Implement TargetState, rewrite `dispatch` (merge only) + `run_loop` (drain then apply), add `apply()` with post-execute drain loop + `execute()` + `cuStreamSynchronize`, RESIZE debounce check |
| `src/core/utils/InteropTexture.cpp` | `release()`: remove redundant `vkDeviceWaitIdle(device)` |
| `src/client/main.cpp` | Remove QTimer resize debounce, send raw `CmdResize` in `beforeRenderPassRecording`, wire `OPERATION_PENDING` → `onPending()` |
| `src/client/QuickController.h` | Add `pending_file_`, `pending_scale_`, `pending_quality_`, Q_PROPERTYs, `onPending()` slot, `pendingChanged()` signal |
| `src/client/QuickController.cpp` | Implement `onPending()` — mirror target state, log for debugging, emit `pendingChanged()` |
| `src/client/overlay.qml` | Bind status display to `controller.pendingFile` / `controller.pendingScale` for optimistic feedback |

---

## 9. Deferred Items

None.
