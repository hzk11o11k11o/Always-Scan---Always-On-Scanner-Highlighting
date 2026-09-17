#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""不启动 MO2 GUI，直接用 MO2 自带的 lootcli.exe 复现/验证「自动排序」。

MO2 2.5.x 的「排序」按钮实际是启动 <MO2>/loot/lootcli.exe 子进程，
本脚本用相同参数调用它，便于在命令行里定位崩溃原因。

用法（在仓库根目录执行）：
    python tools/loot_sort_check.py

退出码 0 表示排序成功，排序结果 JSON 写到 %TEMP%\\loot_sorted.json。
若出现 0xC0000409（-1073740791），说明 lootcli 自身崩溃，通常是
masterlist 下载失败：lootcli 用 WinHTTP 下载 masterlist，而 WinHTTP
不读取 Windows「系统代理」(WinINET)，需要用管理员执行
    netsh winhttp import proxy source=ie
把系统代理同步给 WinHTTP。
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile

MO2_DIR = r"D:\Mod Organizer 2"
GAME_DIR = r"D:\SteamLibrary\steamapps\common\Starfield"
PROFILE_DIR = r"D:\Mod Organizer 2\starfield_mods\profiles\Default"
LOOT_DATA_DIR = os.path.join(
    os.environ.get("LOCALAPPDATA", ""), "LOOT"
)


def main() -> int:
    lootcli = os.path.join(MO2_DIR, "loot", "lootcli.exe")
    if not os.path.isfile(lootcli):
        print(f"[x] 找不到 lootcli.exe: {lootcli}")
        return 2

    out_json = os.path.join(tempfile.gettempdir(), "loot_sorted.json")
    if os.path.exists(out_json):
        os.remove(out_json)

    env = os.environ.copy()
    # lootcli.exe 依赖 loot.dll / Qt6Core.dll，二者分别在 loot\ 与 dlls\ 下
    env["PATH"] = os.pathsep.join(
        [os.path.join(MO2_DIR, "loot"), os.path.join(MO2_DIR, "dlls"),
         MO2_DIR, env.get("PATH", "")]
    )

    # 参数顺序与名称由 lootcli.exe 的 CLI11 定义决定，均来自实测
    cmd = [
        lootcli,
        "--game", "Starfield",
        "--gamePath", GAME_DIR,
        "--pluginListPath", os.path.join(PROFILE_DIR, "plugins.txt"),
        "--gameLocalPath", PROFILE_DIR,
        "--lootDataPath", LOOT_DATA_DIR,
        "--out", out_json,
    ]
    print("[*] " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    proc = subprocess.run(cmd, env=env, cwd=os.path.join(MO2_DIR, "loot"))

    print(f"[*] lootcli 退出码: {proc.returncode} (0x{proc.returncode & 0xFFFFFFFF:08X})")
    if proc.returncode == 0:
        print(f"[+] 排序成功，结果: {out_json}")
        if os.path.isfile(out_json):
            with open(out_json, "r", encoding="utf-8", errors="replace") as fh:
                head = fh.read(400)
            print("[+] 结果预览: " + head)
        return 0

    if proc.returncode == 0xC0000409 - (1 << 32):
        print("[x] lootcli 崩溃(0xC0000409)，绝大多数情况是 masterlist 下载失败；")
        print("    请用管理员执行: netsh winhttp import proxy source=ie")
    return 1


if __name__ == "__main__":
    sys.exit(main())
