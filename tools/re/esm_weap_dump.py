# -*- coding: utf-8 -*-
"""dump 指定 WEAP 记录的字段结构（子记录签名 + 关键数值），用于对比「可拿 / 拿不走」武器。

用法: python esm_weap_dump.py 001DC415 0000E8EC 002C7D29
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

DEFAULT_ESM = Path(r"D:\SteamLibrary\steamapps\common\Starfield\Data\Starfield.esm")


def main() -> int:
    targets = {int(a, 16) for a in sys.argv[1:]}
    if not targets:
        print(__doc__)
        return 1
    d = DEFAULT_ESM.read_bytes()
    hdr = struct.unpack("<I", d[4:8])[0]
    stack = [(24 + hdr, len(d))]
    while stack:
        a, b = stack.pop()
        p = a
        while p + 4 <= b:
            if d[p:p + 4] == b"GRUP":
                sz = struct.unpack("<I", d[p + 4:p + 8])[0]
                stack.append((p + 24, min(p + sz, b)))
                p += sz
                continue
            if p + 24 > b:
                break
            sig = d[p:p + 4]
            ds = struct.unpack("<I", d[p + 4:p + 8])[0]
            fl = struct.unpack("<I", d[p + 8:p + 12])[0]
            fid = struct.unpack("<I", d[p + 12:p + 16])[0]
            if sig == b"WEAP" and fid in targets:
                print(f"=== WEAP {fid:08X} recflags=0x{fl:08X} payload={ds}")
                off = p + 24
                end = off + ds
                while off + 6 <= end:
                    s = d[off:off + 4].decode("latin1")
                    sz = struct.unpack("<H", d[off + 4:off + 6])[0]
                    body = d[off + 6:off + 6 + sz]
                    if s == "EDID":
                        print(f"  EDID = {body.split(b'\\x00')[0].decode('latin1', 'replace')}")
                    elif s in ("OBND", "KWDA", "DNAM", "DATA", "WNAM", "VNAM", "UNAM"):
                        hexs = body[:48].hex().upper()
                        fids = " ".join(f"{struct.unpack_from('<I', body, i)[0]:08X}" for i in range(0, min(len(body) - 3, 64), 4))
                        print(f"  {s} len={sz:4d} hex={hexs}")
                        print(f"        u32s={fids}")
                    else:
                        print(f"  {s} len={sz:4d}")
                    off += 6 + sz
            p += 24 + ds
    return 0


if __name__ == "__main__":
    sys.exit(main())
