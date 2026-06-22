# third_party/ — Bundled Dependencies

这个目录包含预编译的第三方依赖，**不纳入 git 版本控制**。

`make` 编译前自动执行 `scripts/check-deps.sh` 检查文件是否齐全。

---

## 下载来源

### 来源 A：CUDA Toolkit 13.x

**安装（推荐，CachyOS / Arch Linux）：**

```bash
sudo pacman -S cuda
```

安装到 `/opt/cuda/`，对系统无副作用：
- 全部文件在 `/opt/cuda/` 一个目录下，不修改系统库路径
- 安装后文件在所有用户的只读位置，卸载 `sudo pacman -R cuda` 即完全清除
- 如果不想保留，复制所需文件到 `third_party/` 后即可卸载

**所需文件：**

| `/opt/cuda/` 下路径 | 复制到 |
|---|---|
| `include/cuda.h` | `third_party/cuda/include/cuda.h` |
| `include/nvrtc.h` | `third_party/cuda/include/nvrtc.h` |
| `lib64/libnvrtc.so.13` | `third_party/cuda/lib/libnvrtc.so.13` |
| `lib64/libnvrtc-builtins.so.13.0` | `third_party/cuda/lib/libnvrtc-builtins.so.13.0` |

```bash
cp /opt/cuda/include/cuda.h              third_party/cuda/include/
cp /opt/cuda/include/nvrtc.h             third_party/cuda/include/
cp /opt/cuda/lib64/libnvrtc.so.13        third_party/cuda/lib/
cp /opt/cuda/lib64/libnvrtc-builtins.so.13.0 third_party/cuda/lib/

cd third_party/cuda/lib
ln -sf libnvrtc.so.13            libnvrtc.so
ln -sf libnvrtc-builtins.so.13.0 libnvrtc-builtins.so
```

> 若不便安装 CUDA Toolkit，也可以从 NVIDIA 官网下载 `.run` 文件并用
> `--extract` 解压提取（无需安装），下载地址：
> https://developer.nvidia.com/cuda-downloads

---

### 来源 B：NvVFX SDK 头文件（开源，MIT 协议）

**下载地址：** https://github.com/joelvaneenwyk/nvidia-maxine-vfx

```bash
git clone https://github.com/joelvaneenwyk/nvidia-maxine-vfx
cp nvidia-maxine-vfx/nvvfx/include/nvCVImage.h      third_party/nvvfx/include/
cp nvidia-maxine-vfx/nvvfx/include/nvCVStatus.h      third_party/nvvfx/include/
cp nvidia-maxine-vfx/nvvfx/include/nvVideoEffects.h  third_party/nvvfx/include/
```

这是 NVIDIA 官方发布的 API 头文件，MIT 协议，可自由复制使用。

### 来源 C：NvVFX SDK 运行时库（NGC，需企业身份）

**下载地址：** https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/collections/maxine_linux_vfx_sdk_collection_ea

NGC 部分资源需要企业验证。此处需要的是 `.so` 运行时库（`libVideoFX.so` 等全部依赖链）。

> 如果无法从 NGC 下载，可从已有环境中复制：
> 在 `vsr-player` conda 环境的 `nvvfx/libs/` 目录下包含全部所需 `.so` 文件。

复制所有 `.so*` 文件到 `third_party/nvvfx/lib/`，然后创建无版本号符号链接：

```bash
cd third_party/nvvfx/lib
for lib in libnvidia-ngx-vsr libnppc libnppial libnppicc libnppidei \
           libnppif libnppig libnppim libnppist libnppitc \
           libnvinfer libnvinfer_plugin libnvonnxparser libcudnn; do
    ln -sf $(ls ${lib}.so.* | head -1) ${lib}.so
done
```

---

## 目录结构

```
third_party/
├── .gitkeep
├── README.md
├── cuda/
│   ├── include/
│   │   ├── cuda.h
│   │   └── nvrtc.h
│   └── lib/
│       ├── libnvrtc.so.13
│       ├── libnvrtc.so → libnvrtc.so.13
│       ├── libnvrtc-builtins.so.13.0
│       └── libnvrtc-builtins.so → libnvrtc-builtins.so.13.0
└── nvvfx/
    ├── include/
    │   ├── nvCVImage.h
    │   ├── nvCVStatus.h
    │   └── nvVideoEffects.h
    └── lib/
        ├── libVideoFX.so
        ├── libVideoFXLocal.so
        ├── libNVCVImage.so
        ├── libnvVFXVideoSuperRes.so
        ├── libnvngxruntime.so
        ├── libnvidia-ngx-vsr.so.1.8.2 → libnvidia-ngx-vsr.so
        ├── libnvinfer.so.10             → libnvinfer.so
        ├── libnvinfer_plugin.so.10      → libnvinfer_plugin.so
        ├── libnvonnxparser.so.10        → libnvonnxparser.so
        ├── libcudnn.so.9                → libcudnn.so
        └── libnpp*.so.12 ×9             → libnpp*.so
```

## 验证

```bash
./scripts/check-deps.sh
```

## 许可

`third_party/nvvfx/include/` 下的头文件为 MIT 协议（NVIDIA 开源）。

其余 `.so` 文件为 NVIDIA 专有软件，受各自许可协议约束，不属于本项目开源代码。
