# 36 · 「帧数仍有下降」—— 环内 cell 引用遍历分片（DLL v5.1.0 订正 R3 / 公开版仍 2.0）

> 建立：2026-09-27。
> 触发：用户报告（原文）——
>
> ```
> 帧数还是有下降现象，检查一下
> ```
>
> 承接 `docs/33`（v5.1：把「读引擎状态表」改成只读，修「越玩越卡」）。
> **一句话结论**：上一轮修掉的是「引擎表随游玩**持续增长**」；本次查到的是**另一件事** ——
> 每轮扫描要**全量遍历**「当前 cell + 环内 cell」的引用（环合计上限 **60000** 条），
> 用户当轮日志里 `loop avg=55ms`（每 200ms 一轮 ⇒ 主线程占用 ~28%）几乎全部来自这里。
> 治法 = **环内 cell 分片遍历**（每轮只扫一片、游标轮转），单轮环内遍历量降到约 1/5。

---

## 一、证据链（用户 2026-09-27 07:44~07:54 那一局日志）

### 1.1 形态：scan 耗时**随场景**从 3ms 涨到 79ms（不是随游玩时间增长）

```
[07:47:06]   timing: scan avg=3ms  max=3ms     ← 刚进世界，cells=1
[07:50:43]   timing: scan avg=58ms max=85ms    ← 换场景之后（cells=4）
[07:54:26]   timing: scan avg=79ms max=115ms   ← 稳定在这个量级
```

同一条统计行的拆解（同窗口）：

```
  timing2: shape avg=0ms max=2ms vq=21/scan | loop avg=55ms max=76ms | sync avg=14ms max=29ms (unh avg=0ms add avg=14ms)
```

* `shape` ≈ 0ms —— VirtualQuery 缓存（v2.3）在正常工作，形状校验不是问题；
* `loop` = **55ms**（大头）—— 这一段里只有「遍历引用 + 分类 + 距离」，**纯内存读、零引擎调用**；
* `sync` = 14ms —— 其中 `unh`（摘除）≈ 0、`add` ≈ 14ms（挂载 + 3D 复检的引擎调用）。

### 1.2 数字对账：60000 × ~0.9µs ≈ 54ms

* 环的引用数上限是常量 **`kRingRefsCap = 60000`**（`RingTrimByRefs`：环内各 cell 的
  引用数合计超过它就从最旧的开始丢）—— 也就是说**每轮最多可能遍历 6 万条引用**。
* `scan#…` 行里 `refs=730 cells=4` —— 只展示了**当前 cell** 的引用数，
  **环内 3 个 cell 的引用数没有出现在任何日志里**（这是本次诊断的第一个盲点）。
* 单条引用的遍历成本 ≈ 读指针 + `IsDeleted/IsDisabled/IsPlayerRef`（几个成员）+ `parentCell`
  比较 + `data.location` 取三个 float + 距离平方 —— 分散在对象各处，
  **cache miss 主导**，实测量级 ≈ 0.5~1µs/条。6 万 × 0.9µs ≈ **54ms**，与 `loop avg=55ms` 吻合。
* 场景相关性也吻合：`cells` 从 1 → 4（进了外景 / 大场景，环里装进大 cell），
  `cand` 从 25 → 192，loop 从 ~10ms → 55ms。

### 1.3 为什么不是「上一轮没修干净」

* `docs/33` 修的是「状态表被 LookupOrAdd **插入**条目、引用计数持续增长」——
  当轮日志 `引擎状态表: 条目=0`（放下扫描仪后为 0、不再单调增长）**证明那条已经修好**；
* 本次的 loop 开销与「游玩时长」无关、与**场景规模**强相关（同一局里 3ms ↔ 79ms 来回），
  是完全不同的两条通路。

### 1.4 「每轮全扫」原本是有意为之

环（v4.7）的语义 = 「玩家最近待过的 cell」：
① 判断「换 cell 是不是连续过渡」；② 让**边界对面**的引用也参与扫描
（否则站在边界上时，隔壁 cell 的东西时亮时灭）。
为了让②即时，环内 cell 原来是**每轮全量遍历**的 —— 单 cell / 小场景没问题（几百条），
**外景 / 城市的大 cell 就把单轮成本顶到 50ms 量级**。

---

## 二、改动表（`plugin/src/AlwaysScan.cpp` / `main.cpp` / INI 模板）

