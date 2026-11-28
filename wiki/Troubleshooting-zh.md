# 故障排查

> [English](Troubleshooting) · 中文

## 多 GPU（核显 + 独显，或两张 NVIDIA 卡）：画面全黑 / 无 AI / 无硬件解码

整条 AI 链是 **CUDA 输出 → 零拷贝 Vulkan 导入**，因此 **CUDA 设备与 Vulkan 设备必须是同一张卡**。
下面两种症状都源自跨 GPU 的不匹配：

- 启用超分/插帧后画面变**黑**（音频与控件仍然正常），或者
- 日志报告该 Vulkan 设备无法使用 AI 处理（没有相同 UUID 的 CUDA 设备）—— 那是**显式降级模式**：
  不会黑屏，但既没有 AI 也没有硬件解码。

**先确认播放器选中了哪张卡**（启动日志）：

```
[vsr] GPU: <device name> uuid=<first 4 bytes>…
[vsr] AI processing available: <same card> ↔ CUDA <same card>      # matched
```

**让 Vulkan 与 CUDA 指向同一张卡**（两者都要设；`uuid` 取完整值，可从 `nvidia-smi -L` 或
`vulkaninfo --summary` 获得）：

```bash
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json \
CUDA_VISIBLE_DEVICES=GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx \
vsr-player <video>
```

⚠️ 有两张 NVIDIA 卡时，许多 loader 上并没有可靠的「按设备选择 Vulkan」环境变量
（`VK_LOADER_DEVICE_SELECT` 实际无效）；请以日志里的 `GPU:` 行为准，把 `CUDA_VISIBLE_DEVICES`
固定到同一张卡。`vf_hwup` 本来就按 Vulkan UUID 选择它的 CUDA 设备（`cuda-uuid` 选项），因此软解
路径也不会选错卡；找不到匹配时它会记一条警告并回退到设备 0。

## 无法启动：`error while loading shared libraries: libXXX.so.NN`

**原因**：某个系统库升级后 **soname 变了**（实测例子：libcdio `.so.19 → .so.21`，libbluray
`.so.3 → .so.4`），而已经构建好的二进制仍链接旧名字。这不是应用 bug —— 而是二进制与系统库的版本
不匹配，几乎所有 Linux 原生程序都会遇到。源码构建这条路的意义正在于让它消失。

**诊断**：

```bash
./scripts/check-deps.sh
```

- `❌ … missing: libXXX.so.NN` → 该 soname 在系统上已不存在（致命 —— 请重建或换包）
- `⚠ libXXX was replaced since the build` → 库文件被原地升级（建议重建）
- 两者都没有 → 是别的原因；提交 issue 时请附上 `ldd ~/.local/bin/vsr-player | grep "not found"`
  以及 `check-deps.sh` 的输出

**按安装方式处理**：

| 安装方式 | 操作 |
|---|---|
| **源码构建** | 重跑 `./scripts/build-from-source.sh` —— 一条命令就能重建出与当前系统库匹配的二进制。**这正是推荐源码路线的首要原因** |
| **standard 压缩包**（薄包：系统库 + 一份经校验的依赖清单；自行构建的 full 变体则携带 FFmpeg、CUDA、Qt、媒体栈、libplacebo 与 libsndio） | 它的库来自你的系统，所以 soname 提升仍可能弄坏它 —— 但不会是静默的：`install.sh` 会校验 `deps.manifest`，指出缺失的库以及谁需要它。对于已经装好的播放器，`ldd ~/.local/bin/vsr-player \| grep 'not found'` 能指出元凶。依次：① 下载**更新的压缩包** ② 改用源码构建 ③ 下面的临时办法 |

**临时办法（不要长期使用）**：为缺失的 soname 创建兼容软链 —— **仅当新旧库 ABI 兼容时才安全**：

```bash
mkdir -p ~/.local/lib/compat
ln -sf /usr/lib/libcdio.so.21 ~/.local/lib/compat/libcdio.so.19   # use the library from the error
LD_LIBRARY_PATH=~/.local/lib/compat vsr-player <video-or-directory>
```

⚠️ soname 主版本提升通常意味着 ABI 变了。本项目不使用的功能路径（CDDA、蓝光菜单）风险较低，但
**不要把它当成永久修复** —— 真正的修复是重建（源码路线）或更新的包。

**不要**在滚动发行版上单独降级某个库（部分升级）—— 它制造的不匹配比解决的还多。

## 超分没有效果（播放正常，但画面没有被超分）

当 VFX 运行时缺失或加载失败时，`vf_vsr` 会**静默直通**（视频照常播放，不报错）—— 所以「看起来正常」
并不代表 VSR 在工作。请检查：

- 启动日志里的 `VSR: nvVFX libraries loaded`；如果没有，VFX 就没加载
- `ls ~/.local/lib/vsr-player/libNVCVImage.so`，以及**不带版本号**的软链（`libnppc.so`、
  `libcudnn.so` 等）是否存在 —— `vsr_proc.c` 用不带版本号的名字 dlopen，缺一个软链就会断掉
  整条 VFX 链
