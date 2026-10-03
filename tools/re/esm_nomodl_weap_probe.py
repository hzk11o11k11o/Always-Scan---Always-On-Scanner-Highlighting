# -*- coding: utf-8 -*-
"""列出 Starfield.esm 里「无 MODL 且无 0x04」的 WEAP（= 判空会误算「可拿」的虚拟武器候选）。

用法: python esm_nomodl_weap_probe.py
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

DEFAULT_ESM = Path(r"D:\SteamLibrary\steamapps\common\Starfield\Data\Starfield.esm")


def main() -> int:
    d = DEFAULT_ESM.read_bytes()
    hdr = struct.unpack("<I", d[4:8])[0]
    stack = [(24 + hdr, len(d))]
    out = []
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
            if sig == b"WEAP" and not (fl & 0x04):
                off = p + 24
                end = off + ds
                edid = ""
                modl = False
                full = False
                while off + 6 <= end:
                    s = d[off:off + 4]
                    sz = struct.unpack("<H", d[off + 4:off + 6])[0]
                    if s == b"EDID" and not edid:
                        edid = d[off + 6:off + 6 + sz].split(b"\x00")[0].decode("latin1", "replace")
                    elif s == b"MODL":
                        modl = True
                    elif s == b"FULL":
                        full = True
                    off += 6 + sz
                if not modl:
                    out.append((fid, edid, full))
            p += 24 + ds
    print("无 MODL 且无 0x04 的 WEAP 共", len(out))
    for fid, edid, full in out:
        print(f"  0x{fid:08X} FULL={'Y' if full else 'N'}  {edid}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
