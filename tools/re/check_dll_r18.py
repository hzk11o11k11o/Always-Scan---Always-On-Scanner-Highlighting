# -*- coding: utf-8 -*-
"""核对新构建 DLL 里的订正 R18 标记（判空标定「强证据胜场 + 反证闸门」）。

用法：python tools/re/check_dll_r18.py
"""
from __future__ import annotations

import hashlib
import sys
from pathlib import Path

DLL = Path(r"d:\workspace\starfield mod\always scan\plugin\build\windows\x64\releasedbg\SAS_AlwaysScan.dll")


def main() -> int:
    d = DLL.read_bytes()
    print("DLL =", DLL)
    print("size =", len(d), "B")
    print("sha256 =", hashlib.sha256(d).hexdigest().upper())

    # ── 本轮新增（订正 R18）──
    marks_new = [
        "订正 R18",
        "InventoryListOffset",
        "inventoryListOffset",
        "跳过自动标定",
        "强证据胜场",
        "不同引用=",
        "反证=",
        "中性=",
        "弱票=",
        "形状坏=",
        "只接受 0xA0 / 0xA8",
        "st=%c/%c",
        "docs/57",
    ]
    # ── 历史标记（不许丢）──
    marks_old = [
        "订正 R17",
        "display case sticky kept",
        "DisplayCaseEmptyNoContentMs",
        "订正 R16",
        "SkipBuiltinLoot",
        "仍在攒样本",
        "inventory calibration",
        "搜空判空",
    ]

    for title, marks in (("★ 本轮新增（R18）", marks_new), ("历史标记（不回归）", marks_old)):
        print(f"\n{title}")
        for s in marks:
            n = d.count(s.encode("utf-8"))
            flag = "" if n else "   <== 缺!"
            print(f"  [{n:2d}x] {s}{flag}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
