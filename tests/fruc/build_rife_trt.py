#!/usr/bin/env python3
"""用 TensorRT 11 从 rife_v4.25.onnx 构建动态 shape FP16 引擎。

用法: <vfi>/bin/python build_rife_trt.py [out.engine]
引擎构建为性能评估用（整帧、动态 H/W），输出到 build/tests/fruc/（gitignored）。
"""
import sys, os, time
import tensorrt as trt

ONNX = sys.argv[1] if len(sys.argv) > 1 else \
    "/home/zmq/projects/vsr-player/build/tests/fruc/rife_autocast_fp16.onnx"  # ModelOpt AutoCast FP16
OUT = sys.argv[2] if len(sys.argv) > 2 else \
    "/home/zmq/projects/vsr-player/build/tests/fruc/rife_v4.25_dyn.engine"
os.makedirs(os.path.dirname(OUT), exist_ok=True)

logger = trt.Logger(trt.Logger.WARNING)
b = trt.Builder(logger)
net = b.create_network()
p = trt.OnnxParser(net, logger)
t0 = time.time()
with open(ONNX, "rb") as f:
    if not p.parse(f.read()):
        for i in range(p.num_errors):
            print("parse err:", p.get_error(i))
        sys.exit(1)
print(f"onnx parsed ({time.time()-t0:.1f}s), {net.num_layers} layers")

config = b.create_builder_config()
config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, 2 << 30)
profile = b.create_optimization_profile()
inp = net.get_input(0)
print("input:", inp.name, inp.shape, "min/max dynamic:", inp.shape[2] < 0)
# 动态 H/W: min 352x240, opt 1280x720, max 1920x1080
C = inp.shape[1]   # 通道数: full=7, lite=11
# lite 模型内部 pad 到 64 倍数（720->768），profile 值需 64 对齐
profile.set_shape(inp.name, (1, C, 256, 384), (1, C, 1088, 1920), (1, C, 1088, 1920))
config.add_optimization_profile(profile)

t0 = time.time()
engine = b.build_serialized_network(net, config)
if engine is None:
    print("BUILD FAILED"); sys.exit(1)
with open(OUT, "wb") as f:
    f.write(engine)
print(f"engine saved: {OUT} ({time.time()-t0:.1f}s build)")
