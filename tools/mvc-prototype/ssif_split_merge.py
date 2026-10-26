#!/usr/bin/env python3
"""把 Blu-ray 3D 的 SSIF/.m2ts 拆成两路视图，再按 PTS 配对合并成一条 MVC 码流。

背景：Blu-ray 3D 的 base view（PID 0x1011）与 dependent view（PID 0x1012）在
mpegts 里是**两条独立流**，而 H.264 MVC 解码器要求两视图的 NAL **同属一个
access unit** 按 base → dependent 顺序出现。ffmpeg 的 mpegts demuxer 不做这个
关联（FFmpeg PR #24303 明确说"不在本系列"），所以这里手工做：

  1. 按 188/192 字节解析 TS（.m2ts = 4 字节 ATS + 188 字节 TS）；
  2. 按 PID 收集 PES 负载（PUSI 分界），解析 PTS；
  3. 同一 PTS 的 base PES 与 dep PES 视为同一 AU，写成 base 后 dep 的顺序。

用法：
  python3 ssif_split_merge.py <in.m2ts> <out.264>

注：输入可以是 SSIF 文件的前若干 MB（不需要整盘）；截断处会出现未配对的
dependent PES，脚本会统计并忽略。
"""
import collections
import sys


def parse_ts(path, base_pid=0x1011, dep_pid=0x1012):
    data = open(path, 'rb').read()
    stride = 192 if len(data) > 376 and data[4] == 0x47 else 188
    off0 = 4 if stride == 192 else 0
    pes = {base_pid: [], dep_pid: []}
    cur = {base_pid: None, dep_pid: None}
    for i in range(0, len(data) - (stride - 1), stride):
        pkt = data[i + off0:i + off0 + 188]
        if len(pkt) < 188 or pkt[0] != 0x47:
            continue
        pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
        if pid not in pes:
            continue
        pusi = (pkt[1] & 0x40) != 0
        afc = (pkt[3] >> 4) & 0x3
        off = 4
        if afc & 0x2:
            off += 1 + pkt[4]
        if not (afc & 0x1) or off >= 188:
            continue
        payload = pkt[off:]
        if pusi:
            if cur[pid] is not None:
                pes[pid].append(cur[pid])
            pts = None
            if len(payload) >= 14 and payload[0:3] == b'\x00\x00\x01' and (payload[7] & 0x80):
                p = payload[9:14]
                pts = (((p[0] >> 1) & 0x07) << 30 | p[1] << 22
                       | (p[2] >> 1) << 15 | p[3] << 7 | (p[4] >> 1))
            hdr = 9 + (payload[8] if len(payload) > 8 else 0)
            cur[pid] = [pts, bytearray(payload[hdr:])]
        elif cur[pid] is not None:
            cur[pid][1] += payload
    for pid in (base_pid, dep_pid):
        if cur[pid] is not None:
            pes[pid].append(cur[pid])
    return pes, base_pid, dep_pid


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    pes, base_pid, dep_pid = parse_ts(src)
    base, dep = pes[base_pid], pes[dep_pid]
    print(f'  base(PID 0x{base_pid:04x}) PES: {len(base)}   dep(PID 0x{dep_pid:04x}) PES: {len(dep)}')

    dep_by_pts = collections.defaultdict(list)
    for pts, data in dep:
        dep_by_pts[pts].append(data)
    out = bytearray()
    paired = unpaired = 0
    for pts, data in base:
        out += data
        if dep_by_pts.get(pts):
            out += dep_by_pts[pts].pop(0)
            paired += 1
        else:
            unpaired += 1
    open(dst, 'wb').write(out)
    left = sum(len(v) for v in dep_by_pts.values())
    print(f'  写出 {dst}: {len(out)} 字节；配对 {paired} 个 AU，base 未配对 {unpaired}，dep 剩余 {left}')


if __name__ == '__main__':
    sys.exit(main())
