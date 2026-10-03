# -*- coding: utf-8 -*-
"""统计 Starfield.esm 里「生物 / 机器人内置武器」的 Non-Playable 标志（0x04）覆盖情况。

背景：crModelA_CombatRightGun_Auto (0x001DC415) 是战斗机器人的内置手臂炮，
      实测它没有 0x04（Non-Playable）标志 ⇒ MOD 判空把它算作「可拿物品」
      ⇒ 机器人搜刮拿空后仍被判「有东西」⇒ 高亮永不熄灭。
      本脚本核对：所有 cr*/Creature* 前缀武器里，标了 0x04 与没标 0x04 的分布。

用法: python esm_creature_weap_probe.py
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
    all_weaps: list[tuple[int, str, bool, bool, bool]] = []  # fid, edid, has_np04, has_modl, has_full
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
            if sig == b"WEAP":
                off = p + 24
                end = off + ds
                edid = ""
                has_modl = False
                has_full = False
                while off + 6 <= end:
                    s = d[off:off + 4]
                    sz = struct.unpack("<H", d[off + 4:off + 6])[0]
                    if s == b"EDID" and not edid:
                        edid = d[off + 6:off + 6 + sz].split(b"\x00")[0].decode("latin1", "replace")
                    elif s == b"MODL":
                        has_modl = True
                    elif s == b"FULL":
                        has_full = True
                    off += 6 + sz
                all_weaps.append((fid, edid, (fl & 0x04) != 0, has_modl, has_full))
            p += 24 + ds

    # 口径 1：cr* / Creature* 前缀
    cr = [w for w in all_weaps if w[1].lower().startswith("cr") or "creature" in w[1].lower()]
    print(f"cr*/Creature* WEAP 总数 = {len(cr)}；带 0x04 = {sum(1 for w in cr if w[2])}；不带 0x04 = {sum(1 for w in cr if not w[2])}")
    print()
    print("== 不带 0x04 的 cr* 武器（判空会算「可拿」）—— 看 has_MODL ==")
    for fid, edid, np04, modl, full in cr:
        if not np04:
            print(f"  0x{fid:08X}  MODL={'Y' if modl else 'N'} FULL={'Y' if full else 'N'}  {edid}")
    print()
    # 口径 2：全表 —— 无 MODL 的 WEAP 分布
    no_modl = [w for w in all_weaps if not w[3]]
    print(f"全表 WEAP = {len(all_weaps)}；无 MODL = {len(no_modl)}")
    print()
    print("== 全表所有「无 MODL」的 WEAP（前 80 条）==")
    for fid, edid, np04, modl, full in no_modl[:80]:
        print(f"  0x{fid:08X}  0x04={'Y' if np04 else 'N'} FULL={'Y' if full else 'N'}  {edid}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