| # | 位置 | 改动 |
| --- | --- | --- |
| ① | 常量区（`kRingRefsCap` 下方） | 新增 **`kRingSliceMaxRounds = 5`**（默认片数）与 **`kRingSliceMinRefs = 2000`**（小 cell 不分片的阈值）+ 完整背景注释（见 §1.2 的对账与 §2 的安全钳制） |
| ② | `Config` | 新增 **`ringSliceMaxRounds`**（用户值，INI `RingSliceMaxRounds`）与 **`ringSliceRoundsEff`**（**钳制后的有效片数**，Rescan 只认它） |
| ③ | `LoadConfig` | 读取 + **安全钳制**：`eff = min(用户值, (UnhighlightGraceMs - ScanIntervalMs) / ScanIntervalMs)`，下限 1（1 / 0 = 不分片）；含义 = **分片周期（eff × 扫描间隔）必须 < 离开宽限期**，否则「本轮没扫到的目标」会被摘掉再挂回 = 闪烁 |
| ④ | `State::RingCell` | 新增 **`sliceCursor`**（分片游标；`RingTouch` / 校验失败 / `RingClear` 都会把它归 0） |
| ⑤ | `Rescan` 的 `ScanCell` | 新增 **`begin`** 字段（片起点）；语义 = 遍历 `list[begin .. begin+count-1]`。当前 cell 恒 `begin=0`（**不分片**） |
| ⑥ | `Rescan` 环循环 | **分片计算**：`total > kRingSliceMinRefs` 时 `slice = ceil(total / eff)`，本轮取 `[cursor, cursor+slice)`，游标环形推进；诊断计数 `ringSliceDeferred += total - take` |
| ⑦ | `Rescan` 主循环 | 遍历改用 `begin`；**当前 cell 与环内分片分别计数**（`refsWalkCur` / `refsWalkRing`）；`CalibrateInventory(list + begin, count)` 同步订正 |
| ⑧ | 计时埋点 | 新增 **`tWalkMs`**（loop 整段）、**`tLootMs`**（`RefLootState`）、**`tFloraMs`**（`FloraTargetScanned` / `ProbeFloraEngineState`）、**`t3DMs`**（sync 的 3D 复检，原来是混在 tAdd 里的） |
| ⑨ | 统计行 `timing2` | 扩展为：`loop avg=… (refs/scan: cur=… ring=… 分片推迟=… | walk=…ms loot=…ms flora=…ms) | sync … (unh=… add=… 3D=…ms)` |
| ⑩ | 启动日志 | 新增 `config: ringSliceMaxRounds=… -> 环内 cell 分片…有效片数=…（…分片周期 ≈ …ms 必须 < UnhighlightGraceMs=…ms…）`（可直接核对钳制结果） |
| ⑪ | 启动行（`main.cpp`） | 构建指纹换成 **`（订正 R3：环内 cell 分片遍历 + loop/sync 细分计时）`**（版本号不变：DLL 仍 5.1.0、公开版仍 2.0） |
| ⑫ | INI 模板 / 发布文案 | `resources/SAS_AlwaysScan.ini` 新增 `RingSliceMaxRounds` 段（含亮度对比与回退说明）并订正 `timing2` 说明；`package/README.txt` 与 `package/nexus-description.md` 增写第 (5) 条修复 |

**行为变化（给用户的三句话）**：

1. **帧数**：外景 / 大场景下每轮扫描不再一次走完整个「邻域」——
   环内 cell 每轮只走一片，单轮环内遍历量 ≈ 环总量 / 5（默认）；
2. **观感不变**：玩家**所在** cell 永远全扫（视野里的目标即时响应）；
   小 cell（≤2000 引用）不分片；分片周期被自动钳在「离开宽限期」之内（默认 1.0s < 1.5s）
   ⇒ **不会出现「亮一下灭一下」的闪烁**；
3. **可回退**：INI `RingSliceMaxRounds=0`（或 1）= 每轮全扫，退回旧行为。

---

## 三、构建 / 部署

