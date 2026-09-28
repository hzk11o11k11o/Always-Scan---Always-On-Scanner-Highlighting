#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""esm_credit_names_probe.py —— 「信用条」到底有几条 base 记录（离线取证）

背景（2026-09-28 用户实测反馈）：
  截图里桌子上的「信用条」被描成**蓝色**（= 杂项 / 原版蓝），而不是需求要求的**黄色**
  —— 说明 `ClassifyBase()` 里写死的那三个 FormID（Digipick 0xA / Credits 0xF /
  FFNeonZ03_Credits 0xA7312）**没有命中**这个世界物件。

本脚本的取证路径（不依赖游戏、不依赖 xEdit）：
  ① 从 `Starfield - Localization.ba2` 抽出的 `starfield_zhhans.strings` 里
     拿到名字正好是「信用条」的**字符串 ID**（由 ststrings.py 事先查出，见下）；
  ② 全量扫 Starfield.esm（+ ShatteredSpace.esm / 其它插件）：所有把该 ID
     写进 `FULL` 子记录的 base 记录 = 名字叫「信用条」的物品；
  ③ 打印每条的 sig / FormID / EDID / 关键词（KWDA → KYWD EDID）/ MODL，
     用来决定「判据到底该怎么写」（FormID 白名单还是关键词）。

已查得的字符串 ID（zhhans `.strings`，见 `ststrings.py dump --grep 信用条`）：
  0x0002E65E / 0x0002E660 / 0x0002E662  —— 三个 ID 的文本都是「信用条」。
  ★ 对照：`Credits`(MISC 0xF) 的 FULL = 0x00000ED8 = **「信用币」**（不是「信用条」）、
     `Digipick`(MISC 0xA) 的 FULL = 0x00000ECC = 「撬锁器」。
  ⇒ **截图里那个物件根本不是 `Credits`**，「黄组」原来的三个 FormID 天然不可能命中它。

用法: python tools/re/esm_credit_names_probe.py
"""
import struct
import sys
import zlib
from pathlib import Path

DATA = Path(r'D:\SteamLibrary\steamapps\common\Starfield\Data')
WANT_FULL = {0x0002E65E, 0x0002E660, 0x0002E662}

REC_HDR = 24
FLAG_COMPRESSED = 0x00040000


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


def scan(path: Path):
    """→ (hits, kywd_fid->edid)"""
    data = path.read_bytes()
    hdr = struct.unpack_from('<I', data, 4)[0]
    p = REC_HDR + hdr
    n = len(data)
    hits = []
    kywd = {}
    while p + REC_HDR <= n:
        if data[p:p + 4] != b'GRUP':
            break
        gsize = struct.unpack_from('<I', data, p + 4)[0]
        if gsize < REC_HDR or p + gsize > n:
            break
        for sig, fid, payload in walk_inside(data, p + REC_HDR, p + gsize):
            subs = subrecords(payload)
            if sig == 'KYWD':
                for ss, raw in subs:
                    if ss == 'EDID':
                        kywd[fid] = raw.split(b'\x00')[0].decode('utf-8', 'replace')
                        break
                continue
            # ★ 注意：EDID 在记录最前面，**不能**用它当「提前收工」的条件
            #   （第一版就踩了这个坑：扫到 EDID 直接 break ⇒ 永远读不到 FULL）。
            full_id = None
            for ss, raw in subs:
                if ss == 'FULL' and len(raw) == 4:
                    full_id = struct.unpack_from('<I', raw, 0)[0]
                    break
            if full_id in WANT_FULL:
                hits.append((sig, fid, get_edid(payload), subs))
        p += gsize
    return hits, kywd


def main():
    all_hits = []
    # 扫 Data 下**所有**插件（含 Creation 的 SFBGS / BlueprintShips 等）
    esms = sorted(DATA.glob('*.esm'))
    for path in esms:
        name = path.name
        hits, kywd = scan(path)
        for sig, fid, edid, subs in hits:
            kws = []
            modl = ''
            for ss, raw in subs:
                if ss == 'KWDA' and len(raw) % 4 == 0:
                    kws = [kywd.get(struct.unpack_from('<I', raw, i)[0],
                                    f'0x{struct.unpack_from("<I", raw, i)[0]:08X}')
                           for i in range(0, len(raw), 4)]
                elif ss == 'MODL':
                    try:
                        modl = raw.split(b'\x00')[0].decode('utf-8', 'replace')
                    except Exception:
                        pass
            all_hits.append((name, sig, fid, edid, kws, modl))
            print(f'{name}: {sig} 0x{fid:08X} EDID={edid!r}')
            print(f'    KWDA n={len(kws)}: {", ".join(kws)}')
            print(f'    MODL: {modl}')
    print(f'\n总命中 = {len(all_hits)} 条')
    print('done')


if __name__ == '__main__':
    sys.exit(main())
