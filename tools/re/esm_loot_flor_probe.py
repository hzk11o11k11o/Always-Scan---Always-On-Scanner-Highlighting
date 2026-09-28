#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""esm_loot_flor_probe.py —— 列全 `Loot_*` 的 FLOR 记录（「散落物」形态的 FLOR）

为什么需要它（2026-09-28 用户实测反馈「信用条没变黄」的**根因取证**）：

  用户截图里的「信用条」在数据里**不是 MISC**，而是 **FLOR**：
      FLOR 0x3CC325 `Loot_CredStick_Common`  FULL=0x2E660「信用条」
      FLOR 0x3CC32A `Loot_CredStick_Rare`    FULL=0x2E662「信用条」
      FLOR 0x3CC32C `Loot_CredStick_Small`   FULL=0x2E65E「信用条」
  而 MOD 的 `ClassifyBase()` 把 **所有 FLOR** 一律归到「植物 / 星球目标」
  （青色脉冲 + 已扫描/未扫描判据）⇒ 这一类**散落物**永远拿不到「黄组」的颜色。

  运行期证据（本机 SAS_AlwaysScan.log）：
      flora progress probe: base=0x3CC32C … stage=4
      flora scan: base=0x3CC325 … -> 状态 7（原版「未扫描」青色脉冲）

本脚本回答两个问题：
  ① `Loot_*` 的 FLOR 一共有几条、名字分别是什么（中文名从 zhhans .strings 解析）；
     —— 决定「黄组」要补哪些 FormID，也顺便看有没有别的散落物形态（例如开锁器）。
  ② 每条的关键词 / 产出字段（PFIG）/ 模型，便于判断是不是「可拾取散落物」。

用法（需先跑 ststrings 把 zhhans .strings 抽到 out/loc_ba2，见 docs）：
    python tools/re/ba2list.py "<Data>\Starfield - Localization.ba2" --grep zhhans --extract out/loc_ba2
    python tools/re/esm_loot_flor_probe.py
"""
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ststrings import parse  # noqa: E402

DATA = Path(r'D:\SteamLibrary\steamapps\common\Starfield\Data')
ZH = Path(r'd:\workspace\starfield mod\always scan\out\loc_ba2\strings\starfield_zhhans.strings')

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


def main():
    zh = parse(ZH) if ZH.exists() else {}
    path = DATA / 'Starfield.esm'
    data = path.read_bytes()
    hdr = struct.unpack_from('<I', data, 4)[0]
    p = REC_HDR + hdr
    n = len(data)
    rows = []
    # ★ 性能：**只走 `FLOR` 那个顶层 GRUP**（= 全部 FLOR base 记录都在里面），
    #   不要逐条遍历 380 万条记录 —— 纯 Python 逐条解子记录要 ~10 分钟
    #   （第一版就是这么写的，用户直接怀疑「卡住了」）。
    seen_groups = 0
    saw_flor_group = False
    while p + REC_HDR <= n:
        if data[p:p + 4] != b'GRUP':
            break
        gsize = struct.unpack_from('<I', data, p + 4)[0]
        if gsize < REC_HDR or p + gsize > n:
            break
        label = data[p + 8:p + 12]
        seen_groups += 1
        if label == b'FLOR':
            saw_flor_group = True
            for sig, fid, payload in walk_inside(data, p + REC_HDR, p + gsize):
                if sig != 'FLOR':
                    continue
                edid = get_edid(payload)
                if not edid.lower().startswith('loot_'):
                    continue
                subs = dict()
                for ss, raw in subrecords(payload):
                    subs.setdefault(ss, raw)
                full = struct.unpack_from('<I', subs['FULL'], 0)[0] if 'FULL' in subs and len(subs['FULL']) == 4 else None
                name = zh.get(full, '') if full is not None else ''
                kws = []
                if 'KWDA' in subs and len(subs['KWDA']) % 4 == 0:
                    kws = [f'0x{struct.unpack_from("<I", subs["KWDA"], i)[0]:08X}'
                           for i in range(0, len(subs['KWDA']), 4)]
                pfig = struct.unpack_from('<I', subs['PFIG'], 0)[0] if 'PFIG' in subs and len(subs['PFIG']) == 4 else None
                modl = subs['MODL'].split(b'\x00')[0].decode('utf-8', 'replace') if 'MODL' in subs else ''
                rows.append((fid, edid, full, name, kws, pfig, modl))
        p += gsize

    print(f'顶层 GRUP 数 = {seen_groups}；找到 FLOR 组 = {saw_flor_group}')
    print(f'Loot_* FLOR 记录 = {len(rows)} 条')
    for fid, edid, full, name, kws, pfig, modl in sorted(rows):
        print(f'  FLOR 0x{fid:08X}  {edid:28s} FULL=0x{full:08X} = {name}')
        print(f'       PFIG={("0x%08X" % pfig) if pfig else "-"}  KWDA={", ".join(kws) or "-"}  MODL={modl}')
    print('done')


if __name__ == '__main__':
    sys.exit(main())
