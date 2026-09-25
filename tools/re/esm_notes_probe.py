#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""esm_notes_probe.py - 「笔记」类物品（书籍 / 技能杂志 / 数据板 / 音频日志）取证

需求（用户 2026-09-25 追加）：笔记类物品有 书籍、技能杂志、数据板、音频日志，
不要只处理书籍（BOOK）。

这个脚本全量遍历 Starfield.esm 的**所有**顶层 GRUP，按 EDID 关键词分组打印
（签名 / FormID / EDID），用来回答：这四类物品在数据里各自是什么记录签名？

用法: python out/esm_notes_probe.py
"""
import struct
import sys
import zlib
from collections import Counter

PATH = r'D:\SteamLibrary\steamapps\common\Starfield\Data\Starfield.esm'
REC_HDR = 24
FLAG_COMPRESSED = 0x00040000

# 每个桶：名字 -> (匹配用的子串列表，按「去掉下划线/空格后的小写串」匹配)
BUCKETS = {
    '数据板 slate':   ['slate'],
    '音频日志 audolog': ['audiolog', 'audiorec', 'recording', 'transcript'],
    '技能杂志 magazine': ['magazine', 'skillmag'],
    '书籍 book':      ['book'],
    '日记 journal':   ['journal', 'diary'],
}
MAXPER = 60


def subrecords(payload):
    out = []
    off = 0
    n = len(payload)
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
        yield data[p:p + 4].decode('ascii', 'replace'), fid, flags, payload
        p += REC_HDR + dsize


def main():
    with open(PATH, 'rb') as f:
        data = f.read()
    hdr = struct.unpack_from('<I', data, 4)[0]
    p = REC_HDR + hdr
    n = len(data)
    hits = {k: [] for k in BUCKETS}
    sig_count = {k: Counter() for k in BUCKETS}
    total = 0
    while p + REC_HDR <= n:
        if data[p:p + 4] != b'GRUP':
            break
        gsize = struct.unpack_from('<I', data, p + 4)[0]
        if gsize < REC_HDR or p + gsize > n:
            break
        for sig, fid, flags, payload in walk_inside(data, p + REC_HDR, p + gsize):
            total += 1
            edid = get_edid(payload)
            if not edid:
                continue
            flat = edid.lower().replace('_', '').replace(' ', '').replace('-', '')
            for bucket, pats in BUCKETS.items():
                if any(pat in flat for pat in pats):
                    sig_count[bucket][sig] += 1
                    if len(hits[bucket]) < MAXPER:
                        hits[bucket].append((sig, fid, edid))
        p += gsize

    print(f'=== 全量记录 {total} 条 ===\n')
    for bucket in BUCKETS:
        print(f'=== {bucket} —— 签名分布: {dict(sig_count[bucket])} ===')
        for sig, fid, edid in hits[bucket]:
            print(f'  {sig} {fid:08X}  {edid}')
        print()
    print('done')


if __name__ == '__main__':
    main()
