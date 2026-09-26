#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""esm_resnode_probe.py - 「星球扫描目标（矿石/气体/液体/植物/动物）」判据取证（v4.22 用）。

要回答的问题：
  1) `ResourceType*` 关键词都挂在**哪些记录类型**上（MISC 之外还有没有 ACTI/FLOR/MSTT/STAT/BMMO…）？
  2) 带这些关键词的记录样本是什么（是不是星球上的「矿脉 / 气泉 / 液池」这类世界对象）？
  3) 星球上的资源节点用什么 EDID 命名（Deposit / Vein / Geode / Vent / Pool …）？

用法: python tools/re/esm_resnode_probe.py
      （输出较长，建议重定向留档：> out/esm_resnode_probe.txt）
"""
import struct
import zlib
from collections import Counter, defaultdict

PATH = r'D:\SteamLibrary\steamapps\common\Starfield\Data\Starfield.esm'
REC_HDR = 24
FLAG_COMPRESSED = 0x00040000

EDID_HINTS = ('Deposit', 'Vein', 'Geode', 'Vent', 'Pool', 'Resource', 'Ore',
              'Gas', 'Liquid', 'Solid', 'Mineral', 'Crystal')


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
        sig = data[p:p + 4].decode('ascii', 'replace')
        payload = data[p + REC_HDR:p + REC_HDR + dsize]
        if flags & FLAG_COMPRESSED and payload:
            try:
                payload = zlib.decompress(payload[4:])
            except Exception:
                payload = b''
        p += REC_HDR + dsize
        yield sig, fid, payload


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


def get_edid(recs):
    for sig, raw in recs:
        if sig == 'EDID':
            return raw.split(b'\x00')[0].decode('utf-8', errors='replace')
    return ''


def kwds(recs):
    for sig, raw in recs:
        if sig == 'KWDA' and len(raw) % 4 == 0:
            return [struct.unpack_from('<I', raw, i)[0] for i in range(0, len(raw), 4)]
    return []


def main():
    with open(PATH, 'rb') as f:
        data = f.read()
    hdr = struct.unpack_from('<I', data, 4)[0]
    p = REC_HDR + hdr
    n = len(data)

    kw = {}
    recs = []          # (sig, fid, edid, kws)
    while p + REC_HDR <= n:
        if data[p:p + 4] != b'GRUP':
            break
        gsize = struct.unpack_from('<I', data, p + 4)[0]
        label = data[p + 8:p + 12].decode('ascii', 'replace')
        if gsize < REC_HDR or p + gsize > n:
            break
        for sig, fid, payload in walk_inside(data, p + REC_HDR, p + gsize):
            subs = subrecords(payload)
            if sig == 'KYWD':
                kw[fid] = get_edid(subs)
            else:
                recs.append((sig, fid, get_edid(subs), kwds(subs)))
        p += gsize

    res_ids = {k for k, v in kw.items() if v.startswith('ResourceType')}
    print(f'=== 记录的记录数 = {len(recs)}；ResourceType* 关键词 = {len(res_ids)} 个 ===')

    # ① ResourceType* 关键词挂在哪些记录类型上
    by_sig = Counter()
    samples = defaultdict(list)
    for sig, fid, edid, kws in recs:
        hit = [kw.get(k, f'0x{k:08X}') for k in kws if k in res_ids]
        if hit:
            by_sig[sig] += 1
            if len(samples[sig]) < 12:
                samples[sig].append((fid, edid, sorted(hit)))
    print('--- 带 ResourceType* 关键词的记录：按类型分布 ---')
    for sig, cnt in by_sig.most_common():
        print(f'  {sig:>4}  {cnt} 条')
        for fid, edid, hit in samples[sig]:
            print(f'        {fid:08X}  {edid[:58]:<58} {",".join(x.replace("ResourceType", "") for x in hit)}')

    # ② EDID 提示词 —— 看「星球资源节点」类名字都是由什么类型承载的
    print()
    print('--- EDID 含 Deposit/Vein/Geode/Vent/Pool/Ore… 的记录：按类型分布 ---')
    hint_sig = Counter()
    hint_samples = defaultdict(list)
    for sig, fid, edid, kws in recs:
        if not edid:
            continue
        if any(h in edid for h in ('Deposit', 'Vein', 'Geode', 'Vent', 'Pool', 'Ore')):
            hint_sig[sig] += 1
            if len(hint_samples[sig]) < 10:
                hint_samples[sig].append((fid, edid))
    for sig, cnt in hint_sig.most_common(12):
        print(f'  {sig:>4}  {cnt} 条')
        for fid, edid in hint_samples[sig]:
            print(f'        {fid:08X}  {edid[:70]}')

    # ③ FLOR 记录样本（星球上的「植物」到底长什么样：有没有可扫描标志/关键词）
    flor = [(fid, edid, kws) for sig, fid, edid, kws in recs if sig == 'FLOR']
    print()
    print(f'--- FLOR 共 {len(flor)} 条；前 20 条样本 ---')
    for fid, edid, kws in flor[:20]:
        names = [kw.get(k, f'0x{k:08X}') for k in kws[:6]]
        print(f'  {fid:08X}  {edid[:44]:<44} kw={",".join(names)}')
    flor_kw = Counter()
    for fid, edid, kws in flor:
        for k in kws:
            flor_kw[kw.get(k, f'0x{k:08X}')] += 1
    print('--- FLOR 上出现最多的关键词（前 25） ---')
    for name, cnt in flor_kw.most_common(25):
        print(f'  {cnt:>6}  {name}')


if __name__ == '__main__':
    main()
