# -*- coding: utf-8 -*-
"""统计 Starfield.esm 里记录头 flags 含 0x40000000（bit30）的记录分布。

用途：确认 bit30 的语义（是不是「生物部件 / 不可搜刮」类标志）。
用法: python esm_flag30_probe.py [max_edid_per_sig]
"""
from __future__ import annotations

import struct
import sys
from collections import defaultdict
from pathlib import Path

DEFAULT_ESM = Path(r"D:\SteamLibrary\steamapps\common\Starfield\Data\Starfield.esm")


def main() -> int:
    lim = int(sys.argv[1]) if len(sys.argv) > 1 else 24
    d = DEFAULT_ESM.read_bytes()
    hdr = struct.unpack("<I", d[4:8])[0]
    stack = [(24 + hdr, len(d))]
    total = defaultdict(int)
    bit30 = defaultdict(int)
    samples = defaultdict(list)
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
            sig = d[p:p + 4].decode("latin1")
            ds = struct.unpack("<I", d[p + 4:p + 8])[0]
            fl = struct.unpack("<I", d[p + 8:p + 12])[0]
            total[sig] += 1
            if fl & 0x40000000:
                bit30[sig] += 1
                if len(samples[sig]) < lim:
                    off = p + 24
                    end = off + ds
                    edid = ""
                    while off + 6 <= end:
                        s = d[off:off + 4]
                        sz = struct.unpack("<H", d[off + 4:off + 6])[0]
                        if s == b"EDID":
                            edid = d[off + 6:off + 6 + sz].split(b"\x00")[0].decode("latin1", "replace")
                            break
                        off += 6 + sz
                    samples[sig].append((fl, edid))
            p += 24 + ds

    print("== 记录头 flags 含 0x40000000（bit30）的签名分布 ==")
    for sig in sorted(bit30, key=lambda s: -bit30[s]):
        print(f"  {sig}: {bit30[sig]:6d} / {total[sig]:6d}")
        for fl, edid in samples[sig][:lim]:
            print(f"      0x{fl:08X}  {edid}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