- 重新获取：运行 `./scripts/install.sh`（从 NVIDIA 官方索引下载），或按
  [第三方准备](https://github.com/zhangmq/vsr-player/blob/master/docs/third-party-setup.md) 手动放置文件
- 驱动过旧同样会让 VFX 加载失败：**VFX 版本 ↔ 驱动**必须匹配（见 [安装](Installation)）

## 插帧不生效

某个构建用的是哪个后端，可以从启动时的滤镜状态行看出来：VFG（默认）是 `vfg-status:`，RIFE 是
`fruc-status:`。默认的 **VFG** 后端需要 Ada/Blackwell；在更旧的 GPU 上滤镜会保持不生效。

**仅 RIFE 后端**（`--fruc-backend=rife` 源码构建）：RIFE 引擎内嵌构建它的**确切 TensorRT 版本**；
TRT 升级后旧引擎会在反序列化时被拒绝（插帧静默降级为直通）。请用当前系统 TRT 重建：

```bash
bash tests/fruc/build_rife_full_engine.sh
```

## 首次运行像卡住 10–15 s（引擎内核缓存）

在从未跑过 CUDA 计算的 HOME 里，**首次**运行必须编译超分引擎内核，因此画面要 10–15 s 才出现。启动
日志会说明：

```
first run on this HOME: engine kernel cache is empty (<path>) — the first super-resolution
engine load compiles kernels and can take 10-15 s (one-time; later loads ~0.15 s)
```

- 每个 HOME 一次性开销（`$HOME/.nv/ComputeCache`）；之后的引擎加载约 0.15 s。
- 该缓存**与尺寸/倍率无关**：热过一次之后，其他缩放倍率/插帧倍率同样很快。
- **重启不会**使其失效。**驱动大版本升级**是否会使它失效**尚未验证**。
- 这只是一条提示 —— 没有预热：首帧仍然要等 10–15 s。

## 蓝光：没有菜单，或 Title 面板停在「Disc menu」

两件不同的事，在某些光盘上都属预期 —— 细节见 [蓝光与播放列表](Blu-ray-and-Playlists)：

- **完全没有菜单出现。** BD-J 光盘需要 Java VM 以及 libbluray 的 BD-J jar
  （`libbluray-j2se-<version>.jar`）；日志会说明这一点，播放则在无菜单的情况下继续。标题仍在
  Title 面板中。关掉 Preferences → Disc → *Boot into disc menu*，改为直接开始最长的标题。
- **正片在播放，Title 面板却一直显示 `Disc menu`。** Java 菜单从它自己的一份播放列表副本
  （`00001.mpls` 旁边的 `01001.mpls`）开始播放，而那份副本不在光盘的标题列表里，因此 mpv 没有标题
  可报，只能指向合成的 `Disc menu` 条目。画面是正确的，章节也照常可用。
- **`Ctrl+M` / `Disc menu` 一行没有反应。** 光盘屏蔽了菜单调用操作；mpv 不会去覆盖光盘自身的限制。

## 播放因显存不足或驱动错误卡片而停止

播放器拒绝静默降级：CUDA 显存不足或驱动失败会停止播放并显示提示卡。

- **「显存不足，播放已停止」** —— 卡片列出两种现实原因：VFX 设置要求超过 GPU 的显存（降低超分倍率
  或插帧画质，或关闭 SDR→HDR10），或应用存在显存泄漏（请连同日志行一起上报）。释放显存后再次按播放。
- **「播放已停止：图形驱动错误」** —— 细节在日志里；通常是驱动重置或 GPU 挂起导致。

两者之前都会在日志里留下 `vfx-error: node=… reason=… err=… free=…` 行，提交 issue 时请附上它。

## 右键菜单里没有 3D 区块

Preferences → Interface → **Show 3D menu**。该设置会被记住；3D 状态本身与 `J`/`K`/`L` 快捷键
不受影响。

## 3D / 单眼的异常现象

- **画面没有填满眼镜屏幕**：OSD/日志行 `stereo map:` 报告单眼视口（`eye=`）与实际内容尺寸
  （`content=`）；`content` 小于 `eye` 就意味着黑边，即片源宽高比与单眼宽高比不匹配。
- **左右眼反了**：按 `K`（3D 模式下）交换；该设置按会话生效。
- **排布方式选错**：把 **Source layout** 设为实际的排布（SBS/TAB、full/half）。把 TAB 片源当作
  SBS 会显得被压扁并重影。
- **2D 输出下宽高比看起来不对**：宽高比弹出面板里存的是*单眼*宽高比，所以 3D 模式下选过的值
  在只显示一只眼时同样生效。按 `A` 循环各项，或设为 `no` 回退到片源宽高比。
