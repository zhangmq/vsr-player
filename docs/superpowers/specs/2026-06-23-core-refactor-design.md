# Core/Client Refactor — Design

> 重构目标：将 MainWindow（当前 ~700 行 god class）中的播放管线逻辑迁移到 PlayerCore，使 core 成为自包含的视频播放引擎，client 退化为纯 Qt 外壳。

## 架构概览

```
┌─ Qt 主线程 ─────────────────────────────┐
│  MainWindow                              │
│  ├── UI controls (play/pause/overlay)    │
│  ├── VulkanWidget (wl_surface 载体)       │
│  ├── Screenshot (PNG via libpng)         │
│  └── Event → UI update (QMetaObject)      │
│                                          │
│  send_command() ──(CommandQueue)──►       │
│  ◄── EventCallback ────────────────      │
├─────────────────────────────────────────┤
│  PlayerCore Worker 线程                   │
│  ┌──────────────────────────────────┐    │
│  │ Demuxer → Decoder → NV12ToRGB    │    │
│  │              ↓                   │    │
│  │         VSRProcessor (AI)        │    │
│  │              ↓                   │    │
│  │   D2D → InteropTexture (GPU)     │    │
│  │              ↓                   │    │
│  │   VulkanRenderer → swapchain     │    │
│  │              ↓                   │    │
│  │         屏幕呈现                  │    │
│  │                                  │    │
│  │ AudioOutput → PortAudio → clock  │    │
│  └──────────────────────────────────┘    │
└──────────────────────────────────────────┘
```

**核心原则：** Core 拥有完整的视频播放能力（demux、decode、VSR、Vulkan 渲染、音频输出、A/V 同步）。Client 只是 Qt 窗口 + 命令转发 + 截图。

## Module Map

### 迁移

| 数据/逻辑 | 当前位置 (MainWindow) | 目标位置 (PlayerCore) |
|-----------|----------------------|----------------------|
| 管线组件 (Demuxer/Decoder/VSR/Audio/NV12ToRGB) | MainWindow 成员 | PlayerCore 成员 |
| GPU buffers (rgb_gpu_, cuda_stream_) | MainWindow 成员 | PlayerCore 成员 |
| CUDA Context (CUDAContext) | MainWindow 成员 | PlayerCore 成员 |
| YUV420P→NV12 interleave | MainWindow inline | PlayerCore::process_one_frame() |
| D2D copies → InteropTexture | MainWindow inline | PlayerCore::process_one_frame() |
| Vulkan render + present | VulkanWidget → VulkanRenderer | PlayerCore → VulkanRenderer |
| A/V sync throttle | MainWindow::on_timer_tick() | PlayerCore::process_one_frame() |
| 自适应 scale 计算 | MainWindow::update_scale() | PlayerCore::cmd_resize() |
| 截图 DtoH + PNG 保存 | MainWindow | MainWindow (数据由 core 提供) |

### 文件变化

| 文件 | 变化 | 行数变化 |
|------|------|---------|
| `src/client/MainWindow.h` | 重写 | 100 → ~50 |
| `src/client/MainWindow.cpp` | 重写 | 700 → ~250 |
| `src/client/VulkanWidget.h` | 大幅删减 | 50 → ~25 |
| `src/client/VulkanWidget.cpp` | 大幅删减 | 90 → ~25 |
| `src/core/api/Player.h` | 扩展 API | 95 → ~160 |
| `src/core/PlayerCore.h` | 重写 | 55 → ~120 |
| `src/core/PlayerCore.cpp` | 重写 | 90 → ~550 |
| `src/core/AudioOutput.h` | 小改 | 增加 `write_pcm(const float*, int count)` 接口 |
| `src/client/main.cpp` | 小改 | 调整初始化顺序 |
| `Makefile` | 小改 | 更新依赖 |

## Core API

### 命令 (Client → Core)

