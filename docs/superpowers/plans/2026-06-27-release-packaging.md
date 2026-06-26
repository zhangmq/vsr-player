# Release Packaging — Implementation Plan

> Inline execution recommended — interconnected Makefile, main.cpp, QML changes.

**Goal:** Self-contained user-level install via tarball + `install.sh`. No root, no manual third_party setup.

**Architecture:** Binary RPATH → `$ORIGIN/../lib`, runtime paths from `applicationDirPath()`, NVRTC bundled in release, VFX .so from `pip install nvidia-vfx`, font bundled.

**Tech Stack:** Makefile, C++20, Qt 6.11, QML, Bash

---

## File Impact

| File | Change |
|------|--------|
| `Makefile` | RPATH → `$ORIGIN/../lib`, CUDA path → `/opt/cuda`, add `make install` |
| `src/client/main.cpp` | Dynamic resource paths via `applicationDirPath()`, font context property |
| `src/client/ui/overlay.qml:13` | Font path → context property |
| `scripts/check-deps.sh` | Rewrite: nvvfx headers only, no CUDA check |
| `scripts/install.sh` | **New** — user-level installer |
| `third_party/cuda/` | Delete |
| `third_party/fonts/` | Create, bundle MaterialIcons-Regular.ttf |
| `.gitignore` | Add `third_party/fonts/` |
| `docs/BUILD.md` | Update build/install instructions |

---

### Task 1: RPATH + CUDA path in Makefile

- [ ] **Step 1: Change RPATH to `$ORIGIN/../lib`**

```makefile
# Before (lines 31, 33)
-Wl,-rpath,/home/zmq/projects/vsr-player/third_party/cuda/lib \
-Lthird_party/nvvfx/lib -lNVCVImage \
-Wl,-rpath,/home/zmq/projects/vsr-player/third_party/nvvfx/lib

# After
-Wl,-rpath,'$$ORIGIN'/../lib \
-Lthird_party/nvvfx/lib -lNVCVImage
```

Note: `$$ORIGIN` — double `$` escapes make's variable expansion, passes `$ORIGIN` to linker.

- [ ] **Step 2: Change CUDA include/lib to system paths**

```makefile
# Before
CUDA_INC := $(CUDA_DIR)/include
CUDA_LIB := $(CUDA_DIR)/lib

# After
CUDA_INC := /opt/cuda/include
CUDA_LIB := /opt/cuda/lib64
```

- [ ] **Step 3: Remove `third_party/cuda` references from CXXFLAGS, LDFLAGS, test target**

`-I$(CUDA_INC)` and `-L$(CUDA_LIB) -lnvrtc -lnvrtc-builtins` unchanged — just the variable definitions change.

- [ ] **Step 4: Commit**

```bash
git add Makefile && git commit -m "build: RPATH $ORIGIN/../lib, CUDA from /opt/cuda"
```

---

### Task 2: Dynamic resource paths in main.cpp

- [ ] **Step 1: Add resource path detection at top of main()**

After `QGuiApplication app(argc, argv);`, add:

```cpp
// Resource resolution: installed vs dev tree
QString appDir = QCoreApplication::applicationDirPath();
QString installShare = appDir + "/../share/vsr-player";
QString devUI = appDir + "/../src/client/ui";
QString devShaders = appDir + "/..";

bool installed = QFile::exists(installShare + "/qml");

QString qmlDir   = installed ? installShare + "/qml"     : devUI;
QString fontFile = installed ? appDir + "/../lib/fonts/MaterialIcons-Regular.ttf"
                             : "/usr/share/fonts/TTF/MaterialIcons-Regular.ttf";
QString screenshotDir = installed ? appDir + "/../screenshots" : "screenshots";
```

- [ ] **Step 2: Pass font path as context property**

```cpp
view.rootContext()->setContextProperty("fontPath",
    QUrl::fromLocalFile(fontFile).toString());
```

- [ ] **Step 3: Update QML source path**

```cpp
view.setSource(QUrl::fromLocalFile(qmlDir + "/overlay.qml"));
```

- [ ] **Step 4: Update screenshot path**

Replace `mkdir("screenshots", 0755)` and `snprintf(path, ..., "screenshots/...")` with dynamic path from `screenshotDir`.

- [ ] **Step 5: Build and test (dev mode)**

```bash
make -j$(nproc) && ./build/vsr-player input
```

Expected: dev mode still works (font path falls back to system TTF).

- [ ] **Step 6: Commit**

```bash
git add src/client/main.cpp && git commit -m "feat: dynamic resource paths — appDir detection for installed vs dev"
```

---

### Task 3: Font path in overlay.qml

- [ ] **Step 1: Replace hardcoded font path**

```qml
// Before (line 13)
source: "file:///usr/share/fonts/TTF/MaterialIcons-Regular.ttf"

// After
source: fontPath
```

