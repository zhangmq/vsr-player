# Build Guide

## System Requirements

| Component | Minimum Version | Notes |
|-----------|----------------|-------|
| NVIDIA Driver | 570+ | `nvidia_drm.modeset=1` recommended for Wayland |
| Qt 6 | 6.8+ | Modules: Quick, QuickControls, Vulkan |
| FFmpeg | 6.0+ | libavformat, libavcodec, libavutil, libswscale |
| PortAudio | 19+ | Audio output |
| Vulkan SDK | 1.3+ | Loader + `glslc` shader compiler |
| GCC | 13+ | C++20 required |
| GNU Make | 4+ | Build system |

## Install Dependencies

### Arch Linux

```bash
sudo pacman -S --needed \
    qt6-base qt6-declarative qt6-quickcontrols2 \
    ffmpeg portaudio vulkan-devel shaderc glslang \
    gcc make pkgconf
```

### Ubuntu 24.04+

```bash
sudo apt install -y \
    qt6-base-dev qt6-declarative-dev libqt6quickcontrols2-6 \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
    libportaudio-ocaml-dev portaudio19-dev \
    libvulkan-dev glslang-tools glslc \
    g++-13 make pkgconf
```

## Third-Party Dependencies

### CUDA Toolkit (headers + NVRTC)

Required for: CUDA driver API, runtime kernel compilation (NV12→RGB).

The Makefile uses `CUDA_HOME` environment variable (defaults to `third_party/cuda` if not set). Install CUDA Toolkit and point to it:

```bash
# Arch Linux
sudo pacman -S cuda

# Point Makefile to your CUDA installation
export CUDA_HOME=/opt/cuda
```

> If CUDA is installed at a non-standard path, set `CUDA_HOME` accordingly. The Makefile expects `$CUDA_HOME/include/` and `$CUDA_HOME/lib/`.

### NvVFX SDK Headers (MIT License)

Required for: VSR C API declarations.

```bash
git clone https://github.com/joelvaneenwyk/nvidia-maxine-vfx
cp nvidia-maxine-vfx/nvvfx/include/nvCVImage.h      third_party/nvvfx/include/
cp nvidia-maxine-vfx/nvvfx/include/nvCVStatus.h      third_party/nvvfx/include/
cp nvidia-maxine-vfx/nvvfx/include/nvVideoEffects.h  third_party/nvvfx/include/
```

### NvVFX SDK Runtime Libraries (NVIDIA Proprietary)

Required for: AI upscale/denoise inference. Install via pip:

```bash
pip install nvidia-vfx

# Copy .so files to third_party/
python3 -c "
import importlib.util, os, shutil
spec = importlib.util.find_spec('nvidia_vfx')
pkg_dir = os.path.dirname(spec.origin)
lib_dir = os.path.join(pkg_dir, 'libs')
for f in os.listdir(lib_dir):
    shutil.copy2(os.path.join(lib_dir, f), 'third_party/nvvfx/lib/')
"

# Create unversioned symlinks
cd third_party/nvvfx/lib
for lib in libnvidia-ngx-vsr libnppc libnppial libnppicc libnppidei \
           libnppif libnppig libnppim libnppist libnppitc \
           libnvinfer libnvinfer_plugin libnvonnxparser libcudnn; do
    ln -sf $(ls ${lib}.so.* | head -1) ${lib}.so
done
```

> The pip package `nvidia-vfx` bundles all required .so files (TensorRT, cuDNN, NPP, etc.). No NGC account required.

### Material Icons Font

Bundled in `third_party/fonts/`. Downloaded automatically during build. If missing:

```bash
curl -L -o third_party/fonts/MaterialIcons-Regular.ttf \
  https://raw.githubusercontent.com/google/material-design-icons/master/font/MaterialIcons-Regular.ttf
```
done
```

### Verify Dependencies

```bash
./scripts/check-deps.sh
```

Expected output: all items marked ✅.

## Build

### 1. Compile Shaders (one-time)

```bash
glslc -fshader-stage=vert src/client/shaders/video.vert -o build/video.vert.spv
glslc -fshader-stage=frag src/client/shaders/video.frag -o build/video.frag.spv
glslc -fshader-stage=frag src/client/shaders/nv12.frag -o build/nv12.frag.spv
```

### 2. Build

```bash
make -j$(nproc)
```

Output: `build/vsr-player`

### 3. Run Tests

```bash
# Decoder validation (NVDEC + hwaccel)
./build/tests/test_decoder /path/to/test-video.webm

# Full pipeline test (demux → decode → VSR → save frames)
./build/tests/test_pipeline /path/to/test-video.webm [frame_count]
```

### 4. Run Player

```bash
./build/vsr-player /path/to/video.mp4
./build/vsr-player /path/to/video/folder/
./build/vsr-player --scale 3x --quality ultra /path/to/video.mp4
```

## Troubleshooting

### "Vulkan not available" or black screen

Ensure Vulkan is working:
```bash
vulkaninfo --summary | grep -E "deviceName|apiVersion"
```

### Wayland: no Vulkan surface support

Check modesetting:
```bash
sudo cat /sys/module/nvidia_drm/parameters/modeset
```

If `N`, add `nvidia_drm.modeset=1` to kernel command line, or use XWayland fallback:
```bash
QT_QPA_PLATFORM=xcb ./build/vsr-player /path/to/video.mp4
```

### Missing .so at runtime

The player uses `RPATH $ORIGIN/lib`. Ensure VFX libraries are in `build/lib/`:
```bash
ls build/lib/libnvVFXVideoSuperRes.so
```

### NVDEC not active (software decode used)

Check driver version and codec support:
```bash
nvidia-smi
ffmpeg -hwaccel cuda -hwaccel_output_format cuda -i /path/to/video.mp4 -f null -
```

### Build fails with "cuda.h not found"

Ensure third-party dependencies are in place:
```bash
./scripts/check-deps.sh
```

## Installing from Release

Download the latest `vsr-player-<ver>-linux-x86_64.tar.gz` from [GitHub Releases](https://github.com/zhangmq/vsr-player/releases).

```bash
tar xzf vsr-player-*.tar.gz
cd vsr-player-*
./install.sh
```

The installer will:
1. Check system dependencies (Qt, FFmpeg, Vulkan, PortAudio, NVIDIA driver)
2. Install `nvidia-vfx` via pip and deploy `.so` files
3. Locate CUDA NVRTC from system or pip
4. Copy binary, QML, shaders, and font to `~/vsr-player/`

No root required. Add `~/vsr-player/bin` to `PATH` and run `vsr-player <video>`.
