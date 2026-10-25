#!/usr/bin/env python3
"""rewrite-needed.py — 把 ELF 的 DT_NEEDED 从"带版本 soname"改成"无版本名"。

背景（用户 2026-10-26 定）：standard 包不捆 FFmpeg/CUDA，二进制的 NEEDED 若写死
`libavcodec.so.63`，系统升到 64 就连启动都做不到。链接器本身改不了这件事——
`DT_NEEDED` 取的是库的 `DT_SONAME`（Oracle/Debian 文档同款规则），用 `-lavcodec`
或不带版本的 `libavcodec.so` 去链接都照样记 `.so.63`（实测）。

所以这里直接改**产物**的动态段：把白名单里的 NEEDED 名字改短成无版本名
（`libavcodec.so.63` → `libavcodec.so`）。改名只发生在 .dynstr 里那一段字符串上，
新名字更短 → **原地覆写 + 补 NUL**，段偏移与其它字符串全部不动（多出来的尾部字节
变成无人引用的垃圾，无害）。随后安装时由 install.sh 按目标机实际版本建
`$LIB_DIR/libavcodec.so -> /usr/lib/libavcodec.so.<N>`。

只改白名单（FFmpeg 家族 + CUDA runtime）——它们的 soname 会随大版本跳；
libX11/libwayland/libplacebo/libass 这类系统 ABI 稳定的 soname 不动
（改成无版本名反而没有对应的软链可指）。

用法: rewrite-needed.py <elf> [<elf>...]
退出码 0 = 成功（可能 0 处修改）；2 = 参数/格式问题；1 = 内部校验失败。
"""
import re
import sys

# NEEDED → 无版本名。只覆盖"大版本会跳、且由安装时链接决定"的库。
PATTERNS = [
    re.compile(r'^libav(codec|format|util|filter|device)\.so\.\d+$'),
    re.compile(r'^libsw(scale|resample)\.so\.\d+$'),
]
# ⚠ 不要把 libnvrtc/libcudart 加进来：NVIDIA 的**符号版本命名空间就是 soname 本身**
#（libnvrtc 导出 `libnvrtc.so.13` 这个 version tag）。NEEDED 与 VERNEED 的 vna_file/
# vna_name 共用 .dynstr 里同一个偏移，把字符串截短会连 version tag 一起改掉 →
# 加载器报 "version `libnvrtc.so' not found" + 所有 nvrtc* 符号 undefined
#（2026-10-26 实测）。FFmpeg 的 version tag 是 LIBAVUTIL_61 这类，与 soname 无关，
# 且 NEEDED 与 vna_file 同步截断后仍互相一致 → 安全。
SHT_DYNAMIC = 6
SHT_STRTAB = 3
DT_NULL = 0
DT_NEEDED = 1


def unversioned(name: str) -> str:
    """"libavcodec.so.63" -> "libavcodec.so"（只切 .so. 之后）"""
    i = name.find('.so.')
    return name[:i + 3] if i >= 0 else name


def rewrite(path: str) -> int:
    data = bytearray(open(path, 'rb').read())
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        print(f"  ⚠ {path}: 不是 64-bit LE ELF，跳过", file=sys.stderr)
        return 2
    e_shoff = int.from_bytes(data[0x28:0x30], 'little')
    e_shentsize = int.from_bytes(data[0x3A:0x3C], 'little')
    e_shnum = int.from_bytes(data[0x3C:0x3E], 'little')
    e_shstrndx = int.from_bytes(data[0x3E:0x40], 'little')

    shdrs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        (sh_name, sh_type) = (int.from_bytes(data[o:o + 4], 'little'),
                              int.from_bytes(data[o + 4:o + 8], 'little'))
        sh_offset = int.from_bytes(data[o + 24:o + 32], 'little')
        sh_size = int.from_bytes(data[o + 32:o + 40], 'little')
        shdrs.append((sh_name, sh_type, sh_offset, sh_size))
    shstr_off = shdrs[e_shstrndx][2]

    def sec_name(idx: int) -> str:
        off = shstr_off + idx
        end = data.index(0, off)
        return data[off:end].decode('ascii', 'replace')

    dyn = next((s for s in shdrs if s[1] == SHT_DYNAMIC), None)
    dynstr = next((s for s in shdrs if s[1] == SHT_STRTAB and sec_name(s[0]) == '.dynstr'), None)
    if not dyn or not dynstr:
        print(f"  ⚠ {path}: 没有 .dynamic/.dynstr", file=sys.stderr)
        return 2
    dyn_off, dyn_size = dyn[2], dyn[3]
    str_off, str_size = dynstr[2], dynstr[3]

    # 收集 VERNEED 的 vna_name 偏移：这些字符串是"版本标签"，绝不能截短
    verneed_name_offsets = set()
    vr = next((x for x in shdrs if sec_name(x[0]) == '.gnu.version_r'), None)
    if vr:
        o, end = vr[2], vr[2] + vr[3]
        while o + 16 <= end:
            vn_cnt = int.from_bytes(data[o + 2:o + 4], 'little')
            vn_aux = int.from_bytes(data[o + 8:o + 12], 'little')
            vn_next = int.from_bytes(data[o + 12:o + 16], 'little')
            a = o + vn_aux
            for _ in range(vn_cnt):
                if a + 16 > end:
                    break
                verneed_name_offsets.add(int.from_bytes(data[a + 8:a + 12], 'little'))
                na = int.from_bytes(data[a + 12:a + 16], 'little')
                if na == 0:
                    break
                a += na
            if vn_next == 0:
                break
            o += vn_next

    changed = 0
    for o in range(dyn_off, dyn_off + dyn_size, 16):
        tag = int.from_bytes(data[o:o + 8], 'little', signed=True)
        if tag == DT_NULL:
            break
        if tag != DT_NEEDED:
            continue
        val = int.from_bytes(data[o + 8:o + 16], 'little')
        if val >= str_size:
            continue
        s_off = str_off + val
        end = data.index(0, s_off)
        name = data[s_off:end].decode('ascii', 'replace')
        if not any(p.match(name) for p in PATTERNS):
            continue
        if val in verneed_name_offsets:
            print(f"  ⚠ {path.split('/')[-1]}: 跳过 {name}"
                  f"（该字符串同时是 VERNEED 的 version tag，截短会破坏符号版本）",
                  file=sys.stderr)
            continue
        new = unversioned(name)
        if len(new) > len(name) or new == name:
            continue
        data[s_off:s_off + len(new)] = new.encode()
        data[s_off + len(new):end] = b'\0' * (end - s_off - len(new))
        changed += 1
        print(f"  · {path.split('/')[-1]}: {name} -> {new}")

    if changed:
        open(path, 'wb').write(bytes(data))
    return 0


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    rc = 0
    for p in sys.argv[1:]:
        r = rewrite(p)
        rc = max(rc, r if r != 0 else 0)
    return rc


if __name__ == '__main__':
    sys.exit(main())
