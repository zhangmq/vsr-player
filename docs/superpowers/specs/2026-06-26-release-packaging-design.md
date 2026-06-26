# Release Packaging & Self-Contained Install — Design Spec

**Date:** 2026-06-26
**Status:** Approved

## Goal

Make VSR Player distributable: one tarball, one install script, zero manual dependency hunting. Users run `./install.sh` and get a working player.

## Constraints

- **NvVFX .so files are NVIDIA proprietary** — cannot redistribute. User must install `pip install nvidia-vfx`; script copies .so from pip package.
- **NvVFX headers are MIT** — can bundle in release tarball.
- **CUDA headers/libs** — use system `/opt/cuda`, no need to bundle (CUDA Toolkit has Linux redistribution exception; simpler to just read from system).
- **x86_64 only** — VFX SDK has no ARM build.
- **NVIDIA driver required** — `libcuda.so.1` must be present on system.

## Install Layout (user-level, no root)

```
~/vsr-player/
├── bin/vsr-player                                  ← binary
├── lib/                                            ← RPATH $ORIGIN/../lib
│   ├── libnvrtc.so.13                              ← from system CUDA
│   ├── libnvrtc-builtins.so.13.0
│   ├── libnvVFXVideoSuperRes.so                    ← from pip nvidia-vfx
│   ├── libVideoFX.so
│   ├── libVideoFXLocal.so
│   ├── libNVCVImage.so
│   ├── libnvngxruntime.so
│   ├── libnvidia-ngx-vsr.so.1.8.2 → libnvidia-ngx-vsr.so
│   ├── libnvinfer.so.10 → libnvinfer.so
│   ├── libnvinfer_plugin.so.10 → libnvinfer_plugin.so
│   ├── libnvonnxparser.so.10 → libnvonnxparser.so
│   ├── libcudnn.so.9 → libcudnn.so
│   ├── libnpp*.so.12 ×9 → libnpp*.so
│   └── fonts/
│       └── MaterialIcons-Regular.ttf               ← from Google Fonts
├── share/vsr-player/
│   ├── qml/                                        ← overlay.qml + components/
│   │   ├── overlay.qml
│   │   ├── TopBar.qml / BottomBar.qml / ...
│   │   └── components/IconButton.qml
│   └── shaders/                                    ← SPIR-V (.spv)
│       ├── video.vert.spv
│       ├── video.frag.spv
│       └── nv12.frag.spv
├── screenshots/                                    ← runtime output
├── config/                                         ← future config files
└── install.sh                                      ← (placed alongside or separate)
```

## Code Changes

### 1. RPATH: `$ORIGIN/../lib`

Makefile already uses `-Wl,--disable-new-dtags`. Change linker flags:

```
-Wl,-rpath,'$$ORIGIN'/../lib
```

Replace hardcoded `/home/zmq/projects/vsr-player/third_party/...` paths.

### 2. Runtime resource resolution (main.cpp)

```cpp
QString appDir = QCoreApplication::applicationDirPath();

// Dev mode: detect source tree structure
QString shareDir;
if (QFile::exists(appDir + "/../share/vsr-player"))
    shareDir = appDir + "/../share/vsr-player";   // installed
else
    shareDir = appDir + "/../src/client/ui";       // dev: sources

// QML
view.setSource(QUrl::fromLocalFile(shareDir + "/qml/overlay.qml"));  // installed
// or src/client/ui/overlay.qml                                       // dev

// Font — pass to QML as context property
view.rootContext()->setContextProperty("resourceDir", shareDir);

// Screenshots
QString screenshotDir = appDir + "/../screenshots";  // installed
// mkdir(screenshotDir.toStdString().c_str(), 0755);
```

### 3. QML font path (overlay.qml)

Replace hardcoded path:

```qml
// Before
source: "file:///usr/share/fonts/TTF/MaterialIcons-Regular.ttf"

// After  
source: "file://" + resourceDir + "/../lib/fonts/MaterialIcons-Regular.ttf"
```

Or pass as context property:
```cpp
view.rootContext()->setContextProperty("fontPath",
    "file://" + appDir + "/../lib/fonts/MaterialIcons-Regular.ttf");
```

QML: `source: fontPath`

### 4. Third-party dependency handling

**Remove** `third_party/cuda/` directory — no longer needed.

**Keep** `third_party/nvvfx/include/` — MIT headers, bundled in release tarball.

**Update** `scripts/check-deps.sh` — verify only nvvfx headers (not libs, not cuda).

**Update** Makefile — CUDA paths point to system (`/opt/cuda`), not `third_party/cuda`. NvVFX lib path updated.

### 5. Build output restructuring

Current: all objects in `build/src/...`

New: produce installable layout directly in `build/install/`:

```
make install   →   build/install/
                   ├── bin/vsr-player
                   ├── lib/ (symlinks to third_party/nvvfx/lib during dev)
                   ├── share/vsr-player/qml/
                   ├── share/vsr-player/shaders/
                   ├── screenshots/
                   └── config/
```

## install.sh

```bash
#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${INSTALL_DIR:-$HOME/vsr-player}"
echo "VSR Player — Installer"
echo "Install to: $INSTALL_DIR"

# 1. Check system deps (Qt6, FFmpeg, PortAudio, Vulkan, libcuda.so.1)
#    → pkg-config checks, missing → install hints

# 2. pip install nvidia-vfx
#    → find .so path, copy to $INSTALL_DIR/lib/

# 3. Copy CUDA runtime libs (libnvrtc.so.13, libnvrtc-builtins.so.13.0)
#    from /opt/cuda/lib64/

# 4. Download MaterialIcons-Regular.ttf → $INSTALL_DIR/lib/fonts/
#    URL: https://github.com/google/material-design-icons/.../MaterialIcons-Regular.ttf

# 5. Extract tarball: bin/ + share/ → $INSTALL_DIR/

# 6. Create symlinks (libnvinfer.so → libnvinfer.so.10, etc.)

# 7. Verify: run check-deps equivalent

# 8. Print success + PATH hint
echo "Done. Add to PATH: export PATH=\$PATH:$INSTALL_DIR/bin"
echo "Run: vsr-player <video_file>"
```

## Verification

1. Clean install test: fresh VM/container, run `install.sh`, player launches
2. Dev mode unchanged: `make -j$(nproc) && ./build/vsr-player input` still works
3. Screenshots go to `~/vsr-player/screenshots/`
4. Config dir created on first run (future)
