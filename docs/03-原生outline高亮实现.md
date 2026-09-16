# 03 · 原生 outline 高亮实现（v2）

> 2026-09-16 建立。目标：把「扫描仪高亮」从旧方案（Play 一条 EFSH，看起来是一团发光）
> 换成**引擎自带的那套描边渲染** —— 和原版手持扫描仪 / 任务追踪目标 / 工坊高亮**同一套**。
>
> 游戏版本：**Starfield 1.16.244.0**。所有 RVA 都是这个版本的实测值，代码里有首 16 字节
> 签名校验，版本一变会直接拒绝启用（而不是崩游戏）。

## 一、为什么不能用旧方案（EFSH）

上一代 mod（`高亮物品`）和本项目 v1 都是给对象 `EffectShader.Play`。那只会在对象身上
叠一层**发光着色**，而原版扫描仪画的是**轮廓描边 + 少量填充**，两者观感完全不同。
用户反馈「相差甚远」说的就是这个。

原版那套高亮是数据驱动的：整套 GMST 都在 `:Monocle` / `:Highlight` 分类下，按**状态**
分支配色与粗细：

```
aHighlightScannableOutlineColorHigh/Low :Monocle   ← 描边色（两组）
aHighlightScannableFillColor            :Monocle
fHighlightScannableOutlineThickness     :Monocle
fHighlightOutlineColorPulseTime         :Monocle
        每种都有 _Generic / _Scanned / _FullyScanned / _Tracked / _Bounty / _Social / _Target* 变体
```

## 二、逆向出来的真实结构（这才是关键）

### 2.1 一张「引用 → outline 状态」的表

```
std::map<TESObjectREFR*, uint32_t>  g_outlineState    // 树头在 .data RVA 0x5F39CE0
```

节点布局（`0x30` 字节）：`+0x00/0x10/0x18` = 红黑树链接，`+0x20` = 键（引用指针），
`+0x28` = **状态值**（`uint32_t`，`0..11` 是真实样式，`12` = 无高亮）。

### 2.2 12 个 HighlightManager

```
void* g_outlineManagers[12]     // .data RVA 0x5F39CF0，每个状态一个
```

`HighlightManager`（对象 0x48 字节，vtable RVA 0x4B2FA50，只有一个虚函数＝析构
RVA 0x653400）内部：

| 偏移 | 含义 |
| --- | --- |
| `+0x00` | vptr |
| `+0x08` | u32（另一个计数字段） |
| `+0x18` | 内嵌哈希表（开放寻址 + 链）：`+0x08`=数据、`+0x10`=容量(2 的幂)、`+0x18`=元素数、`+0x20`=水位 |
| `+0x40` | u32（构造函数里先写 `0xFFFFFF`，再交给 `0x7cb120`） |

哈希表元素 12 字节：`{ key(u32), next(u32), prev(u32) }`，`+4 == -1` 表示空槽。
**键是 24 位 FormID**（`0x653040` 会校验 `(id&0xFFFFFF) < 0xCBF00` 且 `id>>24 != 0`）。

插入函数 = **RVA `0x6535B0`**：`bool AddId(map*, u32* idPtr)`，返回「是否新插入」。
插入路径末尾会调 `0x653850(rcx=&id, rdx=&params32)` 把该 id 的**高亮参数（32 字节）**
写进渲染器的 StorageTable（`StorageTable::Highlight::UStorage`），再由 `0x653040(id)` →
`0x653920(id)` 激活渲染器那侧的 id 表项（`[rbx+0x2c8] + id*4` 作为索引表）。

### 2.3 引擎自己的公开入口（我们用这三个）

| 用途 | RVA | 签名（都是「引用指针的地址」） |
| --- | --- | --- |
| 查/插入状态槽 | **0x17D5BE0** | `uint32_t* LookupOrAdd(void* 未用, TESObjectREFR** slot)` → 返回状态字段地址 |
| 挂高亮 | **0x17D52B0** | `void Set(void* slot, uint32_t state)`（slot 在 **rcx**，state 在 **edx**） |
| 摘高亮 | **0x17D4F10** | `void Clear(void* 未用, TESObjectREFR** slot)`（slot 在 **rdx**） |

> 注意 `0x17D52B0` 和 `0x17D4F10` 的参数寄存器**不一样**（前者 rcx，后者 rdx），
> 这是从反汇编里逐条对出来的，别想当然。

`LookupOrAdd` 只用到 `rdx`；`rcx` 传 `nullptr` 即可（它内部会自己设 rcx）。

### 2.4 ★ 为什么必须先 LookupOrAdd 再 Set

`SetOutlineState`（0x17D52B0）开头的红黑树查找如果**找不到该引用就直接返回**，
什么都不做。所以正确顺序是（和引擎自己 `0x17D4CD0` 尾部 `mov dword ptr [rax], ebp` 完全一致）：

```cpp
auto* p = LookupOrAdd(nullptr, &ref);
*p = state;
Set(&ref, state);
```

### 2.5 ★ 唯一的崩溃风险：管理器可能不存在

`Set` 的调用链是 `0x17D52B0` → `0x17D4CD0` →（用状态取管理器）→ 然后
`mov rcx, [g_outlineManagers + state*8]` … `mov eax, [rcx + 0x40]`。
**如果那个槽是 0，就是 0xC0000005 空指针崩溃。**

