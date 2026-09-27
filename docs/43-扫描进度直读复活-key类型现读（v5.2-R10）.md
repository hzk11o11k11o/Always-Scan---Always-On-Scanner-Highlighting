# 43 · 扫描进度直读「一次都没跑起来」—— key 类型 word 现读（v5.2 / 订正 R10）

> 用户报告（原文，2026-09-27）：
>
> ```
> 再次出现已扫描物件还是扫描前青色的现象，而且这次开关扫描仪都没自我修复
> ```
>
> 结论：**R9 的新主判据（ⓠ 引擎扫描进度直读）自始至终一次都没执行** ——
> 启动时把「key 类型 word」读成了 0，代码把 0 当异常，于是整条判据被禁用
> （统计行 `ready=0 / 问=0`）。植物只剩「举着扫描仪时才成立」的判据 ⇒
> 放下扫描仪就回到青色；而 R9 又按用户指令关掉了自建记忆 ⇒「开关扫描仪也不自愈」。

---

## 一、证据（用户 2026-09-27 12:57~12:59 那一局，部署目录日志）

| 时间 | 日志 | 含义 |
| --- | --- | --- |
| 12:57:06 | `flora progress: key 类型常量异常（0）-> 引擎扫描进度直读 DISABLED` | ★ 新判据在启动时就被关掉 |
| 12:57:06 | `flora progress ready:` **不出现** | 对应的「已就绪」行缺失 |
| 12:58:23 | `… 引擎进度直读(★v5.2): 问=0 满=0 未满=0 失败=0 … ready=0` | 整局 **0 次查询** |
| 12:58:24 | `flora scan: base=0x18547F … | 引擎状态=1 | … -> 已扫描=0 ⇒ 状态 7（青）` | 判据落到 `GetOutlineState`（对植物几乎恒答未扫描） |
| 12:59:41 | `planet targets (窗口内): 未扫描=56 已扫描=32` + `引擎状态: 问=650 已扫描=67 未扫描=489 未知=94` | 绿的少数来自「举扫描仪时引擎画过 4/5」那一档 |

★ 用户「开关扫描仪也不自愈」的机制：`FloraUseMemory=0`（R9 默认）后，
「引擎画过绿」不再被记住；引擎**放下扫描仪就清空状态表**（`条目=0`）⇒
举扫描仪时短暂变绿、放下又青。

## 二、根因（离线反汇编 + PE 节表，全部实证）

引擎构造主表 key 的那条指令（`0x130A270`）：

```
0130A2BA  440fb705 d27aed04   movzx r8d, word ptr [rip + 0x4ed7ad2]
0130A2C2  49c1e020             shl r8, 0x20
0130A2C6  8b11                 mov edx, dword ptr [rcx]      ; key1
0130A2C8  4c0bc2               or r8, rdx
0130A2CB  49c1e010             shl r8, 0x10                  ; key = (word<<48)|(key1<<16)
```

* word 的地址 = `0x130A2C2 + 0x4ED7AD2` = **`0x61E1D94`**（`tools/re/disasm.py` 核对）。
* PE 节表：`.data` vaddr `0x5862000` / **rawsize `0x4FE800`** —— `0x61E1D94` 距节首
  `0x97FD94`，**超出 raw size** ⇒ **文件里没有这段数据**，是**运行时才被写入**的
  （BSS 语义）。⇒ **启动时读它必然是 0**。
* `findrefs.py 0x61E1D94`：49 处引用**全是读**（无 RIP 相对写）；其中两处 `lea` 把它的
  地址交给 `0x24058B0`（读 `word [rdi]` 当索引用的初始化 / 注册函数）—— 即这是一个
  **启动流程里才被填好的全局对象**（同族对象由 `0x231DA10` 那个初始化函数批量注册）。
* 引擎每次查询都**现读**这条指令 ⇒ 我们也必须**现读**，才能与引擎写入时用的 key 一致。

★ 顺带订正一个 R9 的推理偏差：文档里把它叫「类型**常量**」，实际上它是
「运行时初始化的全局 word」——「常量」这个词诱导了「启动时读一次就够」的写法。

## 三、改动（`plugin/src/AlwaysScan.cpp` + `main.cpp`）

| # | 位置 | 改动 |
| --- | --- | --- |
| ① | 常量段 | 注释改写：这里只解析**地址**；说明该 word 在 `.data` 未初始化段 |
| ② | 全局变量 | `g_floraKeyType`（启动读的值）→ `g_floraKeyTypeWord`（**地址**）+ `g_floraKeyTypeAtLoad` / `g_floraKeyTypeLast`（诊断） |
| ③ | `ResolveFloraProgressFunctions` | 只做签名 + **地址**可读性校验；**删掉**「读到 0 就 DISABLED」那条；就绪行改为 `keyTypeAddr=+0x61E1D94 keyType@load=0x…` |
| ④ | `QueryFloraScanProgressDirect` | **每次查询现读** word（`SafeReadMem(g_floraKeyTypeWord, …)`），key = `(word<<48)\|(key1<<16)`；新增 `stage`（停在哪一层，12 档枚举 + `FloraProgStageName`）与 `keyType/keyTypeRead`（诊断） |
| ⑤ | `QueryFloraScanProgressCached` | 前 8 条查询打 `flora progress probe:`（key1 / key2 / **现读 keyType** / percent / stage / 结论）；统计 `floraProgKeyZero` / `floraProgProbes` |
| ⑥ | 统计行 | `引擎进度直读(★v5.2/订正R10): 问= 满= 未满= 失败= 缓存命中= 冲突= keyType=0x…(现读) keyZero= ready= 记忆层=` |
| ⑦ | `main.cpp` 启动行 | 改为 `订正 R10`（含完整根因说明） |