| 项 | 值 |
| --- | --- |
| 构建 | `xmake build SAS_AlwaysScan` 增量 **7.156 s**（零告警） |
| DLL | `SAS_AlwaysScan.dll`、**780 800 B**、SHA256 **`5212383046DB39B9611A4CDA99884DEC25C339DA114886192012B052EE4FDCCD`** |
| 二进制核对 | `v5.1.0` ×1 / `v5.0.0` ×0、`订正 R3` ×1（启动行）、`订正 R2` ×1（config 行保留）、`按物种扩散` ×3、`RingSliceMaxRounds` ×2、`分片推迟` ×1、`0x5F39CE0` ×1 |
| 部署（MO2） | 流水线 `& tools\build-sas.ps1 -SkipPluginBuild -SkipDllBuild -SkipPapyrusCompile`（0.2 s）；`modid=18268` / **`version=2.0`**；**部署 DLL = 构建 DLL（逐字节一致）** |
| 部署 INI | 替换前核对 = 上一版模板 **`081825EC…`/93 059 B（逐字节一致 ⇒ 无用户自定义）**，旧文件另存 `SAS_AlwaysScan.ini.bak-v51r2`；新模板 **`6644670A…`/96 182 B**（含 `RingSliceMaxRounds=5`） |
| 待办 | 发布包重打（`tools/package-nexus.ps1`）、git 提交 |

---

## 四、验收判据（下一次进游戏）

1. 启动日志：
   ```
   SAS_AlwaysScan v5.1.0 loading（订正 R3：环内 cell 分片遍历 + loop/sync 细分计时）
   config: ringSliceMaxRounds=5 -> 环内 cell 分片：有效片数=5（每轮只遍历每 cell 的 1/5 片，…分片周期 ≈ 1000ms 必须 < UnhighlightGraceMs=1500ms…）
   ```
2. 统计行 `timing2` 出现新格式，且 **`ring`（每轮遍历的环内引用数）显著小于旧值**、
   `分片推迟` 同步增长（= 分片真的生效）；外景 / 大场景下 **`loop avg` 应明显下降**
   （预期从 55ms 量级降到 ~15ms 量级；如果仍然是几十 ms，把 `refs/scan: cur=… ring=…`
   与 `walk/loot/flora` 发出来 —— 那就说明大头在 `cur` 或别的段，下一步据此再改）；
3. ★ **目视不闪烁**：站在边界 / 大场景里走动，环内的目标（隔壁 cell 的东西）
   应保持常亮，不应出现「亮一下灭一下」；
4. 不回归：F8 / 扫描仪自愈（举放后 1 秒内恢复）/ 展示柜拿空熄灭 / 换场景不崩 /
   活人不亮 / 分组配色 / 植物「未扫描青 ↔ 已扫描绿」与按物种扩散；
5. `引擎状态表: 条目=` 仍保持稳定（R2 的债不复发）。

★ **可调项**：仍觉得外景掉帧 ⇒ 试 `RingSliceMaxRounds=8`（更细，周期会被宽限期自动钳到 6）；
想更保守（目标刷新更快）⇒ 试 `3`；要对照旧行为 ⇒ `0`。

---

## 五、经验（写给下一轮）

1. **「耗时」要配上「分母」**：`loop avg=55ms` 只说明「慢」，`refs/scan: cur=… ring=…`
   才说明「慢在遍历了多少条」。**下次加计时器时，把计数一起加上** ——
   这一轮就是靠 `kRingRefsCap=60000` × 单条 ~0.9µs 才对上账的。
2. **上限常量要换算成实际耗时**：`kRingRefsCap = 60000` 从 v4.7 起就在，
   当时只当成「防无界增长」的保险；一旦换成「60000 × 1µs = 60ms」就立刻看出
   它其实是**每轮的现实成本**。给人看的常量注释里最好直接写上这条换算。
3. **优化与「宽限期」这类时间常量必须一起推导**：分片会把「同一目标被重新考虑的间隔」
   拉长到 `片数 × 扫描间隔`，而「离开宽限期」是摘除的判据 ——
   两者不匹配就是「摘掉 → 挂回」的闪烁。做法 = **把不变量写进配置钳制**
   （`eff = min(用户值, (grace - interval)/interval)`），并在启动日志里打印既有值又有推导。
4. **改动要么有回退开关，要么有可证伪的数字**（本次两者都有）：
   `RingSliceMaxRounds=0` 回退；`分片推迟` / `refs/scan` 让用户（和我们）能一眼验证
   「分片是否真的生效、效果有多大」。
5. **别把「场景相关」与「时间相关」混为一谈**：上一轮是「越玩越卡」（表增长），
   这一轮是「某些场景卡」（遍历量大）—— 症状都是「帧数下降」，
   **区分它们靠的是「同一局里的时间序列」**（scan avg 是单调上升还是随 cell 数跳变）。