| 命令 | 参数 | 行为 |
|------|------|------|
| `LOAD_FILE` | arg = 文件路径 | 构建完整管线，成功 emit `VIDEO_INFO`，失败 emit `ERROR` |
| `OPEN_STREAM` | arg = URL，options = avformat 参数 | 同上，流式源可能无 duration、不可 seek |
| `PLAY` | — | 开始帧循环 + 音频恢复 |
| `PAUSE` | — | 冻结帧循环 + 音频暂停 |
| `STOP` | — | 释放管线，回到 STOPPED |
| `SEEK` | seek_ms = 目标位置 | 冲刷解码器 + 音频跳转 |
| `RESIZE` | seek_ms = phys_w，volume = phys_h | 重建 swapchain + 重新计算自适应 scale |
| `GET_TRACKS` | — | 查询音轨/字幕列表，emit `TRACKS_INFO` |
| `SET_AUDIO_TRACK` | seek_ms = 轨道索引 | 切换音轨 |
| `SET_SUBTITLE_TRACK` | seek_ms = 轨道索引（-1 = 关闭） | 切换字幕 |
| `ADD_SUBTITLE` | arg = 字幕文件路径 | 加载外挂字幕（.ass/.srt/.vtt），emit `TRACKS_INFO` |
| `REMOVE_SUBTITLE` | seek_ms = 轨道索引 | 移除外挂字幕 |
| `SET_QUALITY` | arg = "low"/"medium"/"high"/"ultra" | VSR reconfigure |
| `SET_SCALE` | seek_ms = 1-4 | VSR 输出倍率 |
| `SET_VOLUME` | volume = 0.0-1.0 | 音频音量 |
| `SET_MUTE` | seek_ms = 1/0 | 静音/取消 |
| `SET_PLAYBACK_SPEED` | speed = 0.5/1.0/1.5/2.0 | 变速播放 |
| `SET_LOOP` | seek_ms = 0/1/2 | 循环模式（不循环/单文件/列表） |
| `CAPTURE_FRAME` | — | 下个渲染帧 DtoH 拷贝，emit `FRAME_CAPTURED` |
| `QUIT` | — | 退出 worker 线程 |

### 事件 (Core → Client)

| 事件 | 触发时机 | 关键字段 |
|------|---------|---------|
| `STATE_CHANGED` | PLAY/PAUSE/STOP/EOS 后 | state |
| `POSITION_CHANGED` | 每 ~200ms | time_ms, duration_ms |
| `VIDEO_INFO` | LOAD_FILE/OPEN_STREAM 成功 | in_width/height, fps, duration_ms, hw_decoding, has_audio, seekable |
| `TRACKS_INFO` | 文件加载后 / 轨道切换后 | tracks[] (type, index, language, title, codec, channels 等) |
| `FRAME_INFO` | 每帧渲染后 | in/out_width/height, scale, quality, hw_decoding, vsr_active |
| `ERROR` | 管线错误 | error_msg |
| `END_OF_FILE` | 解复用器 EOF | — |
| `FRAME_CAPTURED` | CAPTURE_FRAME 后下一帧 | capture_orig/size, capture_vsr/size（指针仅回调期间有效） |

### 数据结构

```cpp
struct TrackInfo {
    enum Type { AUDIO, SUBTITLE, VIDEO };
    Type type;
    int index;
    bool external = false;
    bool active = false;
    std::string audio_codec;
    int channels = 0;
    int sample_rate = 0;
    std::string subtitle_codec;
    std::string language;
    std::string title;
};
```

### 线程安全

| 接口 | 调用线程 | 执行线程 | 同步 |
|------|---------|---------|------|
| `send_command()` | 任意 | Worker | CommandQueue (mutex + cv) |
| `initialize()` | 主线程 | VkDevice/Surface 在主线程创建，CUDA context 在 worker 创建 | 主线程阻塞直到 worker 就绪 |
| `shutdown()` | 主线程 | Worker join | atomic + join |
| `event_cb_` | Worker | Worker | 回调中 QMetaObject::invokeMethod 切主线程 |

## 初始化顺序

```
1. MainWindow 构造 + show()
     → VulkanWidget::showEvent() → wl_surface 创建
2. window.init_player(use_vsr, quality)
     → player_->initialize(wl_surface, wl_display)
     → Core: 创建 VkInstance/Device/Surface，启动 worker 线程（空闲等命令）
3. window.open_file(path)
     → send_command({LOAD_FILE})
     → Core: demux→decode→VSR→GPU buffer 分配
     → emit VIDEO_INFO
4. 收到 VIDEO_INFO:
     → resize(video_w, video_h)  // 按视频比例调整窗口
     → resizeEvent 触发 send_resize()
     → send_command({RESIZE, phys_w, phys_h})
     → Core: swapchain 以物理像素创建 + 管线初始化
     → send_command({PLAY})
     → Core: 帧循环开始
```

