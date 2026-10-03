# -*- coding: utf-8 -*-
"""按 FormID 反查 Starfield.esm 里记录的签名 / EDID / 记录标志。

用法: python esm_find_by_fid.py 001DC415 001DC416 0000E8EC ...
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

DEFAULT_ESM = Path(r"D:\SteamLibrary\steamapps\common\Starfield\Data\Starfield.esm")


def main() -> int:
    args = [a for a in sys.argv[1:]]
    if not args:
        print(__doc__)
        return 1
    targets = {int(a, 16) for a in args}
    d = DEFAULT_ESM.read_bytes()
    hdr = struct.unpack("<I", d[4:8])[0]
    stack = [(24 + hdr, len(d))]
    found = 0
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
            if fid in targets:
                found += 1
                print(f"=== {sig.decode('latin1')} {fid:08X} recflags=0x{fl:08X} payload={ds}")
                off = p + 24
                end = off + ds
                while off + 6 <= end:
                    s = d[off:off + 4]
                    sz = struct.unpack("<H", d[off + 4:off + 6])[0]
                    if s == b"EDID":
                        edid = d[off + 6:off + 6 + sz].split(b"\x00")[0]
                        print(f"    EDID = {edid.decode('latin1', 'replace')}")
                    elif s == b"FULL":
                        print(f"    FULL(len={sz})")
                    off += 6 + sz
            p += 24 + ds
    print("found:", found, "/", len(targets))
    return 0


if __name__ == "__main__":
    sys.exit(main())
