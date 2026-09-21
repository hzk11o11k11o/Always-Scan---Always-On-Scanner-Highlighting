"""PDB 反查：**源码行 → 模块内 RVA**（`pdbsym.py` 的反向操作）。

为什么需要它：改完代码要核对「编译器到底生成了什么指令」时，
手里只有源码行号（例：RefLootState 里那次库存指针读取），
而 `pdbsym.py` 是「给了 RVA 问是什么」。

实现说明（本机实测的限制，写在最前面免得下次再踩）：
  · `SymFromAddr` / `SymFromName` 在这个 releasedbg PDB 上**不返回符号**
    （返回 0/空名字），但 `SymGetLineFromAddr64` 正常工作 ⇒ 只能靠**行号**定位；
  · 于是本工具对 `.text` 逐地址问行号（×4 步进再回退一格，几秒级），
    把「命中目标行」的地址列出来（通常就是那一行对应的指令）。

用法：
    python symfind.py <a.dll> 4586              # 找「源码第 4586 行」对应的 RVA
    python symfind.py <a.dll> 4586 4622         # 多个行号
    python symfind.py <a.dll> 4586 --file AlwaysScan.cpp
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as w
import struct
import sys

SYMOPT_LOAD_LINES = 0x00000010
SYMOPT_UNDNAME = 0x00000002

LOAD_BASE = 0x7FFFD9070000
LOAD_SIZE = 0xAF000


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", w.DWORD),
        ("Key", ctypes.c_void_p),
        ("LineNumber", w.DWORD),
        ("FileName", ctypes.c_char_p),
        ("Address", ctypes.c_ulonglong),
    ]


def text_section(path: str) -> tuple[int, int]:
    with open(path, "rb") as f:
        buf = f.read()
    e = struct.unpack_from("<I", buf, 0x3C)[0]
    coff = e + 4
    nsec = struct.unpack_from("<H", buf, coff + 2)[0]
    opt_size = struct.unpack_from("<H", buf, coff + 16)[0]
    sec = coff + 20 + opt_size
    for i in range(nsec):
        o = sec + i * 40
        name = buf[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr = struct.unpack_from("<II", buf, o + 8)
        if name == ".text":
            return vaddr, vsize
    raise RuntimeError("no .text section")


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    dll = sys.argv[1]
    want_lines = [int(a) for a in sys.argv[2:] if a.isdigit()]
    want_file = ""
    if "--file" in sys.argv:
        want_file = sys.argv[sys.argv.index("--file") + 1].lower()

    dbghelp = ctypes.WinDLL("dbghelp")
    h = ctypes.c_void_p(0x1234ABCD)
    dbghelp.SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME)
    dbghelp.SymInitialize(h, None, False)
    if not dbghelp.SymLoadModuleEx(h, None, dll.encode(), None,
                                   ctypes.c_ulonglong(LOAD_BASE), LOAD_SIZE, None, 0):
        print("SymLoadModuleEx 失败")
        return 1

    dbghelp.SymGetLineFromAddr64.argtypes = [ctypes.c_void_p, ctypes.c_ulonglong,
                                             ctypes.POINTER(ctypes.c_ulonglong),
                                             ctypes.POINTER(IMAGEHLP_LINE64)]
    dbghelp.SymGetLineFromAddr64.restype = ctypes.c_int

    vaddr, vsize = text_section(dll)
    print(f"扫 .text（RVA 0x{vaddr:X} + 0x{vsize:X}）找行号 {want_lines} …")
    found: dict[int, list[tuple[int, str]]] = {ln: [] for ln in want_lines}
    line = IMAGEHLP_LINE64()
    line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
    for rva in range(vaddr, vaddr + vsize, 4):
        disp = ctypes.c_ulonglong(0)
        if not dbghelp.SymGetLineFromAddr64(h, ctypes.c_ulonglong(LOAD_BASE + rva),
                                            ctypes.byref(disp), ctypes.byref(line)):
            continue
        fn = line.FileName.decode("utf-8", "replace") if line.FileName else ""
        if want_file and want_file not in fn.lower():
            continue
        if line.LineNumber in found:
            found[line.LineNumber].append((rva, fn.split("\\")[-1]))

    for ln in want_lines:
        hits = found[ln]
        if not hits:
            print(f"  行 {ln}: 没找到")
            continue
        # 同一行的地址会成片出现（那一段指令都属于这一行），只报最小/最大
        lo, hi = hits[0], hits[-1]
        print(f"  行 {ln}: RVA 0x{lo[0]:X} .. 0x{hi[0]:X}（{len(hits)} 个探测点，{lo[1]}）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
