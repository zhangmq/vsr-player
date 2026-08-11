#!/usr/bin/env python
"""ModelOpt AutoCast FP16 转换（可复现的 ONNX 资产生成）。

用法: <vfi>/bin/python convert_rife_onnx_fp16.py <src.onnx> <dst.onnx> autocast|all
  autocast = keep_io_types=True（入口/出口保留 FP32 cast）——full 引擎用
            （rife_v4.25.onnx → rife_autocast_fp16.onnx）
  all      = keep_io_types=False（全 FP16 无 cast）——lite 引擎用
            （rife_v4.25_lite.onnx → rife_lite_fp16_all.onnx，NaN bug 规避）

依赖: miniforge3 envs/vfi（modelopt + onnx）。
"""
import sys
import onnx
import modelopt.onnx.autocast as a

def main():
    src, dst, mode = sys.argv[1], sys.argv[2], sys.argv[3]
    keep_io = (mode == "autocast")   # all → False
    model = onnx.load(src)
    out = a.convert_to_f16(model, keep_io_types=keep_io)
    onnx.save(out, dst)
    print(f"{src} -> {dst} (mode={mode}, keep_io_types={keep_io})")

if __name__ == "__main__":
    main()
