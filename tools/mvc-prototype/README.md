# MVC（Blu-ray 3D）原型验证工具

用途：在**没有任何播放器改动**的前提下，验证"把 SSIF 两路视图合并成一条 MVC 码流
后，打过补丁的 ffmpeg 能否解出两视图"。全部离线（文件 I/O + 解码到内存），
不打开窗口、不做任何上屏播放。

## 依赖

- 一个打了 [FFmpeg MVC 补丁系列](https://code.ffmpeg.org/FFmpeg/FFmpeg/pulls/24303)
  的 ffmpeg 源码树（本机实测用的分支：`popcornmix/FFmpeg` 的 `dev/mvc/h264_mvc_1`），
  并且**编出静态库**（`libavcodec.a` / `libavutil.a`）供 `mvc_probe.c` 链接。
  - 最小配置（本机实测可用）：
    ```
    ./configure --disable-everything --disable-doc --disable-network --disable-autodetect \
      --disable-avdevice --disable-x86asm --enable-decoder=h264 \
      --enable-demuxer=h264,mpegts,mov,matroska --enable-parser=h264 \
      --enable-protocol=file,pipe --enable-muxer=null,rawvideo,framecrc \
      --enable-encoder=rawvideo,wrapped_avframe --enable-ffprobe
    ```
  - 本机没装 nasm，所以加了 `--disable-x86asm`（只影响性能，不影响正确性）。
- `7z`（从 ISO 里取 SSIF 文件）、`python3`。

## 步骤

1. 从 ISO 里取 SSIF 的前若干 MB（整盘 19 GB，验证只需要前 30 MB ≈ 3 秒）：
   ```
   7z e -so /path/test.iso "BDMV/STREAM/SSIF/00006.ssif" | head -c 30000000 > ssif_head.m2ts
   ```
2. 拆两路视图并按 PTS 合并成一条 MVC 码流：
   ```
   python3 ssif_split_merge.py ssif_head.m2ts merged.264
   ```
   输出应类似 `base PES: 189  dep PES: 275 ... 配对 189 个 AU`。
3. 用打过补丁的库解码并打印输出帧的尺寸/数量：
   ```
   gcc -O1 -o mvc_probe mvc_probe.c -I<ffmpeg-src> \
       <ffmpeg-src>/libavcodec/libavcodec.a <ffmpeg-src>/libavutil/libavutil.a -lm -lpthread -lz
   ./mvc_probe merged.264 0   # 只解 base view
   ./mvc_probe merged.264 1   # view_ids={0,1}，解两视图
   ```
   `mvc_probe` 会把 AU 内前两帧的 Y 平面导出成 `au_0.y` / `au_1.y`，可直接比对
   两视图是否**不同**（平帧如纯白/纯黑会给出 Δ≈0，要挑中间帧比）。

## 本机实测结论（2026-10-26）

- 拆/合并正确：同一 AU 的 base 与 dep PES 的 PTS **完全相同**（54000000、54011261…）。
- `view_ids={0,1}` 解码：**每个 AU 输出 2 帧**，各 1920×1080（**不是** SBS 打包成一帧）。
- 两帧确实是不同视角：取第 60 个 AU，Y 平面平均 |Δ| = **10.19**，|Δ|>8 的像素占 **27.6%**。
  （第 0 个 AU 是纯白帧，Δ=0，不能用来判断。）
- 解码速度（i5-14600KF，14 代）：
  - 单线程：两视图 **2.9× 实时**（189 AU / 7.9 s 素材用 2.7 s）
  - 多线程（auto）：**4.4× 实时**
- 日志里 `sps_id 1 out of range` 会出现几次但不影响解码（依赖视图 PPS 指向 subset SPS
  的那个已知提示；PR 的补丁 04 就是为它写的，仍会打印）。

## 对播放器实现的意义

MVC 解码输出的是**每个 AU 两帧**，所以播放器侧必须自己配对（Kodi 是在播放器层做的）。
我们最省事的做法：写一个 mpv `vf`（照 `vf_rot` 的 CUDA 零拷贝套路）把同 PTS 的两帧
NV12 拼成一帧 SBS（3840×1080），后面 VSR/VFG/3D 映射全部不用改（3D片源 + 左右并排 全）。
