# -*- coding: utf-8 -*-
"""核对「部署 INI」与「git HEAD 里的模板」的差异（部署前例行核对，防覆盖用户自定义）。

用法（在项目根目录）: python tools/re/check_ini_diff.py
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(r"d:\workspace\starfield mod\always scan")
DEPLOYED = Path(r"D:\Mod Organizer 2\starfield_mods\mods\Starfield Always Scan (SFSE)\SAS_AlwaysScan.ini")


def norm(b: bytes) -> list[str]:
    return b.replace(b"\r\n", b"\n").replace(b"\r", b"\n").decode("utf-8").split("\n")


def main() -> int:
    old = subprocess.check_output(
        ["git", "show", "HEAD:resources/SAS_AlwaysScan.ini"], cwd=str(ROOT))
    deployed = DEPLOYED.read_bytes()
    a = norm(old)
    b = norm(deployed)
    print(f"old template lines = {len(a)}, deployed lines = {len(b)}")
    import difflib
    diff = list(difflib.unified_diff(a, b, "HEAD-template", "deployed", lineterm=""))
    if not diff:
        print(">>> 完全一致（无用户自定义）")
    else:
        print(f">>> 差异 {len([l for l in diff if l[:1] in '+-' and l[:3] not in ('+++','---')])} 行：")
        for line in diff[:80]:
            print("   ", line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
