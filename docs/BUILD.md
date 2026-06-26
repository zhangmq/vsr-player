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

The project bundles two sets of third-party files in `third_party/` (not tracked by git):

### CUDA Toolkit (headers + NVRTC)

Required for: CUDA driver API, runtime kernel compilation (NV12→RGB).

**From system CUDA installation (recommended):**

```bash
# Arch Linux
sudo pacman -S cuda

# Copy required files
cp /opt/cuda/include/cuda.h              third_party/cuda/include/
cp /opt/cuda/include/nvrtc.h             third_party/cuda/include/
cp /opt/cuda/lib64/libnvrtc.so.13        third_party/cuda/lib/
cp /opt/cuda/lib64/libnvrtc-builtins.so.13.0 third_party/cuda/lib/

# Create unversioned symlinks
cd third_party/cuda/lib
ln -sf libnvrtc.so.13            libnvrtc.so
ln -sf libnvrtc-builtins.so.13.0 libnvrtc-builtins.so
```

> After copying, you can uninstall CUDA Toolkit (`sudo pacman -R cuda`) — the project only needs these 4 files.

### NvVFX SDK Headers (MIT License)

Required for: VSR C API declarations.

```bash
git clone https://github.com/joelvaneenwyk/nvidia-maxine-vfx
cp nvidia-maxine-vfx/nvvfx/include/nvCVImage.h      third_party/nvvfx/include/
cp nvidia-maxine-vfx/nvvfx/include/nvCVStatus.h      third_party/nvvfx/include/
cp nvidia-maxine-vfx/nvvfx/include/nvVideoEffects.h  third_party/nvvfx/include/
```

### NvVFX SDK Runtime Libraries (NVIDIA Proprietary)

Required for: AI upscale/denoise inference.

**Option A — From NGC (Early Access, enterprise account required):**

Download from [NVIDIA NGC](https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/collections/maxine_linux_vfx_sdk_collection_ea) and copy all `.so*` files to `third_party/nvvfx/lib/`.

**Option B — From pip package (if you have access):**

The pip package includes pre-built `.so` files. Copy from the package's `libs/` directory.

**Create unversioned symlinks:**

```bash
cd third_party/nvvfx/lib
for lib in libnvidia-ngx-vsr libnppc libnppial libnppicc libnppidei \
           libnppif libnppig libnppim libnppist libnppitc \
           libnvinfer libnvinfer_plugin libnvonnxparser libcudnn; do
    ln -sf $(ls ${lib}.so.* | head -1) ${lib}.so
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
