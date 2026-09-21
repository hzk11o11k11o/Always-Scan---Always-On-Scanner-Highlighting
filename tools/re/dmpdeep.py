"""转储深查：内存区域状态（MemoryInfoList）/ 结构体字段 / 栈上返回地址候选。

dmpinfo.py 只回答「崩在哪」；本工具回答「**为什么**崩」需要的三件事：

  · 崩溃读地址在崩溃那一刻的**真实 region 状态**（MEM_FREE / MEM_MAPPED / 保护位）
    —— 这是区分「视图被 unmap」与「地址算错」的决定性证据；
  · 读某个模块内的结构体字段（例：commonlibsf 的 `REL::IDDB` 实例，看它的
    `m_mmap` / `m_v5` 到底是活的还是悬空的）；
  · 故障线程栈上**所有**落在指定模块内的返回地址（去重后给 pdbsym.py 批量符号化）。

用法：
    python dmpdeep.py <a.dmp> --mods                     # 模块表
    python dmpdeep.py <a.dmp> --addr 0x1D039945DB8 ...   # region 状态 + 转储内字节
    python dmpdeep.py <a.dmp> --field 0x7FFFD90A5910     # 结构体（8 字节步进 dump）
    python dmpdeep.py <a.dmp> --stack SAS_AlwaysScan     # 栈上该模块内的返回地址
"""
from __future__ import annotations

import struct
import sys

STREAM_THREAD_LIST = 3
STREAM_MODULE_LIST = 4
STREAM_MEMORY_LIST = 5
STREAM_EXCEPTION = 6
STREAM_MEMORY64_LIST = 9
STREAM_MEMORY_INFO_LIST = 16
STREAM_THREAD_NAMES = 24

STATE = {0x1000: "MEM_COMMIT", 0x2000: "MEM_RESERVE", 0x10000: "MEM_FREE"}
TYPE = {0x1000000: "MEM_IMAGE", 0x40000: "MEM_MAPPED", 0x20000: "MEM_PRIVATE"}
PROT = {
    0x01: "NOACCESS", 0x02: "R", 0x04: "RW", 0x08: "WRITECOPY",
    0x10: "X", 0x20: "XR", 0x40: "XRW", 0x80: "XWRITECOPY",
    0x100: "GUARD", 0x200: "NOCACHE", 0x400: "WRITECOMBINE",
}


def prot_str(p: int) -> str:
    base = PROT.get(p & 0xFF, f"0x{p & 0xFF:02X}")
    extra = "".join(f"|{PROT[b]}" for b in (0x100, 0x200, 0x400) if p & b)
    return base + extra


