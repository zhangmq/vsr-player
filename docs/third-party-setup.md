# 第三方依赖安装

> **版权说明**：`third_party/` 目录下的内容为版权敏感材料，**不加入源码管理**。
> 开发人员按下文说明自行准备。`.gitignore` 已排除整个目录。

## CUDA Toolkit

系统包安装：

```bash
# Arch Linux
sudo pacman -S cuda
```

头文件位于 `/opt/cuda/include/`，运行时库位于 `/opt/cuda/lib64/`。构建时通过 `CUDA_DIR` 变量指定（默认 `/opt/cuda`）。

## NVIDIA Video Effects SDK (NvVFX)

VFX SDK 是 AI 超分的核心依赖。包含头文件和 ~1.1GB 运行时库（TensorRT、cuDNN、NPP 等）。

### 获取

**头文件**（MIT，**不随仓库分发**，开发自行准备）：

```bash
git clone https://github.com/joelvaneenwyk/nvidia-maxine-vfx
cp nvidia-maxine-vfx/nvvfx/include/nvCVImage.h      third_party/nvvfx/include/
cp nvidia-maxine-vfx/nvvfx/include/nvCVStatus.h      third_party/nvvfx/include/
cp nvidia-maxine-vfx/nvvfx/include/nvVideoEffects.h  third_party/nvvfx/include/
```

**方式 A — pip（推荐）：**

```bash
pip install nvidia-vfx
```

**方式 B — NGC（需 NVIDIA 账号）：**

从 [NVIDIA NGC](https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/collections/maxine_linux_vfx_sdk_collection_ea) 下载 Linux VFX SDK。

> **用户端分发（非开发机）**：发布 tarball **不含 VFX**（SLA 8.5 禁止被许可人向
> 第三方分发，EA 评估许可）。`scripts/install.sh`（用户端）检测缺失时自动从
> PyPI 官方 `nvidia-vfx` wheel curl 下载提取到 `~/.local/lib/vsr-player/`
> （环境纯净：不调 pip 安装、不 sudo）；wheel 库为带版本名，脚本补无版本软链
> （vsr_proc.c 按无版本名 dlopen）。

### 安装位置

将头文件和 .so 文件放到 `third_party/nvvfx/`（不上传 git）：

```
third_party/nvvfx/
├── include/
│   ├── nvCVImage.h
│   ├── nvCVStatus.h
│   └── nvVideoEffects.h
└── lib/
    ├── libnvVFXVideoSuperRes.so
    ├── libVideoFX.so
    ├── libVideoFXLocal.so
    ├── libNVCVImage.so
    ├── libnvngxruntime.so
    ├── libnvidia-ngx-vsr.so
    ├── libnvinfer.so.10
    ├── libnvinfer_plugin.so.10
    ├── libnvonnxparser.so.10
    ├── libcudnn.so.9
    └── libnpp*.so.12 (9 个)
```

### 符号链接

复制 .so 后创建无版本号链接：

```bash
cd third_party/nvvfx/lib
for lib in libnvidia-ngx-vsr libnppc libnppial libnppicc libnppidei \
           libnppif libnppig libnppim libnppist libnppitc \
           libnvinfer libnvinfer_plugin libnvonnxparser libcudnn; do
    ln -sf $(ls ${lib}.so.* | head -1) ${lib}.so
done
```

## mpv 源码

完整 mpv 源码（构建 libmpv 与 vf_vsr 用）：

```bash
git clone https://github.com/mpv-player/mpv.git third_party/mpv
```

版本对应：上游 master 开发线（0.41.0-dev）。构建脚本 `scripts/build_mpv.sh` 将
`src/mpv/` overlay 合入 `third_party/mpv` 后构建。

## 图标字体（Material Design Icons, Pictogrammers）

QML UI 图标使用 MDI 字体，运行时从 `third_party/material-icons/` 加载
（`VSR_ICON_FONT` 编译宏指定路径，见 `src/client/meson.build`）。
字体为 v7.4.47，字形位于补充平面 PUA-B（`U+F0000`–`U+F1FFF`）。

```bash
mkdir -p third_party/material-icons
# 官方 npm 包 @mdi/font（jsDelivr CDN，无需 sudo）
curl -sL -o third_party/material-icons/materialdesignicons-webfont.ttf \
  "https://cdn.jsdelivr.net/npm/@mdi/font@7.4.47/fonts/materialdesignicons-webfont.ttf"
# 校验：文件 ~1.3MB，family 名 "Material Design Icons"
# 可访问 docs/icon-cheatsheet.html 按名称检索字形与码点
```

图标选择与码点表：`docs/icon-cheatsheet.html`（本地生成，基于实际内置字体）。
官方在线目录：<https://pictogrammers.com/library/mdi/>。

## 已移除组件（不需要再准备）

- **imgui / fontawesome**：早期客户端原型用，当前 Qt Quick 客户端不再需要，已移除。
- **client-glfw / qt-test-client / test-client**：早期原型客户端，已被现行 Qt Quick 客户端取代，已移除（git 历史可追溯）。

## 许可

| 组件 | 协议 | 再分发 |
|------|------|--------|
| CUDA Toolkit | NVIDIA 专有 | 允许（Linux） |
| NvVFX 头文件 | MIT | 允许 |
| NvVFX 运行时 | NVIDIA 专有 | **不允许** |
| MDI (Pictogrammers) | Apache 2.0 | 允许 |
