#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SAS_AlwaysScan.log 计时报表 —— 把每个统计窗口的 scan/timing/timing2 相关行拼成一张表。

用途：排查「帧数下降 / 卡顿」时，一眼看出耗时是否与「遍历量（refs）」相关。

用法：
    python tools/sas-timing-report.py [日志路径] [--csv out.csv]

默认日志路径 = MO2 部署目录下的 SAS_AlwaysScan.log。
"""
import re
import sys
import os
from datetime import datetime

DEFAULT_LOG = r"D:\Mod Organizer 2\starfield_mods\mods\Starfield Always Scan (SFSE)\SAS_AlwaysScan.log"

RE_T = re.compile(r"^\[(\d\d:\d\d:\d\d)\.(\d+)\]")
RE_SCAN = re.compile(
    r"scan#(\d+) cell=([0-9A-F]+) off=0x([0-9A-F]+) refs=(\d+) cells=(\d+) cand=(\d+) sel=(\d+) "
    r"outline=(\d+)/(\d+)/(\d+) on=(\d+) resync=(\d+) monocle=(\d+) load=(\d+) notify=(\d+) move=([\d.]+)m"
)
RE_TIMING = re.compile(
    r"timing: scan avg=(\d+)ms max=(\d+)ms (?:完成=(\d+)/(\d+) 轮 )?ops=(\d+) deferred=(\d+) loading=(\d+)"
)
RE_T2 = re.compile(
    r"timing2: shape avg=(\d+)ms max=(\d+)ms vq=(\d+)/scan \| loop avg=(\d+)ms max=(\d+)ms "
    r"\(refs/scan: cur=(\d+) ring=(\d+) 分片推迟=(\d+) \| walk=(\d+)ms loot=(\d+)ms flora=(\d+)ms\) \| "
    r"sync avg=(\d+)ms max=(\d+)ms \(unh avg=(\d+)ms add avg=(\d+)ms 3D=(\d+)ms\)"
)
# ★ 订正 R4 新增：timing3 —— µs 细分 / 调用次数 / 卡顿 / 最慢一轮（墙钟 vs 线程 CPU）
RE_T3 = re.compile(
    r"timing3: 完成=(\d+)轮 平均遍历=(\d+)个引用/轮 \| 细分\(µs/轮\): walk=(\d+) classify=(\d+) loot=(\d+) "
    r"probe=(\d+) flora=(\d+) 其它=(\d+) \| 调用/轮: classify=(\d+) loot=(\d+)\(缓存命中=(\d+)\) flora=(\d+) "
    r"probe=(\d+) vq=(\d+) 内核读=(\d+) 直读异常=(\d+) \| 卡顿: 次数=(\d+) 合计=(\d+)µs 最大=(\d+)µs \| "
    r"最慢一轮: 总=(\d+)µs cpu=(\d+)µs 引用=(\d+) walk=(\d+)µs 卡顿=(\d+)µs/(\d+)次"
)
RE_CAND = re.compile(r"candTypes .*?: (.*)$")
RE_REJ = re.compile(r"rejTypes .*?: (.*)$")


def ft_map(s):
    """'ft=46x6, ft=40x85' -> {46: 6, 40: 85}"""
    out = {}
    if not s:
        return out
    for m in re.finditer(r"ft=(\d+)x(\d+)", s):
        out[int(m.group(1))] = int(m.group(2))
    return out


def main():
    log = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else DEFAULT_LOG
    if not os.path.exists(log):
        print("日志不存在：" + log)
        return 1

    rows = []
    cur = {}
    with open(log, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\n")
            mt = RE_T.match(line)
            if mt:
                cur["t"] = mt.group(1)
            if "scan#" in line and (m := RE_SCAN.search(line)):
                cur.update(
                    scan=int(m.group(1)), refs=int(m.group(4)), cells=int(m.group(5)),
                    cand=int(m.group(6)), sel=int(m.group(7)), outline=int(m.group(8)),
                    monocle=int(m.group(12)), move=float(m.group(16)),
                )
            elif "timing: scan" in line and (m := RE_TIMING.search(line)):
                cur.update(scanAvg=int(m.group(1)), scanMax=int(m.group(2)))
                if m.group(3):
                    cur.update(full=int(m.group(3)), calls=int(m.group(4)))
            elif "timing2:" in line and (m := RE_T2.search(line)):
                cur.update(
                    shapeAvg=int(m.group(1)), shapeMax=int(m.group(2)), vq=int(m.group(3)),
                    loopAvg=int(m.group(4)), loopMax=int(m.group(5)),
                    refsCur=int(m.group(6)), refsRing=int(m.group(7)), deferred=int(m.group(8)),
                    walk=int(m.group(9)), loot=int(m.group(10)), flora=int(m.group(11)),
                    syncAvg=int(m.group(12)), syncMax=int(m.group(13)),
                    unh=int(m.group(14)), add=int(m.group(15)), t3d=int(m.group(16)),
                )
            elif "timing3:" in line and (m := RE_T3.search(line)):
                # ★ 订正 R4：细分（µs/轮）+ 调用次数 + 卡顿 + 最慢一轮（墙钟 vs CPU）
                cur.update(
                    t3walk=int(m.group(3)), t3cls=int(m.group(4)), t3loot=int(m.group(5)),
                    t3probe=int(m.group(6)), t3flora=int(m.group(7)), t3other=int(m.group(8)),
                    cLoot=int(m.group(10)), cLootMemo=int(m.group(11)), cVq=int(m.group(14)),
                    kReads=int(m.group(15)), sehFaults=int(m.group(16)),
                    stalls=int(m.group(17)), stallUs=int(m.group(18)), stallMaxUs=int(m.group(19)),
                    wTotal=int(m.group(20)), wCpu=int(m.group(21)), wRefs=int(m.group(22)),
                    wWalk=int(m.group(23)), wStall=int(m.group(24)),
                )
            elif "candTypes" in line and (m := RE_CAND.search(line)):
                cur["candTypes"] = ft_map(m.group(1))
            elif "rejTypes" in line and (m := RE_REJ.search(line)):
                cur["rejTypes"] = ft_map(m.group(1))
                if "scanAvg" in cur:
                    rows.append(dict(cur))
                    cur = {k: v for k, v in cur.items() if k == "t"}

    hdr = (f"{'time':8} {'scanAvg':>7} {'scanMax':>7} {'shape':>5} {'loop':>5} {'loopMax':>7} "
           f"{'cur':>6} {'ring':>6} {'def':>6} {'walk':>5} {'loot':>5} {'flora':>5} "
           f"{'sync':>5} {'sMax':>5} {'3D':>4} {'vq':>4} {'refs':>6} {'cells':>5} "
           f"{'cand':>5} {'sel':>5} {'out':>5} {'florCand':>8} {'statRej':>7} {'mono':>4} {'move':>6}")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        cand_t = r.get("candTypes", {})
        rej_t = r.get("rejTypes", {})
        print(f"{r.get('t',''):8} {r.get('scanAvg',0):7} {r.get('scanMax',0):7} "
              f"{r.get('shapeAvg',0):5} {r.get('loopAvg',0):5} {r.get('loopMax',0):7} "
              f"{r.get('refsCur',0):6} {r.get('refsRing',0):6} {r.get('deferred',0):6} "
              f"{r.get('walk',0):5} {r.get('loot',0):5} {r.get('flora',0):5} "
              f"{r.get('syncAvg',0):5} {r.get('syncMax',0):5} {r.get('t3d',0):4} {r.get('vq',0):4} "
              f"{r.get('refs',0):6} {r.get('cells',0):5} {r.get('cand',0):5} {r.get('sel',0):5} "
              f"{r.get('outline',0):5} {cand_t.get(46,0):8} {rej_t.get(41,0):7} "
              f"{r.get('monocle',0):4} {r.get('move',0):6.1f}")
    print(f"\n共 {len(rows)} 个统计窗口。列说明：florCand=候选里的 FLOR(ft=46) 个数；statRej=被拒的 STAT(ft=41) 个数。")

    if any("t3walk" in r for r in rows):
        hdr2 = (f"{'time':8} {'walkUs':>7} {'clsUs':>6} {'lootUs':>6} {'probeUs':>7} {'floraUs':>7} "
                f"{'其他Us':>6} {'loot次':>6} {'缓存':>5} {'内核读':>6} {'异常':>5} "
                f"{'卡顿次':>6} {'卡顿Us':>7} {'卡顿Max':>7} | {'最慢总':>7} {'最慢cpu':>7} {'最慢引用':>8} {'最慢卡顿':>8}")
        print("\n【timing3 · 订正 R4】前 25 个窗口（µs/轮）：")
        print(hdr2)
        print("-" * len(hdr2))
        for r in rows:
            if "t3walk" not in r:
                continue
            print(f"{r.get('t',''):8} {r.get('t3walk',0):7} {r.get('t3cls',0):6} {r.get('t3loot',0):6} "
                  f"{r.get('t3probe',0):7} {r.get('t3flora',0):7} {r.get('t3other',0):6} "
                  f"{r.get('cLoot',0):6} {r.get('cLootMemo',0):5} {r.get('kReads',0):6} "
                  f"{r.get('sehFaults',0):5} {r.get('stalls',0):6} {r.get('stallUs',0):7} "
                  f"{r.get('stallMaxUs',0):7} | {r.get('wTotal',0):7} {r.get('wCpu',0):7} "
                  f"{r.get('wRefs',0):8} {r.get('wStall',0):8}")
        print("★ 判读：卡顿Us ≈ walkUs 且 最慢cpu ≪ 最慢总 ⇒ 时间被线程抢占 / 等内存，"
              "不是我们的指令；内核读 / vq / 异常 应为 0。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