class Dump:
    def __init__(self, path: str):
        with open(path, "rb") as f:
            self.buf = f.read()
        self.path = path
        self.streams: dict[int, tuple[int, int]] = {}
        n, dir_rva = struct.unpack_from("<II", self.buf, 8)
        ts = struct.unpack_from("<I", self.buf, 20)[0]
        self.timestamp = ts
        for i in range(n):
            st, size, rva = struct.unpack_from("<III", self.buf, dir_rva + i * 12)
            self.streams[st] = (rva, size)

    # ---- 基础流 ----------------------------------------------------------
    def modules(self):
        rva, _ = self.streams[STREAM_MODULE_LIST]
        cnt = struct.unpack_from("<I", self.buf, rva)[0]
        out = []
        off = rva + 4
        for _ in range(cnt):
            base, size = struct.unpack_from("<QI", self.buf, off)
            name_rva = struct.unpack_from("<I", self.buf, off + 20)[0]
            n = struct.unpack_from("<I", self.buf, name_rva)[0]
            name = self.buf[name_rva + 4:name_rva + 4 + n].decode("utf-16-le", "replace")
            out.append((base, size, name))
            off += 108
        return out

    def exception(self):
        rva, _ = self.streams[STREAM_EXCEPTION]
        tid = struct.unpack_from("<I", self.buf, rva)[0]
        code, _fl, _rec, addr, nparam = struct.unpack_from("<IIQQI", self.buf, rva + 8)
        params = struct.unpack_from("<15Q", self.buf, rva + 8 + 32)[:nparam]
        ctx_size, ctx_rva = struct.unpack_from("<II", self.buf, rva + 8 + 152)
        return dict(tid=tid, code=code, addr=addr, params=params,
                    ctx_rva=ctx_rva, ctx_size=ctx_size)

    def threads(self):
        rva, _ = self.streams[STREAM_THREAD_LIST]
        cnt = struct.unpack_from("<I", self.buf, rva)[0]
        out = []
        off = rva + 4
        for _ in range(cnt):
            tid = struct.unpack_from("<I", self.buf, off)[0]
            stack_start = struct.unpack_from("<Q", self.buf, off + 24)[0]
            stack_size, stack_rva = struct.unpack_from("<II", self.buf, off + 32)
            ctx_size, ctx_rva = struct.unpack_from("<II", self.buf, off + 40)
            out.append(dict(tid=tid, stack_start=stack_start, stack_size=stack_size,
                            stack_rva=stack_rva, ctx_size=ctx_size, ctx_rva=ctx_rva))
            off += 48
        return out

    def regs(self):
        ex = self.exception()
        if ex["ctx_size"] < 0x120:
            return None
        off = ex["ctx_rva"]
        keys = {"rax": 0x78, "rcx": 0x80, "rdx": 0x88, "rbx": 0x90, "rsp": 0x98,
                "rbp": 0xA0, "rsi": 0xA8, "rdi": 0xB0, "r8": 0xB8, "r9": 0xC0,
                "r10": 0xC8, "r11": 0xD0, "r12": 0xD8, "r13": 0xE0, "r14": 0xE8,
                "r15": 0xF0, "rip": 0xF8}
        return {k: struct.unpack_from("<Q", self.buf, off + v)[0] for k, v in keys.items()}

    def thread_names(self) -> dict[int, str]:
        """ThreadNamesStream（24）：tid -> 线程名（游戏自己设的名字，能一眼看出主线程）。"""
        if STREAM_THREAD_NAMES not in self.streams:
            return {}
        rva, _ = self.streams[STREAM_THREAD_NAMES]
        cnt = struct.unpack_from("<I", self.buf, rva)[0]
        out: dict[int, str] = {}
        off = rva + 4
        for _ in range(cnt):
            tid, name_rva = struct.unpack_from("<QI", self.buf, off)
            n = struct.unpack_from("<I", self.buf, name_rva)[0]
            raw = self.buf[name_rva + 4:name_rva + 4 + n]
            out[tid] = raw.decode("utf-16-le", "replace")
            off += 12
        return out

    # ---- 内存 ------------------------------------------------------------
    def mem_ranges(self):
        """转储里**真的带了字节**的区间 [(start, size, file_off)]。"""
        out = []
        if STREAM_MEMORY64_LIST in self.streams:
            rva, _ = self.streams[STREAM_MEMORY64_LIST]
            n = struct.unpack_from("<Q", self.buf, rva)[0]
            base_off = struct.unpack_from("<Q", self.buf, rva + 8)[0]
            off = rva + 16
            for _ in range(n):
                start, size = struct.unpack_from("<QQ", self.buf, off)
                out.append((start, size, base_off))
                base_off += size
                off += 16
        elif STREAM_MEMORY_LIST in self.streams:
            rva, _ = self.streams[STREAM_MEMORY_LIST]
            n = struct.unpack_from("<I", self.buf, rva)[0]
            for i in range(n):
                o = rva + 4 + i * 16
                start, size, off = struct.unpack_from("<QII", self.buf, o)
                out.append((start, size, off))
        return out

    def mem_info(self):
        """崩溃那一刻的 VAD 快照 [(base, size, alloc_base, alloc_prot, state, prot, type)]。"""
        if STREAM_MEMORY_INFO_LIST not in self.streams:
            return []
        rva, _ = self.streams[STREAM_MEMORY_INFO_LIST]
        hdr, entry, cnt = struct.unpack_from("<III", self.buf, rva)
        out = []
        off = rva + hdr
        for _ in range(cnt):
            (base, alloc_base, alloc_prot, _al1, size, state, prot, mtype,
             _al2) = struct.unpack_from("<QQIIQIIII", self.buf, off)
            out.append((base, size, alloc_base, alloc_prot, state, prot, mtype))
            off += entry
        return out

    def region(self, addr: int):
        for r in self.mem_info():
            if r[0] <= addr < r[0] + r[1]:
                return r
        return None

    def read(self, addr: int, n: int):
        for start, size, off in self.mem_ranges():
            if start <= addr and addr + n <= start + size:
                fo = off + (addr - start)
                return self.buf[fo:fo + n]
        return None

    def read_partial(self, addr: int, n: int):
        """能读多少读多少：区间内整段拿、区间外的空洞补 0（用于栈扫描 / struct dump）。"""
        regs = sorted(self.mem_ranges(), key=lambda r: r[0])
        out = bytearray()
        cur = addr
        end = addr + n
        while cur < end:
            r = next((r for r in regs if r[0] <= cur < r[0] + r[1]), None)
            if r is None:
                nxt = next((r for r in regs if r[0] > cur), None)
                stop = min(nxt[0] if nxt else end, end)
                out += b"\x00" * (stop - cur)
                cur = stop
                continue
            start, size, off = r
            take = min(end, start + size) - cur
            fo = off + (cur - start)
            out += self.buf[fo:fo + take]
            cur += take
        return bytes(out)


