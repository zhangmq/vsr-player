# Project Cleanup & Documentation — Design Spec

**Date:** 2026-06-26
**Branch:** `feature/cleanup-docs-readme`

## Goal

Remove all obsolete content, reorganize documentation to reflect current codebase state, and add bilingual README.

## Phase 1: Deletion

| Target | Reason |
|---|---|
| `archive/python-v1/` | Python prototype, fully superseded by C++ |
| `archive/prototype-v1.py` | First prototype script |
| `archive/overlay-ui-v2/` | Old C++ widget UI, replaced by QML components |
| `docs/superpowers/` (entire) | 24 historical AI-assisted spec/plan files, no future development value |
| `tests/test_qtquick_overlay/` | Early Qt+Vulkan spike, pattern now integrated in main.cpp |
| `screenshots/` | Debug screenshots, already gitignored |
| `.ruff_cache/` | Python lint cache from deleted python-v1 |

## Phase 2: New Files

### README.md (English)

Sections:
- Project name + one-line description
- Features (AI super-resolution, NVDEC hardware decode, zero-PCIe GPU pipeline, Vulkan rendering, QML overlay UI)
- Screenshot placeholder
- Quick Start (dependencies → build → run)
- CLI options table
- Architecture diagram (ASCII art from CLAUDE.md)
- Directory structure
- License

### README_zh.md (Chinese)

Same content as README.md, in Chinese.

### docs/ARCHITECTURE.md

Extracted from CLAUDE.md, intended for human readers:
- Full architecture ASCII diagram
- Data flow description
- Module responsibilities (Demuxer, Decoder, VSRProcessor, Renderer, AudioOutput, ClockManager, NV12ToRGB)
- Key design decisions:
  - Codec selection rules (native + hwaccel, never _cuvid)
  - Vulkan + Wayland judgment chain
  - VSR dependency chain
  - SDK isolation strategy

### docs/BUILD.md

- Prerequisites (NVIDIA driver 570+, Vulkan SDK, Qt 6, FFmpeg, PortAudio)
- Distro-specific install commands (Arch, Ubuntu)
- third_party/ preparation (CUDA headers, NvVFX headers/libs)
- Build: `make -j$(nproc)`
- Run tests
- Run player

## Phase 3: Updated Files

### CLAUDE.md

Keep as AI assistant context. Remove:
- All historical/obsolete findings
- Python prototype references
- `archive/` references
- Plans/specs index (deleted)

Keep and update:
- Project overview (one paragraph)
- Build/run commands
- Full architecture diagram
- Module file index (src/client/ + src/core/ paths)
- Key findings still relevant: codec selection rules, Vulkan Wayland judgment chain, VSR dependency chain, SDK isolation
- Environment notes (Qt 6.11, C++20, Makefile-based build)

### .gitignore

Add `.ruff_cache/`. Confirm `screenshots/` already covered.

## Verification

1. `make -j$(nproc)` — build succeeds
2. `./build/vsr-player input` — player launches correctly
3. All deleted files are git-tracked and recoverable via history
