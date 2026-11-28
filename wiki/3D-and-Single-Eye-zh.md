# 3D 与单眼

> [English](3D-and-Single-Eye) · 中文

VSR Player 可以驱动接受左右并排（SBS）帧缓冲的 XR 眼镜；而在你更想平着看的时候，也能把**其中一只眼**
作为普通全画幅画面输出。

## 输出模式

右键菜单里有一个小的 **3D** 分组，含三个互相独立的条目。

| 条目 | 取值 | 含义 |
|---|---|---|
| **Output mode** | Off · 3D glasses · 2D output | 送到屏幕的内容 |
| **Source layout** | Side by side · full / half，Top and bottom · full / half | *片源*如何排布两只眼 |
| **Output option** | Left eye / Right eye | 「2D output」显示哪只眼 |

这三个条目刻意保持独立 —— 选择片源布局不会改变输出模式，反之亦然。只有「Output mode」本身是一个开关：
选 3D glasses 会启动 3D（同时停止单眼模式），选 2D output 则相反。

## 在菜单中隐藏 3D 区块

如果你从不使用 3D，Preferences → Interface → **Show 3D menu** 会把整个 3D 区块（输出模式、片源
布局、输出选项）从右键菜单中移除。设置仍然保留，`J`/`K`/`L` 快捷键照常可用。

## 3D 眼镜模式

- 选择 **3D glasses** 会打开屏幕选择器（显示物理分辨率，并按屏幕记忆），并把视频窗口移成该显示器上的
  全屏演示窗口。
- 桌面保留自己的窗口与控件（播放列表、画质、3D 菜单），因此界面不会消失。
- `H` 在眼镜上显示热键叠加层；`J` 切换 SBS/TAB，`K` 交换左右眼，`L` 切换 full/half。
- 演示窗口忽略焦点与右键；退出 3D 后视频窗口回到原来的位置。

**几何**。输出帧缓冲始终是 SBS，因此单眼视口为 `outW/2 × outH`，其宽高比为 `outAspect / 2`。单眼内容
尺寸取决于片源布局：

| 片源布局 | 单眼图像 |
|---|---|
| SBS · full | `srcW/2 × srcH`，1:1 拷贝 |
| SBS · half | `srcW × srcH`（单眼沿分割轴被拉伸） |
| TAB · full | `srcW × srcH/2` |
| TAB · half | `srcW × srcH` |

随后合成器把单眼按比例装入其视口（不拉伸；宽高比不匹配时出现黑边）。分割坐标按**整数像素**处理，渲染
目标宽度向上取偶，使两只眼采样完全相同的网格 —— 半像素的差异会表现为恒定的水平视差。

**3D 下的宽高比。** 底栏弹出面板与 `A` 热键设置的是*单眼*宽高比。mpv 的 `video-aspect-override`
作用于整帧，所以该值在发送前会先换算：SBS·full = ×2，SBS·half = ×1，TAB·full = ÷2，TAB·half = ×1。
普通 2D 播放时该值原样传递。

## 单眼（「2D output」）模式

适合只想看电影的场合：播放器取 SBS/TAB 片源的其中一只眼，按普通画面呈现，填满一个普通窗口，无需眼镜
即可观看。

- `vf_eye` 滤镜在超分**之前**运行，因此 VSR 作用在单眼画面上（每眼 1080p 的片源会得到 1:1 输出，
  而不是把两只眼一起压宽的整帧）。
- **half** 片源由滤镜本身（而不是像素宽高比）拉伸回自然单眼尺寸，因此宽高比覆盖、旋转、自动缩放的 VSR
  与插帧的表现都与普通 2D 完全一致。
- 没有 3D 演示窗口，也不用第二块显示器：画面留在普通播放器窗口中，桌面界面照常工作。
- 在该模式下按 `K` 可切到另一只眼。

同一个滤镜在 `mpv-vsr` CLI 中也可用，并可通过 JSON IPC 调用
（`set-eye off|left|right [sbs|tab] [full|half]`、`get-eye`）。

## 3D 片源的自动缩放

`scale=auto` 时，超分倍率按**单眼**几何计算，而不是按整个并排帧：

```
eyeW = outW/2, eyeH = outH                     # framebuffer is always SBS
imgW,imgH = per-eye source pixels (table above)
need = min(eyeW/imgW, eyeH/imgH)               # then snapped to the SDK set 4/3, 1.5, 2, 3, 4
```

在 3840×1080 帧缓冲、输出 60 fps 下的实测（RTX 5060 Ti）：

| 片源 | 倍率 | VSR 耗时 | 结果 |
|---|---|---|---|
| 3840×1080 SBS full（1080p/眼） | 1.00 | 直通 | 60.0 fps，0 丢帧 |
| 1280×720 SBS full | 1.50 | 3.4 ms | 60.0 fps，0 丢帧 |
| 960×720 TAB full | 2.00 | 2.3 ms | 60.0 fps，0 丢帧 |
| 640×360 SBS full | 3.00 | 1.0 ms | 60.0 fps，0 丢帧 |

每眼 1080p 的 3D 内容再加 ×2 插帧，GPU 占用约 13 %：昂贵的情形是低分辨率片源，而不是 3D 通路本身。

## 把 3D 蓝光转换为 SBS

播放器刻意不解码 MVC（H.264 Stereo High）光盘 —— 那需要软件解码两个相互依赖的视图，并且会改动整条
GPU 管线，对于一张盘只做一次的操作并不值得。离线路线更简单也更可靠，所用脚本是公开的：
<https://gist.github.com/zhangmq/cb457cb5263f2101b7adf36d81b46e31>。这套管线（`ssif_to_mvc.py` +
`mvc2sbs`）会把 3D 蓝光转成全宽 SBS 文件，可用「3D glasses · Side by side · full」播放。
