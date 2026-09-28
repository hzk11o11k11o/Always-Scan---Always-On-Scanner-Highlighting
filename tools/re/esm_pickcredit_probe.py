#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""esm_pickcredit_probe.py —— 「开锁器 / 信用币」黄组分类判据的离线取证

用途（配合 `颜色分类.md` 需求：黄 = 开锁器 + 信用币）：
  ① 全量扫 Starfield.esm，列出**所有** EDID 含 digipick / credit 的记录
     （sig / FormID / EDID）—— 确认「开锁器 / 信用币」是不是各只有一条；
  ② 对其中 MISC 记录打印**完整关键词表**（KWDA → KYWD EDID）
     —— 判断能不能用「关键词命中」当判据（比写死 FormID 更能覆盖 mod 新增物品）；
  ③ 顺带统计 MISC 总数，核对 docs/16 的 1319 条口径。

用法: python tools/re/esm_pickcredit_probe.py
"""
import re
import struct
import sys
import zlib

PATH = r'D:\SteamLibrary\steamapps\common\Starfield\Data\Starfield.esm'
REC_HDR = 24
FLAG_COMPRESSED = 0x00040000
WANT_RE = re.compile(r'digipick|credit', re.I)


def subrecords(payload):
    out = []
    off, n = 0, len(payload)
    while off + 6 <= n:
        sig = payload[off:off + 4].decode('ascii', errors='replace')
        size = struct.unpack_from('<H', payload, off + 4)[0]
        if off + 6 + size > n:
            break
        out.append((sig, payload[off + 6:off + 6 + size]))
        off += 6 + size
    return out


def get_edid(payload):
    for sig, raw in subrecords(payload):
        if sig == 'EDID':
            return raw.split(b'\x00')[0].decode('utf-8', errors='replace')
        if sig in ('VMAD', 'OBND'):
            break
    return ''


def walk_inside(data, a, b):
    p = a
    while p + REC_HDR <= b:
        if data[p:p + 4] == b'GRUP':
            sz = struct.unpack_from('<I', data, p + 4)[0]
            if sz < REC_HDR or p + sz > b:
                return
            yield from walk_inside(data, p + REC_HDR, p + sz)
            p += sz
            continue
        dsize = struct.unpack_from('<I', data, p + 4)[0]
        flags = struct.unpack_from('<I', data, p + 8)[0]
        fid = struct.unpack_from('<I', data, p + 12)[0]
        payload = data[p + REC_HDR:p + REC_HDR + dsize]
        if flags & FLAG_COMPRESSED and payload:
            try:
                payload = zlib.decompress(payload[4:])
            except Exception:
                payload = b''
        yield data[p:p + 4].decode('ascii', 'replace'), fid, payload
        p += REC_HDR + dsize


def main():
    with open(PATH, 'rb') as f:
        data = f.read()
    hdr = struct.unpack_from('<I', data, 4)[0]
    p = REC_HDR + hdr
    n = len(data)

    kywd = {}          # FormID → EDID（关键词表）
    hitted = []        # (sig, fid, edid, payload) —— 只要 MISC
    other = []         # 其他签名里命中的（只记 sig+EDID，便于确认真物品只有 MISC 一条）
    misc_total = 0

    while p + REC_HDR <= n:
        if data[p:p + 4] != b'GRUP':
            break
        gsize = struct.unpack_from('<I', data, p + 4)[0]
        if gsize < REC_HDR or p + gsize > n:
            break
        for sig, fid, payload in walk_inside(data, p + REC_HDR, p + gsize):
            edid = get_edid(payload)
            if sig == 'KYWD':
                kywd[fid] = edid
                continue
            if sig == 'MISC':
                misc_total += 1
            if WANT_RE.search(edid):
                if sig == 'MISC':
                    hitted.append((sig, fid, edid, payload))
                else:
                    other.append((sig, fid, edid))
        p += gsize

    print(f'MISC 记录总数 = {misc_total}')
    print(f'MISC 里 EDID 命中 digipick/credit 的 = {len(hitted)} 条；'
          f'其他签名命中 {len(other)} 条（不逐条打印）')
    for sig, fid, edid, payload in hitted:
        print(f'\n=== {sig} 0x{fid:08X}  {edid}')
        for ss, raw in subrecords(payload):
            if ss == 'KWDA' and len(raw) % 4 == 0:
                kws = [struct.unpack_from('<I', raw, i)[0] for i in range(0, len(raw), 4)]
                names = [kywd.get(k, f'0x{k:08X}') for k in kws]
                print(f'    KWDA n={len(kws)}: {", ".join(names)}')
            elif ss == 'FULL':
                txt = raw.split(b'\x00')[0]
                try:
                    print(f'    FULL: {txt.decode("utf-8")}')
                except Exception:
                    print(f'    FULL: {txt.hex(" ")}')
    print('\ndone')


if __name__ == '__main__':
    sys.exit(main())
