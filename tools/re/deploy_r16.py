# -*- coding: utf-8 -*-
"""订正 R16 部署：DLL / PDB / INI 到 MO2 mod 目录 + 逐字节校验。"""
from __future__ import annotations

import hashlib
import shutil
import sys
from pathlib import Path

SRC = Path(r"d:\workspace\starfield mod\always scan")
MOD = Path(r"D:\Mod Organizer 2\starfield_mods\mods\Starfield Always Scan (SFSE)")


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest().upper()


def main() -> int:
    # 1) 备份部署 INI（旧模板 v52r15 口径 + LogMaxMB=10）
    dst_ini = MOD / "SAS_AlwaysScan.ini"
    bak = MOD / "SAS_AlwaysScan.ini.bak-v52r16"
    shutil.copy2(dst_ini, bak)
    print("backup  ->", bak.name, len(bak.read_bytes()), "B")

    # 2) 新模板 -> 部署 INI（LogMaxMB 0 -> 10，本机开发口径）
    raw = (SRC / "resources" / "SAS_AlwaysScan.ini").read_bytes()
    assert b"\nLogMaxMB=0\n" in raw, "模板里找不到 `LogMaxMB=0` 行"
    new = raw.replace(b"\nLogMaxMB=0\n", b"\nLogMaxMB=10\n")
    dst_ini.write_bytes(new)
    print("deploy ini ->", len(new), "B", sha(dst_ini))

    # 3) DLL + PDB
    dll_src = SRC / "plugin" / "build" / "windows" / "x64" / "releasedbg" / "SAS_AlwaysScan.dll"
    pdb_src = dll_src.with_suffix(".pdb")
    plug = MOD / "SFSE" / "Plugins"
    dll_dst = plug / "SAS_AlwaysScan.dll"
    shutil.copy2(dll_src, dll_dst)
    if pdb_src.exists():
        shutil.copy2(pdb_src, plug / "SAS_AlwaysScan.pdb")
        print("pdb copied ->", pdb_src.name, pdb_src.stat().st_size, "B")
    print("deploy dll ->", dll_dst.stat().st_size, "B", sha(dll_dst))

    # 4) 逐字节校验（部署 = 构建）
    same = dll_dst.read_bytes() == dll_src.read_bytes()
    print("build  dll =", dll_src.stat().st_size, "B", sha(dll_src))
    print("deploy == build :", same)
    if not same:
        print("!!! 部署 DLL 与构建 DLL 不一致")
        return 1
    # 5) 部署 INI 关键键核对
    txt = dst_ini.read_bytes()
    for key in (b"SkipBuiltinLoot=1", b"LogMaxMB=10", b"SkipEquippedLoot=1", b"SkipNonPlayableLoot=1"):
        print(f"  [{'OK' if key in txt else 'MISSING'}] {key.decode()}")
    print("DONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
