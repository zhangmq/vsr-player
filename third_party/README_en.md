# third_party/ — Third-Party Dependencies

## Directory Layout

```
third_party/
├── cuda/                          # CUDA Toolkit (not in git)
│   ├── include/ → cuda.h, nvrtc.h
│   └── lib/     → libnvrtc.so.13, libnvrtc-builtins.so.13.0
├── nvvfx/                         # NVIDIA Video Effects SDK
│   ├── include/ → nvCVImage.h, nvCVStatus.h, nvVideoEffects.h (MIT, in git)
│   └── lib/     → *.so (not in git)
└── fonts/                         # Material Icons (Apache 2.0, in git)
    └── MaterialIcons-Regular.ttf
```

---

## 1. CUDA Toolkit

**Purpose:** GPU programming interface (`cuda.h`) and runtime kernel compilation (`nvrtc.h` + `libnvrtc.so`).

**Contents:** Headers + NVRTC runtime libraries (2 `.so` files).

**How to obtain:** Install CUDA Toolkit and copy from system path, or set `CUDA_HOME` to point to the installation.

```bash
# Arch Linux
sudo pacman -S cuda

# Default reads from third_party/cuda/; override with env var
export CUDA_HOME=/opt/cuda
```

**Release package:** NVRTC libraries are bundled in the release tarball — users do not need to install CUDA Toolkit.

**License:** NVIDIA Proprietary. Redistribution permitted on Linux under the CUDA redistribution clause.

---

## 2. NvVFX — NVIDIA Video Effects SDK

**Purpose:** AI super-resolution and denoising.

NvVFX consists of **headers** and **runtime libraries**, obtained from separate sources.

### 2.1 Headers

**License:** MIT — freely redistributable. Included in the git repository.

**Source:** NVIDIA's official GitHub repository.

```bash
git clone https://github.com/joelvaneenwyk/nvidia-maxine-vfx
cp nvidia-maxine-vfx/nvvfx/include/nvCVImage.h      third_party/nvvfx/include/
cp nvidia-maxine-vfx/nvvfx/include/nvCVStatus.h      third_party/nvvfx/include/
cp nvidia-maxine-vfx/nvvfx/include/nvVideoEffects.h  third_party/nvvfx/include/
```

### 2.2 Runtime Libraries

**License:** NVIDIA Proprietary — **redistribution not permitted**. Not included in release packages.

**Source:** pip package `nvidia-vfx`.

```bash
pip install nvidia-vfx
```

This package contains the full dependency chain (TensorRT, cuDNN, NPP, etc., ~1.1 GB). The `install.sh` script handles this automatically.

> Users with an NVIDIA commercial license may also obtain the runtime from [NGC](https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/collections/maxine_linux_vfx_sdk_collection_ea).

**Symlinks:** After copying `.so` files, create unversioned links:

```bash
cd third_party/nvvfx/lib
for lib in libnvidia-ngx-vsr libnppc libnppial libnppicc libnppidei \
           libnppif libnppig libnppim libnppist libnppitc \
           libnvinfer libnvinfer_plugin libnvonnxparser libcudnn; do
    ln -sf $(ls ${lib}.so.* | head -1) ${lib}.so
done
```

---

## 3. Icon Font

**Purpose:** UI icons (play, pause, volume, etc.).

**License:** Apache 2.0 — freely redistributable. Included in the git repository.

**Source:** [Google Material Design Icons](https://github.com/google/material-design-icons)

```bash
curl -L -o third_party/fonts/MaterialIcons-Regular.ttf \
  https://raw.githubusercontent.com/google/material-design-icons/master/font/MaterialIcons-Regular.ttf
```

---

## Verification

```bash
./scripts/check-deps.sh
```

## License Summary

| Component | License | Redistribution |
|-----------|---------|----------------|
| CUDA Toolkit | NVIDIA Proprietary | Allowed (Linux exception) |
| NvVFX Headers | MIT | Allowed |
| NvVFX Runtime | NVIDIA Proprietary | **Not allowed** |
| Material Icons | Apache 2.0 | Allowed |
