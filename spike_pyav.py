"""Spike: evaluate PyAV for replacing OpenCV as the decode backend.

Tests (each ~5s):
1. Software decode — fps, frame format, numpy access
2. Hardware decode (NVDEC) — availability, GPU memory path
3. Audio stream — detect, decode PCM
4. Subtitle — detect streams
5. Container capabilities — seek, duration, chapters
6. GPU zero-copy potential — CUDA HWFrame mapping
"""
import sys
import time
import av
import numpy as np


def _header(msg: str):
    print(f"\n{'='*60}")
    print(f"  {msg}")
    print(f"{'='*60}")


def test_software_decode(video_path: str):
    _header("1. Software Decode")
    container = av.open(video_path)
    video_stream = container.streams.video[0]

    print(f"  Container: {container.format.long_name}")
    print(f"  Video: {video_stream.codec_context.name} "
          f"{video_stream.width}x{video_stream.height} "
          f"@{video_stream.average_rate}")
    print(f"  Pixel fmt: {video_stream.codec_context.pix_fmt}")
    duration_s = float(video_stream.duration * video_stream.time_base) if video_stream.duration else 0
    print(f"  Duration: {duration_s:.1f}s" if duration_s else "  Duration: unknown")

    # Decode 300 frames, measure fps
    t0 = time.perf_counter()
    frame_count = 0
    first = last = None
    for frame in container.decode(video_stream):
        if frame_count == 0:
            first = frame
        last = frame
        frame_count += 1
        if frame_count >= 300:
            break
    elapsed = time.perf_counter() - t0
    fps = frame_count / elapsed if elapsed > 0 else 0

    print(f"  Decoded: {frame_count} frames in {elapsed:.1f}s → {fps:.0f} fps")
    if first:
        arr = first.to_ndarray()
        print(f"  Frame format: {first.format.name}  "
              f"ndarray shape: {arr.shape}  dtype: {arr.dtype}  "
              f"is_contiguous: {arr.flags['C_CONTIGUOUS']}")
    container.close()


def test_nvdec(video_path: str):
    _header("2. NVDEC Hardware Decode")
    container = av.open(video_path)
    video_stream = container.streams.video[0]
    codec = video_stream.codec_context

    # Check codec compatibility with NVDEC
    codec_name = codec.name
    nvdec_codecs = {"h264", "hevc", "mpeg2video", "mpeg4", "vc1", "vp8", "vp9", "av1"}
    print(f"  Codec: {codec_name}  NVDEC-supported: {codec_name in nvdec_codecs}")

    # Attempt to configure hardware decode
    # FFmpeg hwaccel can be set via codec context options
    try:
        codec.options["hwaccel"] = "cuda"
        codec.options["hwaccel_output_format"] = "cuda"
        print(f"  HW accel options set: hwaccel=cuda")
    except Exception as e:
        print(f"  HW accel setup failed: {e}")

    # Try decoding a few frames
    try:
        frame_count = 0
        t0 = time.perf_counter()
        pixel_fmts = set()
        for frame in container.decode(video_stream):
            pixel_fmts.add(str(frame.format.name))
            frame_count += 1
            if frame_count >= 10:
                break
        elapsed = time.perf_counter() - t0
        print(f"  Decoded {frame_count} frames in {elapsed:.3f}s")
        print(f"  Pixel formats seen: {pixel_fmts}")
    except Exception as e:
        print(f"  Decode error: {e}")

    container.close()


def test_audio(video_path: str):
    _header("3. Audio Stream")
    container = av.open(video_path)

    audio_streams = container.streams.audio
    if not audio_streams:
        print("  No audio streams found")
        container.close()
        return

    for i, a_stream in enumerate(audio_streams):
        ac = a_stream.codec_context
        print(f"  Audio[{i}]: {ac.name}  {ac.channels}ch  "
              f"{ac.sample_rate}Hz  layout: {ac.layout.name if ac.layout else '?'}")

        # Decode a few packets
        frame_count = 0
        total_samples = 0
        for frame in container.decode(a_stream):
            total_samples += frame.samples
            frame_count += 1
            if frame_count >= 20:
                break
        duration_s = total_samples / ac.sample_rate if ac.sample_rate else 0
        print(f"    Decoded {frame_count} audio frames ~{duration_s:.2f}s "
              f"format: {frame.format.name}")

    container.close()