`fontPath` is set via context property in main.cpp.

- [ ] **Step 2: Commit**

```bash
git add src/client/ui/overlay.qml && git commit -m "refactor: font path from context property, remove hardcoded system path"
```

---

### Task 4: Delete third_party/cuda, create third_party/fonts

- [ ] **Step 1: Delete cuda directory**

```bash
rm -rf third_party/cuda
mkdir -p third_party/fonts
```

- [ ] **Step 2: Download MaterialIcons font for bundling**

```bash
curl -L -o third_party/fonts/MaterialIcons-Regular.ttf \
  https://raw.githubusercontent.com/google/material-design-icons/master/font/MaterialIcons-Regular.ttf
```

- [ ] **Step 3: Update .gitignore**

```
# Before
third_party/*
!third_party/.gitkeep
!third_party/README.md

# After  
third_party/*
!third_party/.gitkeep
!third_party/README.md
!third_party/fonts/
```

- [ ] **Step 4: Commit**

```bash
git add -A && git commit -m "chore: remove third_party/cuda, bundle MaterialIcons font"
```

---

### Task 5: Rewrite check-deps.sh

- [ ] **Step 1: Rewrite to check only nvvfx headers**

Remove all CUDA and VFX lib checks. Keep only:

```bash
#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
MISSING=0

check_file() {
    if [ -f "$TP/$1" ] || [ -L "$TP/$1" ]; then
        echo "  ✅ $1"
    else
        echo "  ❌ $1 — MISSING"
        MISSING=1
    fi
}

echo "=== Checking third_party/nvvfx/ ==="
check_file "nvvfx/include/nvCVImage.h"
check_file "nvvfx/include/nvCVStatus.h"
check_file "nvvfx/include/nvVideoEffects.h"

echo "=== Checking third_party/fonts/ ==="
check_file "fonts/MaterialIcons-Regular.ttf"

echo ""
if [ "$MISSING" -eq 0 ]; then
    echo "All dependencies present."
else
    echo ""
    echo "Missing dependencies. See docs/BUILD.md."
    exit 1
fi
```

- [ ] **Step 2: Commit**

```bash
git add scripts/check-deps.sh && git commit -m "refactor: check-deps.sh — nvvfx headers + font only"
```

---

### Task 6: Write install.sh

- [ ] **Step 1: Write scripts/install.sh**

