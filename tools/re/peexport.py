"""按 RVA 查 DLL 的导出符号（转储里出现 `kernel32.dll+0x2CCB7` 这类地址时用）。

用法：
    python peexport.py kernel32.dll 0x2CCB7
    python peexport.py ntdll.dll 0xAAD40 0xAAD6C 0x160404
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

SEARCH = [Path(r"C:\Windows\System32")]


def exports(path: Path):
    buf = path.read_bytes()
    e = struct.unpack_from("<I", buf, 0x3C)[0]
    coff = e + 4
    nsec = struct.unpack_from("<H", buf, coff + 2)[0]
    opt_size = struct.unpack_from("<H", buf, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", buf, opt)[0]
    ddir = opt + (0x70 if magic == 0x20B else 0x60)
    edir_rva, edir_size = struct.unpack_from("<II", buf, ddir)
    if not edir_rva:
        return []
    sec = opt + opt_size
    def rva2off(rva):
        for i in range(nsec):
            o = sec + i * 40
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", buf, o + 8)
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return rawptr + (rva - vaddr)
        return None
    off = rva2off(edir_rva)
    nnames = struct.unpack_from("<I", buf, off + 24)[0]
    names_rva = struct.unpack_from("<I", buf, off + 32)[0]
    funcs_rva = struct.unpack_from("<I", buf, off + 28)[0]
    noff, foff = rva2off(names_rva), rva2off(funcs_rva)
    out = []
    for i in range(nnames):
        nr = struct.unpack_from("<I", buf, noff + i * 4)[0]
        fo = rva2off(nr)
        name = buf[fo:buf.index(b"\0", fo)].decode("latin1")
        frva = struct.unpack_from("<I", buf, foff + i * 4)[0]
        out.append((frva, name))
    return sorted(out)


def main() -> int:
    target, rvas = sys.argv[1], [int(x, 0) for x in sys.argv[2:]]
    path = None
    for d in SEARCH:
        p = d / target
        if p.exists():
            path = p
            break
    if path is None:
        print(f"找不到 {target}")
        return 1
    ex = exports(path)
    print(f"{path}  导出 {len(ex)} 个")
    for rva in rvas:
        prev = None
        for ea, name in ex:
            if ea <= rva:
                prev = (ea, name)
            else:
                break
        if prev:
            print(f"  0x{rva:X}  <- 最近的导出 0x{prev[0]:X} {prev[1]}  （+0x{rva - prev[0]:X}）")
        else:
            print(f"  0x{rva:X}  <- 没有更小的导出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
