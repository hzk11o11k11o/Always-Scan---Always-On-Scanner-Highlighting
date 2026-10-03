# -*- coding: utf-8 -*-
"""核对新构建 DLL 里的订正 R16 标记（UTF-8 字符串 + FormID 数值）。"""
from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path

DLL = Path(r"d:\workspace\starfield mod\always scan\plugin\build\windows\x64\releasedbg\SAS_AlwaysScan.dll")


def main() -> int:
    d = DLL.read_bytes()
    print("DLL =", DLL)
    print("size =", len(d), "B")
    print("sha256 =", hashlib.sha256(d).hexdigest().upper())
    marks = [
        "订正 R16",
        "SkipBuiltinLoot",
        "生物内置武器",
        "无模型虚拟 WEAP",
        "crModelA",
        "loot: 跳过",
        "判空跳过: np={} eq={} bi={}",
        "builtin={} biTotal={}",
        "skipBi={}",
        "为什么是「偶发」",
    ]
    for s in marks:
        print(f"  [{d.count(s.encode('utf-8')):2d}x] {s}")
    # 14 个内置武器 FormID 至少应有一个（0x001DC415）以 u32 形式出现
    print("  0x001DC415 u32 出现 =", d.count(struct.pack("<I", 0x001DC415)))
    print("  0x003CB56D u32 出现 =", d.count(struct.pack("<I", 0x003CB56D)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