```bash
#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
INSTALL_DIR="${INSTALL_DIR:-$HOME/vsr-player}"

echo -e "${GREEN}VSR Player — Installer${NC}"
echo "Install to: $INSTALL_DIR"
echo ""

# ── 1. Check system deps ──
check_pkg() {
    pkg-config --exists "$1" 2>/dev/null && echo -e "  ✅ $1" || {
        echo -e "  ${RED}❌ $1 — not found${NC}"; MISSING_SYS=1; }
}

echo "Checking system dependencies..."
MISSING_SYS=0
check_pkg Qt6Quick
check_pkg Qt6QuickControls2
check_pkg vulkan
check_pkg libavcodec
check_pkg libavformat
check_pkg libavutil
check_pkg libswscale
check_pkg portaudio-2.0

# libcuda.so.1 — runtime check
ldconfig -p | grep -q libcuda.so.1 && echo -e "  ✅ libcuda.so.1" || {
    echo -e "  ${RED}❌ libcuda.so.1 — NVIDIA driver not found${NC}"; MISSING_SYS=1; }

if [ "$MISSING_SYS" -ne 0 ]; then
    echo ""
    echo -e "${RED}Missing system dependencies.${NC}"
    echo "Arch:  sudo pacman -S qt6-base ffmpeg vulkan-devel portaudio"
    echo "Ubuntu: sudo apt install qt6-base-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libvulkan-dev portaudio19-dev"
    exit 1
fi

# ── 2. Create dirs ──
mkdir -p "$INSTALL_DIR"/{bin,lib/fonts,share/vsr-player/{qml,shaders},screenshots,config}

# ── 3. Copy binary + resources from tarball ──
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
cp "$SRC_DIR/bin/vsr-player" "$INSTALL_DIR/bin/"
cp -r "$SRC_DIR/share/vsr-player/qml/"* "$INSTALL_DIR/share/vsr-player/qml/"
cp -r "$SRC_DIR/share/vsr-player/shaders/"* "$INSTALL_DIR/share/vsr-player/shaders/"
cp "$SRC_DIR/lib/fonts/MaterialIcons-Regular.ttf" "$INSTALL_DIR/lib/fonts/"

# ── 4. Install nvidia-vfx .so files ──
echo "Installing nvidia-vfx (NVIDIA Video Effects SDK runtime)..."
pip install nvidia-vfx 2>/dev/null || pip3 install nvidia-vfx

VFX_LIB_DIR=$(python3 -c "
import importlib.util, os, nvidia_vfx
spec = importlib.util.find_spec('nvidia_vfx')
pkg_dir = os.path.dirname(spec.origin)
lib_dir = os.path.join(pkg_dir, 'libs')
print(lib_dir)
" 2>/dev/null)

if [ -d "$VFX_LIB_DIR" ]; then
    cp "$VFX_LIB_DIR"/*.so* "$INSTALL_DIR/lib/"
    echo -e "  ${GREEN}✅ nvidia-vfx .so files copied${NC}"
else
    echo -e "  ${RED}❌ Could not find nvidia-vfx libs/ directory${NC}"
    echo "  Please install nvidia-vfx manually: pip install nvidia-vfx"
    exit 1
fi

# ── 5. Copy NVRTC from system CUDA ──
SYSTEM_CUDA=""
for d in /opt/cuda/lib64 /usr/local/cuda/lib64 /usr/lib64; do
    if [ -f "$d/libnvrtc.so.13" ]; then SYSTEM_CUDA="$d"; break; fi
done

if [ -n "$SYSTEM_CUDA" ]; then
    cp "$SYSTEM_CUDA/libnvrtc.so.13" "$INSTALL_DIR/lib/"
    cp "$SYSTEM_CUDA/libnvrtc-builtins.so.13.0" "$INSTALL_DIR/lib/"
    echo -e "  ${GREEN}✅ NVRTC from $SYSTEM_CUDA${NC}"
else
    echo -e "  ${RED}❌ libnvrtc.so.13 not found. Install CUDA Toolkit: sudo pacman -S cuda${NC}"
    exit 1
fi

# ── 6. Create .so symlinks ──
cd "$INSTALL_DIR/lib"
ln -sf libnvrtc.so.13 libnvrtc.so 2>/dev/null || true
ln -sf libnvrtc-builtins.so.13.0 libnvrtc-builtins.so 2>/dev/null || true

# VFX symlinks
for lib in libnvidia-ngx-vsr libnppc libnppial libnppicc libnppidei \
           libnppif libnppig libnppim libnppist libnppitc \
           libnvinfer libnvinfer_plugin libnvonnxparser libcudnn; do
    target=$(ls ${lib}.so.* 2>/dev/null | head -1)
    [ -n "$target" ] && ln -sf "$(basename "$target")" "${lib}.so" 2>/dev/null || true
done

# ── 7. Verify ──
echo "Verifying..."
MISSING_LIB=0
check_lib() {
    [ -f "$INSTALL_DIR/lib/$1" ] && echo -e "  ✅ $1" || {
        echo -e "  ${RED}❌ $1${NC}"; MISSING_LIB=1; }
}

check_lib "libnvVFXVideoSuperRes.so"
check_lib "libVideoFX.so"
check_lib "libnvrtc.so.13"
check_lib "MaterialIcons-Regular.ttf"

if [ "$MISSING_LIB" -ne 0 ]; then
    echo -e "${RED}Some libraries missing.${NC}"
    exit 1
fi

# ── 8. Done ──
echo ""
echo -e "${GREEN}Installation complete!${NC}"
echo ""
echo "Add to PATH:"
echo "  export PATH=\$PATH:$INSTALL_DIR/bin"
echo ""
echo "Run:"
echo "  vsr-player <video_file>"
```

- [ ] **Step 2: Make executable, test**

```bash
chmod +x scripts/install.sh
./scripts/install.sh
```

- [ ] **Step 3: Commit**

```bash
git add scripts/install.sh && git commit -m "feat: add install.sh — user-level installer"
```

---

### Task 7: Update BUILD.md

- [ ] **Step 1: Update docs/BUILD.md**

- Remove `third_party/cuda/` setup section
- Add note: NvVFX headers from GitHub (MIT)  
- Add note: NVRTC bundled in release, for dev: install `cuda` package
- Add "Installing from Release" section pointing to install.sh

- [ ] **Step 2: Commit**

```bash
git add docs/BUILD.md && git commit -m "docs: update BUILD.md for new release flow"
```

---

### Task 8: Final verification

- [ ] **Step 1: Clean build**

```bash
make clean && make -j$(nproc)
```

Expected: `All dependencies present.`

- [ ] **Step 2: Dev mode test**

```bash
./build/vsr-player input
```

Expected: player launches, QML loads, font renders.

- [ ] **Step 3: Release layout test**

```bash
mkdir -p /tmp/vsr-test && cd /tmp/vsr-test
tar xzf /path/to/vsr-player-*.tar.gz
./install.sh
~/vsr-player/bin/vsr-player ~/input/test.mp4
```

Expected: player launches from install dir, font renders from bundled path, screenshots go to `~/vsr-player/screenshots/`.