引擎只在「进入扫描模式」时才建这 12 个管理器（RVA `0x17D47B0` 负责建立并按当前 GMST
刷新配色），所以**不装备扫描仪时它们很可能是空的**（这正是本 MOD 要解决的场景）。

因此：

1. 每次挂高亮之前先读 `g_outlineManagers[state]`，为 0 就**绝不**调 `Set`；
2. 为 0 时调用引擎自己的重建函数 **RVA `0x17D47B0`**（`void Ensure(void*)`，
   `rcx` 未使用，是幂等的），再复查；
3. 统计日志里打印 `outline=<已挂数>/<nativeReady>/<存活管理器数>`，
   从日志就能判断「管理器到底存不存在」。

## 三、状态值 0..11 怎么选

从 `0x159ED90`（引擎算 outline 状态的地方）反出来的组合逻辑：

```
bl ? (7 + (al?1:0)) : [原值]
若 == 12:
   r13b ? (2 + (al?1:0)) : (0 + (al?1:0))          // al 大概率是「已扫描」
另一条分支：追踪目标用 9
```

即 `0..1` / `2..3` / `7..8` / `9` 各成一组，`12` 是哨兵（无）。
所以 `OutlineState=0` 就是「未扫描的通用高亮」—— 最接近原版扫描仪看到 loot 时的样子。
想换颜色/样式就试这几个值，**不需要改代码**。

## 四、本 MOD 的做法

```
每 200ms 扫一轮 cell 引用（沿用 v1 的「实测偏移 + 形状校验」）
  → 距离 / base form 白名单 / （可选）正前方夹角 过滤
  → 距离分档挑选（每 2 米 6 个，上限 MaxTargets）
  → SyncNativeOutline():
       新目标 / 距上次重申 > ReassertMs  →  Ensure + LookupOrAdd + Set
       掉队的目标                      →  LookupOrAdd 写 12 + Clear
```

- **不再需要令牌桶、FormList 信箱、Papyrus 桥**（那些是 EFSH 方案的产物，
  `HighlightMode=0` 时仍然保留可用，作为对照/回退）。
- `ReassertMs`（默认 3 秒）：引擎换场景 / 退出扫描模式时会销毁重建自己的管理器，
  所以周期性重申一次，避免「高亮忽然全没了」。
- 关热键 / 换 cell / 读档 → 把已挂的引用状态写回 `12` 并调 `Clear`。

## 五、还有一个「和原版不一样」的地方：选哪些对象

原版扫描仪有**两层**限制：距离（`fHandScannerScanRange`，默认 10 米）
+ **屏幕中央圆圈**（`fScanHighlightDotAngle`）。本 MOD 要去掉圆圈，改成「整个屏幕」。

v2 提供 `OnlyInFront` / `FrontFovDeg`：用玩家偏航角做水平夹角判定，把身后的目标排除，
这样才能接近「看得见才亮」。**默认关闭**，因为偏航角→前向量的符号约定需要进游戏确认
（代码里是 `kYawForwardSign`，反了改符号即可）。

## 六、工具（本轮新增 / 改进）

| 工具 | 用途 |
| --- | --- |
| `tools/re/func.py` | **新增**。`at`（地址 → 所在函数并反汇编，用 int3 填充反推函数起点）、`callers`（直接 call/jmp 调用者）、`qword`（8 字节绝对引用，找 vtable 槽）、`rip`（RIP 相对引用，找「谁 lea 了这个 vtable / 谁读了那个全局」）、`strings`、`vtable` |
| `tools/re/colname.py` | **新增**。MSVC CompleteObjectLocator RVA → 类名（识别 vtable 归属） |

`rip` 用的是「指令前缀 + ModRM mod=00/rm=101」模式匹配，比全量 capstone 线性扫描快几十倍；
**踩坑**：一开始把 `disp32` 当成相对 RVA 加了，正确做法是相对**下一条指令的 VA**。

## 七、如果哪天游戏更新了

1. 用 `func.py rip` / `callers` 重新走一遍本文的推导链，确认新 RVA；
2. 改 `plugin/src/AlwaysScan.cpp` 顶部的 5 个 `kRva*` + 4 个 `kSig*`；
3. 更省事的替代：改用 Address Library（`REL::ID`），但 `HighlightManager` / 这几个
   函数在 commonlibsf 的 `IDs.h` 里**没有**条目，需要先自己挖出 ID。

## 八、还没验证的部分（下次实测重点）

- [ ] 不装备扫描仪时 `g_outlineManagers` 是否为空（看日志 `outline=` 后面的管理器数）；
- [ ] `AutoEnsureManagers=1` 建出来的管理器，渲染侧是否真的会画（即
      `GatherHighlightsRenderPass` 是否受 Monocle 状态门控）；
- [ ] `OutlineState=0` 的实际颜色/粗细是否就是原版扫描仪的样子，不是就试 1/2/3/7/8/9；
- [ ] 摘除是否干净（关热键后不应留残留描边）；
- [ ] 多几何体对象（比如容器/门）是否整体描边（`Set` 里除了塞管理器还有一遍
      「遍历 attached objects」的逻辑，我们走的是同一条路，理论上应该一致）。
