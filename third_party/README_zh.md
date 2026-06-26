# third_party/ — 第三方依赖

## 目录结构

```
third_party/
├── cuda/                          # CUDA Toolkit（不纳入 git）
│   ├── include/ → cuda.h, nvrtc.h
│   └── lib/     → libnvrtc.so.13, libnvrtc-builtins.so.13.0
├── nvvfx/                         # NVIDIA Video Effects SDK
│   ├── include/ → nvCVImage.h, nvCVStatus.h, nvVideoEffects.h（MIT，纳入 git）
│   └── lib/     → *.so（不纳入 git）
└── fonts/                         # Material Icons（Apache 2.0，纳入 git）
    └── MaterialIcons-Regular.ttf
```

---

## 1. CUDA Toolkit

**用途：** GPU 编程接口（`cuda.h`）和运行时 kernel 编译（`nvrtc.h` + `libnvrtc.so`）。

**包含：** 头文件 + NVRTC 运行时库（2 个 `.so`）。

**获取方式：** 安装 CUDA Toolkit 后从系统路径复制，或设置 `CUDA_HOME` 环境变量指向安装位置。

```bash
# Arch Linux
sudo pacman -S cuda

# 默认从 third_party/cuda/ 读取，也可通过环境变量指定
export CUDA_HOME=/opt/cuda
```

**Release 包：** NVRTC 库已随包分发，用户无需安装 CUDA Toolkit。

**协议：** NVIDIA 专有。Linux 平台允许随应用程序再分发。

---

## 2. NvVFX — NVIDIA Video Effects SDK

**用途：** AI 超分和降噪。

NvVFX 由**头文件**和**运行时库**两部分组成，分别来自不同渠道。

### 2.1 头文件

**协议：** MIT — 可自由分发，已随源码纳入 git。

**来源：** NVIDIA 官方 GitHub 仓库。

```bash
git clone https://github.com/joelvaneenwyk/nvidia-maxine-vfx
cp nvidia-maxine-vfx/nvvfx/include/nvCVImage.h      third_party/nvvfx/include/
cp nvidia-maxine-vfx/nvvfx/include/nvCVStatus.h      third_party/nvvfx/include/
cp nvidia-maxine-vfx/nvvfx/include/nvVideoEffects.h  third_party/nvvfx/include/
```

### 2.2 运行时库

**协议：** NVIDIA 专有 — **不可再分发**，不包含在 Release 包中。

**来源：** pip 包 `nvidia-vfx`。

```bash
pip install nvidia-vfx
```

该包内含全部依赖链（TensorRT、cuDNN、NPP 等，约 1.1 GB）。`install.sh` 会自动完成此步骤。

> 如果使用 NVIDIA 商业许可，也可从 [NGC](https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/collections/maxine_linux_vfx_sdk_collection_ea) 获取。

**符号链接：** 复制 `.so` 后需创建无版本号链接：

```bash
cd third_party/nvvfx/lib
for lib in libnvidia-ngx-vsr libnppc libnppial libnppicc libnppidei \
           libnppif libnppig libnppim libnppist libnppitc \
           libnvinfer libnvinfer_plugin libnvonnxparser libcudnn; do
    ln -sf $(ls ${lib}.so.* | head -1) ${lib}.so
done
```

---

## 3. 图标字体

**用途：** UI 图标（播放、暂停、音量等）。

**协议：** Apache 2.0 — 可自由分发，已随源码纳入 git。

**来源：** [Google Material Design Icons](https://github.com/google/material-design-icons)

```bash
curl -L -o third_party/fonts/MaterialIcons-Regular.ttf \
  https://raw.githubusercontent.com/google/material-design-icons/master/font/MaterialIcons-Regular.ttf
```

---

## 验证

```bash
./scripts/check-deps.sh
```

## 许可总览

| 组件 | 协议 | 再分发 |
|------|------|--------|
| CUDA Toolkit | NVIDIA 专有 | 允许（Linux 例外条款） |
| NvVFX 头文件 | MIT | 允许 |
| NvVFX 运行时 | NVIDIA 专有 | **不允许** |
| Material Icons | Apache 2.0 | 允许 |
