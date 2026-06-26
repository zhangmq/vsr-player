# Project Cleanup & Documentation — Implementation Plan

> Inline execution recommended — file deletions + markdown docs, no code logic changes.

**Goal:** Remove all obsolete content, add bilingual README, reorganize docs to reflect current codebase.

**Architecture:** Delete stale files → update config → write new docs (README×2, ARCHITECTURE, BUILD) → rewrite CLAUDE.md → verify build.

**Tech Stack:** Markdown, git

---

### Task 1: Delete obsolete files

- [ ] **Step 1: Remove archive/, docs/superpowers/, stale tests, screenshots, cache**

```bash
cd /home/zmq/projects/vsr-player
rm -rf archive/ docs/superpowers/ tests/test_qtquick_overlay/ screenshots/ .ruff_cache/
```

- [ ] **Step 2: Verify git status shows all deletions**

```bash
git status
```

- [ ] **Step 3: Commit**

```bash
git add -A && git commit -m "chore: remove obsolete files — archive, historical specs/plans, stale tests, screenshots, cache"
```

---

### Task 2: Update .gitignore

- [ ] **Step 1: Add .ruff_cache/ entry**

Read current `.gitignore`, append `.ruff_cache/` if not already present.

- [ ] **Step 2: Commit**

```bash
git add .gitignore && git commit -m "chore: add .ruff_cache/ to gitignore"
```

---

### Task 3: Write README.md (English)

- [ ] **Step 1: Write README.md**

Sections:
1. Project title + tagline — "VSR Player — Real-time AI super-resolution video player for Linux"
2. Features bullet list (NVDEC HW decode, VFX SDK AI upscale/denoise, zero-PCIe GPU pipeline, Vulkan rendering, QML overlay UI, A/V sync)
3. Screenshot placeholder
4. Prerequisites (NVIDIA driver 570+, Vulkan, Qt 6, FFmpeg, PortAudio)
5. Quick Start — clone, setup third_party/, make, run
6. CLI options table
7. Architecture diagram (ASCII)
8. Directory structure tree
9. License (MIT)

- [ ] **Step 2: Commit**

```bash
git add README.md && git commit -m "docs: add English README"
```

---

### Task 4: Write README_zh.md (Chinese)

- [ ] **Step 1: Write README_zh.md**

Same content as README.md, translated to Chinese.

- [ ] **Step 2: Commit**

```bash
git add README_zh.md && git commit -m "docs: add Chinese README"
```

---

### Task 5: Write docs/ARCHITECTURE.md

- [ ] **Step 1: Create docs/ directory, write ARCHITECTURE.md**

Sections:
1. Architecture diagram (full ASCII art from CLAUDE.md)
2. Data flow — container → demux → NVDEC → NV12→RGB → VSR → Vulkan → screen
3. Audio path — FFmpeg → PortAudio → master clock → A/V sync
4. Module descriptions (Demuxer, Decoder, VSRProcessor, Renderer, AudioOutput, ClockManager, NV12ToRGB, FramePool)
5. Key design decisions (codec rules, Vulkan Wayland judgment chain, VSR dependency chain, SDK isolation)
6. File index (src/client/ + src/core/ paths with brief descriptions)

- [ ] **Step 2: Commit**

```bash
git add docs/ARCHITECTURE.md && git commit -m "docs: add ARCHITECTURE.md"
```

---

### Task 6: Write docs/BUILD.md

- [ ] **Step 1: Write BUILD.md**

Sections:
1. System requirements (NVIDIA driver 570+, Vulkan SDK, Qt 6.8+, FFmpeg 6+, PortAudio, glslc)
2. Arch Linux install commands
3. Ubuntu install commands
4. third_party/ setup (CUDA headers/libs from /opt/cuda, NvVFX headers from GitHub, NvVFX .so from NGC)
5. Build: `make -j$(nproc)`
6. Run tests
7. Run player
8. Troubleshooting (common issues: modeset, missing .so, Wayland fallback)

- [ ] **Step 2: Commit**

```bash
git add docs/BUILD.md && git commit -m "docs: add BUILD.md"
```

---

### Task 7: Rewrite CLAUDE.md

- [ ] **Step 1: Rewrite CLAUDE.md**

Remove all obsolete sections (Python prototype, archive/, historical docs refs, old plans).
Keep and update:
1. Project overview paragraph
2. Build/run commands
3. Full architecture diagram
4. Module file index (current paths)
5. Codec selection rules (native + hwaccel, never _cuvid)
6. Vulkan + Wayland judgment chain (verified)
7. VSR dependency chain + SDK isolation
8. Key findings from prototype (still relevant ones only)

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md && git commit -m "docs: rewrite CLAUDE.md — remove obsolete content, update to current codebase"
```

---

### Task 8: Final verification

- [ ] **Step 1: Build**

```bash
make -j$(nproc)
```

Expected: `OK` or `All dependencies present.`

- [ ] **Step 2: Quick run test**

```bash
timeout 5 ./build/vsr-player input 2>&1 | grep -E "qt\.|file://|Error" || echo "clean"
```

Expected: `clean`

- [ ] **Step 3: Final commit if any stragglers**

```bash
git add -A && git diff --cached --stat && git commit -m "chore: final cleanup verification" || echo "nothing to commit"
```