★ 另外复核（本轮离线做，确认链路本身没问题）：`0x24105D0` 把结果写在 **out**（rdx）
并**返回 out**（`mov rax, rbx` ⇒ `rax == rdx`）⇒ 从 out 读 `+0x10/+0x18` 与引擎从
`[rax+0x10]` 读**完全等价**；`0x7BCBD0` 的主路径就是「`GetComponent(容器,0x81)` →
`[组件+0x28]` 非 0」⇒ 与我们的实现一致（它的兜底路径含写表，继续不碰）。

## 四、构建 / 部署 / 打包

* `xmake build SAS_AlwaysScan` 增量 **8.672 s**（零告警）；DLL **837 120 B**、
  SHA256 **`39675E89EEAE1F40603E3D1A06FF2F6288153FEF4B6CAA659FF451BA7CCAB321`**。
* 二进制核对：`订正 R10` ×3、`flora progress probe` ×2、`keyTypeAddr` ×1、`keyZero` ×1、
  `现读` ×6、`（未读到）` ×1、`链路走通` / `主表里没有这个物种` / `桶内偏移` /
  `SEH 捕获到异常` 各 ×1、`引擎进度直读` ×1。
* 流水线 `& tools\build-sas.ps1 -SkipPluginBuild -SkipDllBuild -SkipPapyrusCompile`（0.2 s）；
  MO2 `modid=18268` / **`version=2.0`**、**部署 DLL = 构建 DLL（逐字节一致）**；
  部署 INI 未改（`config ini kept` —— 本轮无新 INI 键）。
* 发布包：`dist\StarfieldAlwaysScan-2.0.zip`（覆盖旧包），README / nexus-description 已同步
  第 (12) / ⑪ 条（修复计数 11 → **twelve fixes**）。

## 五、待实测验收点（下一局游戏）

1. 启动日志：`loading（订正 R10：…）` + `config: floraEngineProgress=1 … ★ **订正 R10：key 的
   类型 word 每次查询现读** …` + `flora progress ready: … keyTypeAddr=+0x61E1D94
   keyType@load=0x0 (sig verified …)`；★ **不应再出现** `key 类型常量异常（0）-> DISABLED`。
2. ★★ 本会话前几条 `flora progress probe:` —— 看 `keyType=0x…(现读)` 与 `stage=`：
   * `stage=0` + `percent=100` ⇒ **修复生效**（`flora scan:` 行会出现
     `判据 = 引擎扫描进度直读（★ v5.2）：percent=100/100`，目标**不举扫描仪就是绿的**）；
   * `stage=7`（主表没有这个物种）⇒ 该物种在本存档里**确实**没扫过（青色是对的）；
   * `stage=4/5/6`（没有 0x81 组件 / key1==0 / 单例读不到）⇒ 把整行发我，按 stage 定位；
   * `keyZero=` 一直涨而 `满=` 不涨 ⇒ 类型 word 仍未初始化（把它发我）。
3. ★★ 以前扫过的植物（不举扫描仪）**直接是绿的**，且**开关扫描仪不再影响**它；
   统计行 `引擎进度直读: 问=/满=` 持续增长、`ready=1`。
4. ★ 没扫过的仍青（`未满` / `主表没有`）；扫一个新实例 ⇒ 5 秒内同 species 全绿（按 base 缓存）。
5. 不回归：F8 / 自愈 / 展示柜 / 换场景 / 配色 / `引擎状态表: 条目=` 稳定 / 动态场景不卡 /
   **不再出现「所有星球资源全绿」**（记忆层仍然关着）。

## 六、经验

1. **「常量」两个字是陷阱**：反汇编里 `movzx …, word [rip+disp]` 读的可能是一个
   **运行时才初始化**的全局（BSS）—— 判定「能不能启动时读」要看**它落在哪个节、
   是否在 raw data 之外**，而不是看它长得像不像常量。
2. **启动期一次性读取 = 把「时序」当「常量」**：凡是要复刻引擎「同一时刻取值」的表达式，
   就应当和引擎一样**在使用点现读**（成本是 1 次 2 字节读）。
3. **「禁用整条判据」是危险动作**：R9 的「读到 0 就当异常」把一个可能合法的值（0）
   升级成了「整条判据不可用」，而它的失败方向恰好是**用户最敏感的那一侧**（假青）。
   正确姿势：**值可疑就照用 + 打诊断**，而不是关掉整条链。
4. **诊断要能定位到「哪一层」**：新增的 `stage` 枚举 + 前 8 条 probe 日志，让「链路不通」
   在一局日志里就能指到具体那一步（这次 R9 的 `问=0` 就是最好的反面教材 ——
   只知道「没跑」，不知道「为什么没跑」）。
5. **复刻函数要先核对「返回值 vs out 参数」**：`0x24105D0` 返回的就是 out 指针
   （`mov rax, rbx`），从 out 读与从返回值读等价 —— 这类细节必须看反汇编，
   不能凭「名字像」猜。