def owner(addr: int, mods):
    for base, size, name in mods:
        if base <= addr < base + size:
            return name.split("\\")[-1], base
    return None, None


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    d = Dump(args[0])
    mods = d.modules()
    ex = d.exception()
    regs = d.regs()
    print(f"文件：{d.path}")
    print(f"  流：{sorted(d.streams)}  模块 {len(mods)} 个  内存区间 {len(d.mem_ranges())} 段"
          f"  VAD 条目 {len(d.mem_info())}")
    if ex:
        n, b = owner(ex["addr"], mods)
        print(f"  异常：tid={ex['tid']:X} code=0x{ex['code']:08X} addr=0x{ex['addr']:X}"
              f" [{n or '?'}{f' +0x{ex['addr']-b:X}' if b else ''}]"
              f" params=[{', '.join('0x%X' % p for p in ex['params'])}]")

    # 逐项解析：--xxx 后面跟 0..n 个「参数」（遇下一个 -- 结束），
    # 这样 --field 0xAAA --stack X 能同时生效（v1 的迭代器写法会把 --stack 吃掉）。
    argv = args[1:]
    i = 0
    while i < len(argv):
        a = argv[i]
        i += 1
        vals = []
        while i < len(argv) and not argv[i].startswith("--"):
            vals.append(argv[i])
            i += 1

        if a == "--mods":
            for base, size, name in sorted(mods, key=lambda m: m[0]):
                print(f"  {base:016X} {size:>9X}  {name}")
        elif a == "--threads":
            tn = d.thread_names()
            print(f"\n线程（RSP / RIP / 栈捕获区间）：")
            for t in d.threads():
                raw = d.buf[t["ctx_rva"]:t["ctx_rva"] + 0x100]
                if len(raw) < 0x100:
                    continue
                rsp = struct.unpack_from("<Q", raw, 0x98)[0]
                rip = struct.unpack_from("<Q", raw, 0xF8)[0]
                n, b = owner(rip, mods)
                mark = " ★崩溃线程" if ex and t["tid"] == ex["tid"] else ""
                print(f"  tid={t['tid']:<7} {tn.get(t['tid'], ''):<12} rsp={rsp:018X}"
                      f"  rip={rip:018X} [{n or '?'}{f' +0x{rip-b:X}' if b else ''}]"
                      f"  stack={t['stack_start']:X}+{t['stack_size']:X}"
                      f"  rspInStack={'Y' if t['stack_start'] <= rsp <= t['stack_start'] + t['stack_size'] else 'N'}{mark}")
        elif a == "--tn":
            tn = d.thread_names()
            print(f"\n线程名（{len(tn)} 条）：")
            for t in d.threads():
                tid = t["tid"]
                mark = " ★崩溃线程" if ex and tid == ex["tid"] else ""
                rip = ""
                print(f"  tid={tid:<7} {tn.get(tid, '<无名>')}{mark}  stack=0x{t['stack_start']:X}"
                      f"+0x{t['stack_size']:X}{rip}")
        elif a == "--addr":
            for tok in vals:
                addr = int(tok, 0)
                r = d.region(addr)
                if r:
                    base, size, alloc_base, alloc_prot, state, prot, mtype = r
                    print(f"\n地址 0x{addr:016X}:")
                    print(f"  region  0x{base:016X} .. 0x{base+size:016X}  size=0x{size:X}")
                    print(f"  alloc   0x{alloc_base:016X}  allocProt={prot_str(alloc_prot)}")
                    print(f"  state   {STATE.get(state, hex(state))}  protect={prot_str(prot)}"
                          f"  type={TYPE.get(mtype, hex(mtype))}")
                    blk = d.read(addr & ~0xFFF, 0x100)
                    print(f"  dump    {'含该页（前 0x40 字节如下）' if blk else '**不含该页字节**'}")
                    if blk:
                        for i in range(0, 0x40, 16):
                            print("     " + " ".join(f"{x:02X}" for x in blk[i:i + 16]))
                else:
                    print(f"\n地址 0x{addr:016X}: **不在任何 region 里（MEM_FREE）**")
        elif a == "--field":
            for tok in vals:
                addr = int(tok, 0)
                n, b = owner(addr, mods)
                print(f"\n结构体 dump 0x{addr:016X}"
                      f"{f'  [{n} +0x{addr-b:X}]' if b else ''}:")
                blob = d.read_partial(addr, 0xA0)
                if not blob:
                    print("  **该地址的字节不在转储里**")
                for i in range(0, len(blob), 8):
                    q = struct.unpack_from("<Q", blob, i)[0]
                    note = ""
                    on, ob = owner(q, mods)
                    if on:
                        note = f"  -> {on}+0x{q-ob:X}"
                    print(f"  +0x{i:03X}  0x{q:016X}{note}")
                for i in range(0, len(blob) - 4, 4):
                    v = struct.unpack_from("<I", blob, i)[0]
                    if 0 < v < 0x10000000:
                        print(f"    (+0x{i:03X} u32 = {v}  0x{v:X})")
        elif a == "--rsp":
            # 直接看「故障寄存器 RSP 附近」的栈（有些转储的 thread stack 描述符
            # 与真正的活动栈不是同一段，dmpinfo 的整段扫描会扫到无关区域）。
            span = int(vals[0], 0) if vals else 0x2000
            mod = vals[1] if len(vals) > 1 else "."
            rsp = regs["rsp"] if regs else 0
            print(f"\n从 RSP=0x{rsp:016X} 起 {span:#x} 字节内、落在「{mod}」内的值：")
            blob = d.read_partial(rsp, span)
            for i in range(0, len(blob) - 8, 8):
                v = struct.unpack_from("<Q", blob, i)[0]
                n, b = owner(v, mods)
                if n and (mod.lower() in n.lower()):
                    print(f"  RSP+0x{i:05X}  {v:016X}  {n}+0x{v - b:X}")
        elif a == "--stack":
            mod = vals[0] if vals else ""
            th = next((t for t in d.threads() if ex and t["tid"] == ex["tid"]), None)
            if not th:
                print("  找不到故障线程")
                continue
            blob = d.read(th["stack_start"], th["stack_size"])
            if blob is None:
                print("  栈字节不在转储里")
                continue
            print(f"\n栈（base 0x{th['stack_start']:X} size 0x{th['stack_size']:X}）里落在"
                  f"「{mod}」内的值（按栈地址从低到高）：")
            rsp = regs["rsp"] if regs else th["stack_start"]
            seen: dict[int, int] = {}
            for i in range(0, len(blob) - 8, 8):
                v = struct.unpack_from("<Q", blob, i)[0]
                n, b = owner(v, mods)
                if n and (mod.lower() in n.lower()):
                    addr = th["stack_start"] + i
                    seen[v - b] = seen.get(v - b, 0) + 1
                    print(f"  0x{addr:016X} {'>RSP+' if addr > rsp else '<RSP-'}{abs(addr-rsp):#0x}"
                          f"  {n}+0x{v-b:X}")
            print("  —— 去重后的候选返回地址（RVA，交给 pdbsym.py）：")
            for rva, cnt in sorted(seen.items(), key=lambda kv: -kv[1]):
                print(f"    {rva:#07X}  x{cnt}")
        else:
            print(f"  (未知参数 {a})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