不再有 `resize(1280, 720)` 硬编码。swapchain 只在收到明确物理像素尺寸后创建。

### 边缘情况

- **视频大于屏幕**（如 4K 视频在 1080p 屏幕上）：`resize()` 需限制窗口不超过屏幕可用尺寸（`QScreen::availableGeometry()`），这是 client 职责。
- **窗口最小化**（0×0）：`send_resize()` 检测 phys_w/phys_h 有一边 ≤ 0 时跳过，不发送 RESIZE。Core 侧收到 RESIZE 时验证尺寸 > 0。
- **RESIZE 先于 VIDEO_INFO**：Core 在管线未就绪时收到 RESIZE，只记录 pending size。管线构建完成后再应用。
- **LOAD_FILE 失败后重新 LOAD**：`cmd_stop()` + `cmd_load_file()`，复用同一 worker 线程，重建全部管线。

### 物理像素 vs 逻辑像素

- Client 负责 DPR 转换：`phys = logical × devicePixelRatio`
- Core 只收物理像素（swapchain 尺寸）
- `RESIZE` 命令传入物理像素宽高
- 自适应 scale 用物理像素 / 视频原始像素计算

## PlayerCore 内部

### run_loop

```
while running_:
    处理所有积压命令（try_pop，非阻塞）
    if PLAYING:
        process_one_frame():  读包→解码→NV12→RGB→VSR→D2D→Vulkan→A/V sync
        如果 EOF → STOP + emit END_OF_FILE
    else:
        sleep 5ms
```

### 命令延迟

- 最坏情况：当前帧渲染完成后处理（~16ms @ 60fps），人完全无感
- CAPTURE_FRAME 设置 pending 标志，实际捕获发生在下一帧

## AudioOutput 改造

当前 AudioOutput 自己打开文件 + 解码（独立 FFmpeg 实例）。重构后改为纯 PCM sink：

```cpp
class AudioOutput {
public:
    // 新增：由 Core 喂 PCM 数据，不再自己打开文件
    bool open(int sample_rate, int channels);  // 无文件路径参数
    void write_pcm(const float* interleaved, int num_samples);  // 写入 ring buffer
    void close();

    // 保留：PortAudio 线程 + 时钟
    bool start();
    void stop();
    void pause();
    void resume();
    void seek(double target_sec);
    double clock_sec() const;
    bool is_active() const;
};
```

Core 在 `process_one_frame()` 中处理音频包：`demuxer_->read_packet()` 读到音频包 → FFmpeg 软件解码 → PCM float32 → `audio_->write_pcm()`。

## 截图流程

```
S 键 (Qt 主线程)
  → send_command({CAPTURE_FRAME})
Worker:
  → capture_pending_ = true
  → 下一帧 process_one_frame() 尾声:
    - DtoH rgb_gpu_ → capture_orig_buf_ (float32 CHW → uint8 RGB)
    - DtoH vsr_out   → capture_vsr_buf_  (RGBA → uint8 RGB, 尊重 pitch)
    - emit FRAME_CAPTURED（携带 capture buffer 指针）
Qt 主线程 (callback 内):
  → MainWindow::on_player_event(FRAME_CAPTURED)
  → memcpy 到本地 buffer（同步，立即）
  → libpng 写 PNG 到 ./screenshots/
```

## 暂不实施

以下已在 API 层面预留，但本次重构不实现：

- 流式源的 buffering 事件和断连重试
- 字幕渲染管线（libass → Vulkan overlay）
- 播放列表管理（client 维护，core 只接收 LOAD_FILE）
- FramePool（需要再补）

## 状态机

```
              LOAD_FILE / OPEN_STREAM
STOPPED ──────────────────────────────► STOPPED (管线就绪)
  ↑                                        │
  │ STOP / EOF / ERROR                     │ PLAY
  │                                        ▼
  │                                    PLAYING ◄──────┐
  │                                      │  │          │
  │                                      │  │ SEEK     │
  │                              PAUSE   │  │          │
  │                                ↑     │  └──────────┘
  │                                │     │
  │                                └─────┘
  │                              PLAYING → PAUSED
  │                                        │
  │ STOP                                   │
  └────────────────────────────────────────┘
```
