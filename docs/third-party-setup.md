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

## TensorRT

RIFE 引擎用系统 `trtexec` 构建（`bash tests/fruc/build_rife_full_engine.sh`）。

⚠ **用 pip wheel 装的 TensorRT（`site-packages/tensorrt_libs/`）需要显式
`LD_LIBRARY_PATH`**：那批 `libnvinfer_builder_resource_sm*.so.*` 的 SONAME 是伪造的
`do_not_link_against_nvinfer_builder_resource_smNN`（实测 `readelf -d`），`ldconfig`
只会按这个假名建索引，而 TRT 运行时是**按文件名** `dlopen` 的。放在发行版默认库目录
（如 `/usr/lib`）时能命中（加载器会回落到默认搜索路径），放在 `/usr/local/lib`、
venv 的 `site-packages/tensorrt_libs` 等非默认路径时则报：

```
createInferBuilder: Error Code 6: Unable to load library:
  libnvinfer_builder_resource_sm86.so.11.3.0
```

解法（任选）：把该目录加进 `LD_LIBRARY_PATH`；或改用发行版包的 TensorRT（本机
`pacman -S tensorrt`，装到 `/usr/lib`，无需额外配置）。

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
# manylinux wheel 只在 NVIDIA 官方索引；pypi.org 上 nvidia-vfx 只有数 KB 的
# sdist 占位、没有 wheel（2026-09 实测）
pip install nvidia-vfx --extra-index-url https://pypi.nvidia.com
```

**方式 B — NGC（需 NVIDIA 账号）：**

从 [NVIDIA NGC](https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/collections/maxine_linux_vfx_sdk_collection_ea) 下载 Linux VFX SDK。

> **用户端分发（非开发机）**：发布 tarball **不含 VFX**（SLA 8.5 禁止被许可人向
> 第三方分发，EA 评估许可）。`scripts/install.sh`（用户端）检测缺失时自动从
> **NVIDIA 官方索引 `https://pypi.nvidia.com/nvidia-vfx/`** 选最新 manylinux
> x86_64 wheel，curl 下载并提取 `nvvfx/libs/*` 到 `~/.local/lib/vsr-player/`
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

### nvidia_vfx.libs/（wheel 0.2.0.0 起必需）

wheel 里的 `libcudart-<hash>.so.12.9.79` **不在** `nvvfx/libs/` 下，而在同级的
`nvidia_vfx.libs/`。VFX 库的 RPATH 为 `$ORIGIN/../../nvidia_vfx.libs`，所以这个目录
必须与 `nvvfx/` 保持同级；扁平复制进 `nvvfx/lib/` 解析不到。

```bash
# wheel 解压后
mkdir -p third_party/nvidia_vfx.libs
cp nvidia_vfx.libs/*.so* third_party/nvidia_vfx.libs/
```

缺失时 `libVideoFXLocal.so` 加载失败 → `VSR: core libs failed`，VSR 静默直通（画面
照常播放，但没有超分）。

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
git -C third_party/mpv checkout a3d4b5919771a0ae351b80eb266c79681ee529b2   # 2026-07-02
```

**必须 checkout 到上面这个固定提交**——`src/mpv/` overlay 是**整文件副本**，不是
补丁，基座 revision 不匹配时会以晦涩的编译/链接错误出现（issue #12 缺陷 2 实测：
基座用当时的 master、领先 266 个提交，报
`struct MPOpts has no member named 'playlist_inherit_options'`、
`undefined reference to stream_info_env` / `stream_bstr_open2`）。

- `a3d4b591…` 是 overlay 的**实际出处**：overlay 里的 `options.c` / `options.h` 与该
  提交逐字节相同，`meson.build` 相对它只多出项目自身的改动；本仓库的合并构建
  （`scripts/build_mpv.sh` + `src/mpv/`）就是在它之上验证通过的。
- 版本对应关系是**固定提交**，不是"上游 master 开发线"。
- 自行推导别的基座时，候选提交需同时满足：① 其 `meson.build` 的源文件清单是
  overlay 版的子集（`meson.build` 变动频繁，锚点比 `options.h` 紧）②
  `options.h` / `options.c` 与 overlay 一致 ③ 能独立编译通过。
- `scripts/build_mpv.sh` 构建前会做基座预检，不匹配时打印警告（不中止——自行换基座
  是允许的）。

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
