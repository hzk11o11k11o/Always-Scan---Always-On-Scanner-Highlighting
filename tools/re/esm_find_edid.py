#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""esm_find_edid.py - 按 EDID 反查 FormID（全量扫 Starfield.esm）

用途：核对 v4.17 资源自检里写死的 FormID 常量
（kResKeywordIron=0x5556E / kResKeywordDigipick=0xA / kResKeywordCredits=0xF）。

用法: python out/esm_find_edid.py
"""
import struct
import zlib

PATH = r'D:\SteamLibrary\steamapps\common\Starfield\Data\Starfield.esm'
REC_HDR = 24
FLAG_COMPRESSED = 0x00040000
WANT = {
    'InorgCommonIron', 'Digipick', 'Credits',
    'InorgCommonLead', 'InorgExoticNeon', 'InorgCommonAluminum',
}


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
        yield data[p:p + 4].decode('ascii', 'replace'), fid, flags, payload
        p += REC_HDR + dsize


def main():
    with open(PATH, 'rb') as f:
        data = f.read()
    hdr = struct.unpack_from('<I', data, 4)[0]
    p = REC_HDR + hdr
    n = len(data)
    found = {}
    while p + REC_HDR <= n:
        if data[p:p + 4] != b'GRUP':
            break
        gsize = struct.unpack_from('<I', data, p + 4)[0]
        if gsize < REC_HDR or p + gsize > n:
            break
        for sig, fid, flags, payload in walk_inside(data, p + REC_HDR, p + gsize):
            edid = get_edid(payload)
            if edid in WANT:
                found.setdefault(edid, []).append((sig, fid))
        p += gsize

    print('=== EDID -> FormID ===')
    for k in sorted(WANT):
        for sig, fid in found.get(k, [('(not found)', 0)]):
            print(f'  {k:28s} {sig} 0x{fid:08X}')
    print('done')


if __name__ == '__main__':
    main()