def test_subtitle(video_path: str):
    _header("4. Subtitle Streams")
    container = av.open(video_path)

    sub_streams = container.streams.subtitles
    if not sub_streams:
        print("  No subtitle streams found")
    else:
        for i, s in enumerate(sub_streams):
            print(f"  Subtitle[{i}]: codec={s.codec_context.name}  "
                  f"lang={s.metadata.get('language', '?')}  "
                  f"title={s.metadata.get('title', '?')}")

    # Also list all streams
    for s in container.streams:
        print(f"  [{s.type}] index={s.index}  codec={s.codec_context.name}  "
              f"metadata={dict(s.metadata)}")
    container.close()


def test_seek(video_path: str):
    _header("5. Seek Accuracy")
    container = av.open(video_path)
    video_stream = container.streams.video[0]

    # Seek to middle
    duration = (video_stream.duration / video_stream.time_base
                if video_stream.duration else 10.0)
    target = duration / 2

    t0 = time.perf_counter()
    container.seek(int(target * av.time_base), stream=video_stream)
    # Decode one frame after seek
    for frame in container.decode(video_stream):
        pts = frame.pts * video_stream.time_base / av.time_base if frame.pts else 0
        elapsed = time.perf_counter() - t0
        print(f"  Seek target: {target:.2f}s  "
              f"first frame pts: {pts:.2f}s  "
              f"seek+decode: {elapsed*1000:.1f}ms")
        break
    container.close()


def test_gpu_path(video_path: str):
    _header("6. GPU Zero-Copy Verification")
    import ctypes, glob, torch

    # Open with HWAccel API
    hwaccel = av.codec.hwaccel.HWAccel('cuda')
    container = av.open(video_path, hwaccel=hwaccel)
    codec = container.streams.video[0].codec_context
    print(f"  HW accel active: {codec.is_hwaccel}")

    # Decode one frame
    frame = next(container.decode(video=0))
    print(f"  Frame format: {frame.format.name}  planes: {len(frame.planes)}")
    for i, p in enumerate(frame.planes):
        print(f"    plane[{i}]: {p}")

    # Verify GPU pointer access via cudaMemcpy D2D
    lib = ctypes.CDLL(
        '/home/zmq/miniforge3/envs/vsr-player/lib/python3.12/'
        'site-packages/nvidia/cu13/lib/libcudart.so.13')
    lib.cudaMemcpy.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_size_t, ctypes.c_int]
    lib.cudaMemcpy.restype = ctypes.c_int

    y_size = frame.planes[0].buffer_size
    y_gpu = torch.empty(y_size, dtype=torch.uint8, device='cuda')
    ret = lib.cudaMemcpy(y_gpu.data_ptr(),
                         frame.planes[0].buffer_ptr, y_size, 3)
    y_cpu = y_gpu.cpu()
    print(f"\n  D2D copy result: {ret} (0=success)")
    print(f"  Y plane data: min={y_cpu.min().item()} "
          f"max={y_cpu.max().item()} "
          f"mean={y_cpu.float().mean().item():.0f}")
    print(f"  Valid video data: "
          f"{y_cpu.min().item() > 10 and y_cpu.max().item() > 100}")

    # Decode perf
    container2 = av.open(video_path, hwaccel=av.codec.hwaccel.HWAccel('cuda'))
    count = 0
    t0 = time.perf_counter()
    for _ in container2.decode(video=0):
        count += 1
        if count >= 300:
            break
    fps = count / (time.perf_counter() - t0)
    print(f"\n  NVDEC perf: {fps:.0f} fps ({count} frames)")
    print(f"  vs OpenCV soft: ~600 fps")
    print(f"  Pipeline impact: NVDEC → GPU NV12 → GPU RGB → VSR (ZERO PCIe)")
    container2.close()
    container.close()


def main():
    if len(sys.argv) < 2:
        print("Usage: python spike_pyav.py <video_file>")
        sys.exit(1)
    video_path = sys.argv[1]

    print(f"PyAV version: {av.__version__}")
    print(f"FFmpeg: {av.library_versions}")

    test_software_decode(video_path)
    test_nvdec(video_path)
    test_audio(video_path)
    test_subtitle(video_path)
    test_seek(video_path)
    test_gpu_path(video_path)

    print(f"\n{'='*60}")
    print("  Evaluation complete")
    print(f"{'='*60}")


if __name__ == "__main__":
    main()
