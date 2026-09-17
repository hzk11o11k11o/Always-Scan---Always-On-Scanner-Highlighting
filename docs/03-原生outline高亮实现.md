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

---

## 九、v2.1 修正（2026-09-16，第二次逆向）

游戏内实测 v2 的结果：**描边出来了、观感确实是原版那套**，但有三个问题，其中
「按 F8 关不掉」逼着我把「摘除」这条链重新逆了一遍，发现了 v2 里一个**根本性的错误**。

### 9.1 ★ 0x17D4F10 不是「摘高亮」（v2 的 F8 关不掉的根因）

v2 里以为 `0x17D4F10` 是 Clear，所以「关开关」的流程是：写状态 12 → 调它。实际反汇编：

```
017D4F58  cmp byte [rcx+0x19], 0     ; 红黑树的 lower_bound 结果是不是哨兵
017D4F5C  jne 0x17D4F6E              ; 是哨兵 → 走「做活」那条路
017D4F61  cmp r8, [rcx+0x20]         ; key < node->key
017D4F65  jb  0x17D4F6E
017D4F67  cmp rcx, rdx               ; rcx == 树头（= 已经找到）
017D4F6A  jne 0x17D4FC1              ; ★ 找到了 → 直接 ret
017D4F6C  jmp 0x17D4F71              ; 没找到 → 才去 LookupOrAdd + 写 12
```

也就是说它的语义是「**该引用还不在状态表里**才建一条并写 12」——对**已经挂上高亮的
引用是空操作**。而真正在画描边的是「状态 → HighlightManager → 它内嵌的哈希表
（键 = FormID）」：**只改状态位根本不会把 id 从哈希表里删掉**，渲染器照画。
⇒ v2 的「关」等于没关。

### 9.2 引擎自己的摘除流程（0x653F60，就是三条指令）

```
00653F60  ...                                   ; 参数 rcx = 某个持有管理器指针的上下文, rdx = 引用
00653F6F  call [rax + 0x50]                     ; 取 id（rsi → [rax+0x1f0] 那一支）
00653F81  mov ebx, [rax + 0x1f0]                ; id
00653F8B  mov [rsp+0x38], ebx
00653F8F  mov rcx, [rax]                        ; ← 管理器指针
00653F92  add rcx, 0x18                         ; ★ map = manager + 0x18
00653F96  call 0x6535B0                         ; Remove(map, &id) → 是否真的删到
00653F9B  test al, al
00653F9D  je 0x653FA6
00653FA1  call 0x653040                         ; ★ Deactivate(id)：渲染器侧注销
```

两个函数的真实签名（这是 v2.1 新增的两个 RVA）：

| 用途 | RVA | 签名 | 首 16 字节 |
| --- | --- | --- | --- |
| 从管理器哈希表删 id | **0x6535B0** | `bool Remove(void* map, u32* id)` | `48 89 5C 24 10 57 4C 8B 51 10 4C 8B D9 4D 8B CA` |
| 渲染器侧注销 | **0x653040** | `void Deactivate(u32 id)` | `48 83 EC 28 81 F9 FF FF FF 00 74 40 4C 8B 0D 25` |

（v2 文档里把 `0x6535B0` 当成过「AddId」——**错了**，它是 Remove：函数体里
`lea rax,[rbx+rbx*2]; lea rdx,[r8+rax*4]; … mov qword [rdx+4], -1` 就是把槽标记成空。）

`0x653040` 自己带 id 合法性校验（`id == 0xFFFFFF` / `(id & 0xFFFFFF) >= 0xCBF00` /
`id>>24 == 0` / 载入序字节对不上 → 直接返回），所以拿一个「可能不对」的 id 调它是安全的。

### 9.3 管理器内嵌哈希表在 `manager + 0x18`

两处交叉确认：

- ctor **0x6532F0**：`lea rax,[rdi+0x10]; lea rbx,[rax+8]` ⇒ rbx = manager+0x18；
  随后 `[rbx+8]`=data / `[rbx+0x10]`=capacity（初值 0x20）/ `[rbx+0x18]`=count /
  `[rbx+0x20]`=watermark，并把每个 12 字节元素的 `+4` 写成 `-1`（空槽标记）。
- `0x653F60` 里 `add rcx, 0x18; call Remove`。

### 9.4 管理器是 **11** 个，不是 12 个

- `0x17D4CD0` 开头：`movsxd rbp, r8d; cmp ebp, 0xB; jae <ret>` ⇒ 状态 0..10 有效；
- `0x17D47B0` 建表循环：`mov r12d, 0xB`。

所以日志里 `managers = 11/11` 才是全部都在（v2 打的是 `11/12`，看着像缺一个）。
另外 **`0x17D47B0` 只创建缺失的管理器**（已有则只刷新参数），反复调用**不会**清空高亮。

### 9.5 顺带修掉的观感问题

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 货架上总有两三个罐子/碗不亮 | `MaxTargets` 之外的**分档限流**（每 2 米一档、每档 6 个）：同一档里排不进前 6 的就永远不亮 | 去掉分档限流，改成「按距离从近到远取到 MaxTargets」 |
| 描边「一个一个慢慢出来」 | 疑似周期性重申（`ReassertMs=3000`）往引擎的高亮请求通道里灌重复请求 | `ReassertMs` 默认改成 **0 = 不重申** |
| 换场景/关开关后管理器里攒垃圾 id | 旧「摘除」从没删掉过任何 id | 关开关、换场景、目标掉队都走真正的 Remove+Deactivate |

### 9.6 新增诊断日志

- 每 5 秒一行 `outline remove: rmOk=.. rmMiss=.. removeReady=..`
  （`rmMiss` 持续增长 = 键值/RVA 对不上，摘不到东西）；
- 每 5 秒（当有内容时）一行 `rejTypes (半径内但类型不在白名单): ft=NxM, ...`
  ——「某个东西该亮却没亮」时，看它的 formType 是多少，再决定要不要加进
  `IsHighlightableBase` 的白名单。

### 9.7 仍然没解决 / 没验证

- [ ] **任务引导路径（地上那条线）**：它不属于 outline 高亮系统。已确认
      `Starfield.esm` 里存在 EDID = `ScannerGuideEffect` 的形态（另有
      `ArtifactPowerPrecognition_GuideEffect`），以及一套 `Guide*` INI 设置
      （`GuideNoPath` / `GuideWaypoints` / `GuideSpacing` / `GuideTargetRadius` …）。
      也就是说引导路径很可能是**挂在玩家身上的一个魔法效果**；要让它出现，得找到
      那个效果所在的具体形态（MGEF/SPEL）并把它加到玩家身上。见 `docs/04`。
- [ ] 「一个一个点亮」只在去掉重申后**可能**好转，需要实测确认；若依旧，下一步要
      查 `0x17D4CD0` 里那个经由 `0x24181E0` 调用的 functor（vtbl 0x4B2F9F0）是不是
      异步/限流的请求通道。

---

## 十、v2.2 第三次逆向（2026-09-16）—— 「摘除」的 id 根本不是 FormID

### 10.1 实测数据（v2.1 的日志）

```
scan#1331 ... outline=256/1/11 ...
  outline remove: rmOk=0 rmMiss=4965 removeReady=1
...
native outline cleared (n=256 removeOk=0 removeMiss=256 totalRemoved=0)
```

`rmOk` **恒为 0**：我们自己拼的 `Remove(map, &FormID) → Deactivate(FormID)` 一次都没命中。
说明**管理器哈希表里的键不是 `TESForm::GetFormID()`**，我们一直在拿一个错的 id 去删。

顺便看出一条更重要的线索（同一份日志）：

```
[20:21:41.619] reset (first cell) ... outline=256
[20:21:59.713] native outline cleared (n=256 ...)      ← 相隔 18.1 秒！
```

这 18 秒里一行日志都没有 —— 也就是说 `ClearAllNativeOutline()` 那个「一帧里发 256×12 条
引擎调用」的循环**卡住了 18 秒**（正好落在换区域/载入的时候，主线程在跑加载，
每条调用都被拖慢到几十毫秒）。这就是「轻微卡顿」的大头。

### 10.2 ★ 引擎自己的摘除函数 `0x653F60`

顺着 `Remove(0x6535B0)` 的调用者往回找，**全镜像只有一处**调用它（`Deactivate 0x653040`
也一样）——就是 `0x653F60`，而且它**没有任何直接调用者**（是虚函数/函数指针表里的）：

```
00653F60  push rdi / sub rsp,0x20
00653F66  mov rax,[rdx]          ; rdx = ref → vtable
00653F69  mov rdi,rcx            ; rdi = ctx
00653F6C  mov rcx,rdx            ; this = ref
00653F6F  call [rax+0x50]        ; 引擎自己的一层间接（取内部对象）
00653F72  test rax,rax / je ret
00653F81  mov ebx,[rax+0x1F0]    ; ★ id 由引擎自己算 —— 我们不碰
00653F87  mov rax,[rdi+8]        ; ctx+8
00653F8F  mov rcx,[rax]          ; ★ *(ctx+8) 必须指向「管理器指针」本身
00653F92  add rcx,0x18           ; 内嵌哈希表
00653F96  call 0x6535B0          ; Remove(map, &id)
00653F9B  test al,al
00653FA1  call 0x653040          ; Deactivate(id)
```

⇒ **我们根本不需要知道「键是什么」**：只要造一个
`ctx = { unused, &g_outlineManagers[state] }`（即 `*(ctx+8)` 就是管理器指针），
调 `0x653F60(ctx, ref)` 就等于引擎自己摘一次高亮。v2.2 就是这么做的。

### 10.3 哈希表布局（复核，和 v2.1 的结论一致）

`Remove(0x6535B0)` 的反汇编（`rcx` = map）：

```
mov r10,[rcx+0x10]        ; capacity（2 的幂，用 and (cap-1) 取槽）
mov rdx,[r11+8]           ; data
mov ebx,[rdx]             ; ★ 键 = 32 位，直接从 id 指针里读
... CRC32 查表哈希（表在 rip+0x470579e）
cmp [data + slot*12 + 4], -1   ; 空槽
cmp [data + slot*12], ebx      ; 键比较
```
元素仍是 12 字节 `{ key(u32), next(u32), prev(u32) }`。
另外 `0x6530A0`（批量把 32 字节高亮参数写进渲染器 StorageTable）里的 id 是
**`0x7CB120(&id)` 现算的**，也不是 FormID —— 这进一步说明「键 ≠ FormID」。

同时 `0x653850` 把 id `& 0xFFFFFF` 当作**下标**去索引 `[renderer+0x2c8]` 的数组，
再写 `[renderer+0x3c8] + idx*0x20` 的 32 字节参数 —— 也就是「本地 FormID 索引表」，
但**这张表用的 id 和「管理器哈希表的键」不是同一个来源**，所以不要用 FormID 去猜管理器。

### 10.4 v2.2 的改动

| 问题 | 改法 |
| --- | --- |
| `rmOk=0`（摘不掉 → 渲染器里攒垃圾 id → 越玩越卡） | 优先调**引擎自己的 `0x653F60`**（带首 16 字节签名校验）；失败才退回旧的 FormID 路径；最后无论如何把状态写回 12 |
| 目标在集合边缘反复进出 → 每 200ms 一波 Set/Remove | **宽限期**（`UnhighlightGraceMs`，默认 1500ms）：掉队先只打时间戳，到点还没回来才真摘 |
| 换场景一帧发 3000 条调用（实测卡 18 秒） | **单轮预算**（`MaxOutlineOpsPerScan`，默认 64），换场景改成「标待摘 + 分几轮慢慢摘」，并且**载入画面期间完全不动作**（`LoadingMenu`/`FaderMenu`） |
| 每轮对每个被拒引用做一次哈希表自增（每秒 2 万次） | `rejectTypes` 换成 `std::array<uint32_t,256>` |
| 「到底卡在哪」说不清 | 统计日志新增 `timing: scan avg=…ms max=…ms ops=… deferred=… loading=…`，以及每 30 秒一次的 `manager map[…] cap=… count18=… 前 N 个非空键=[…]`（这一行会直接把「键到底是什么」摊开） |

### 10.5 下次实测要看什么

```
  outline remove: rmOk=NN unhMiss=NN removeMiss=NN removeReady=1 unhighlightReady=1
  timing: scan avg=Nms max=Nms ops=N deferred=N loading=0
manager map[runtime] state=0 : mgr=0x… cap=… count18=… count20=… data=0x…
manager map[runtime] state=0 : 前 N 个非空键 = [0x…, …]
```

- `unhighlightReady=1`：`0x653F60` 签名校验通过（=0 会打 `signature mismatch` 警告）；
- `rmOk` 开始增长、`unhMiss` 停止增长 ⇒ 摘除终于真的生效了；
- `max` 应是个位数毫秒；`ops` 稳态应接近 0（站着不动时宽限期把抖动全吃掉了）；
- `前 N 个非空键` 直接把管理器哈希表的真实键贴出来 —— 这一步之后不用再猜 id 了。

---

## 十一、v2.3 第四次逆向（2026-09-16 夜）—— 摘除的参数是「3D 节点」，不是「引用」

### 11.1 实测数据（v2.2 的日志）

```
scan#719 ... outline=256/1/11 ...
  outline remove: rmOk=0 unhMiss=3694 removeMiss=3694 removeReady=1 unhighlightReady=1
manager map[runtime] state=0 : mgr=0x203b7f85d88 cap=4096 count18=13 count20=287
  timing: scan avg=47ms max=146ms ops=0 deferred=0 loading=0
```

三个致命信号：

1. **`unhMiss` 一直在涨** —— 连引擎自己的 `0x653F60` 都一次没动到管理器里的东西；
2. **`timing: scan avg=47ms max=146ms ops=0`** —— 单轮扫描（纯内存读！）要 47ms；
   甚至第 1 轮「什么都还没干」就花了 31ms（`scan#1 ... cand=0 sel=0 ... avg=31ms`）；
3. 用户的观感三条都能解释：**F8 关不掉已亮的**、**走远了还亮**、
   **已高亮区域也卡**（管理器哈希表只增不减，渲染器每帧都要过一遍）。

### 11.2 ★ 根因：`0x653F60` 的第二个参数是 3D 节点

把 **`0x17D4CD0`**（就是 `Set(0x17D52B0)` 内部真正干活的那个函数）整段读完之后，
链条才完全清楚：

```
017D4CF7  mov rcx, [rdx]              ; rcx = TESObjectREFR*（引用）
017D4D4C  mov rax, [rcx]              ; 引用虚表
017D4D57  call [rax + 0x560]          ; ★ Get3D(NiPointer<NiAVObject>&) → rsi = 3D 根节点
017D4D91  movsxd rax, [rbx+0x28]      ; 树节点里记的「旧状态」
017D4DA1  mov rax, [managers + rax*8] ; 旧管理器
017D4DB3  lea rax, [rip+0x335ac36]    ; = 0x4B2F9F0（摘除 visitor 的 vtable）
017D4DC9  mov rdx, rsi                ; arg2 = ★ 3D 节点
017D4DD1  call 0x24181E0              ; ★ 递归遍历 3D 图，每个节点摘一次
...
017D4DFF  lea rax, [rip+0x335ac0a]    ; = 0x4B2FA10（挂上 visitor 的 vtable）
017D4E22  call 0x17D5BE0 ; mov [rax], ebp    ; 状态写进「引用→状态」表
```

而 **`0x24181E0` 是「递归遍历 3D 图」的访问器**（本次新挖出来的东西）：

```
02418207  call [rax+0x20]        ; node->vtable[0x20]() → 子节点容器（NiNode 才有）
02418210  jne 0x2418220
02418212  mov rax,[r15]          ; r15 = visitor
0241821B  call [rax+0x10]        ; ★ visitor->vtable[0x10](visitor, node) ← 叶子才回调
02418230  movzx eax, word [rbp+0x142]   ; 子节点数
02418242  mov rbx, [rbx+rdi*8]          ; 子节点
02418265  call 0x24181E0                ; 递归
```

两个 visitor 的 vtable（`tools/re/func.py vtable` 核对过，都在 .rdata）：

| visitor | vtable RVA | 槽 +0x08 / +0x10 | 数据 |
| --- | --- | --- | --- |
| 摘除 | `0x4B2F9F0` | `0x653F60` / `0x653F60` | `void** 管理器槽`（&managers[state]） |
| 挂上 | `0x4B2FA10` | `0x653FC0` / `0x653FC0` | `{ manager, manager->0x40 }` |

再回头看 `0x653F60` 自己就全对上了：

```
00653F66  mov rax, [rdx]        ; rdx = ★ 节点
00653F6F  call [rax+0x50]       ; 节点 → 「持有它的引用对象」
00653F81  mov ebx, [rax+0x1F0]  ; id 由节点侧算出
00653F87  mov rax, [rdi+8]      ; rdi = visitor；visitor+8 = 管理器槽
00653F8F  mov rcx, [rax]        ; = manager
00653F92  add rcx, 0x18         ; 内嵌哈希表
00653F96  call 0x6535B0         ; Remove(map, &id) → 删到才 Deactivate
```

⇒ v2.2 把 `TESObjectREFR*` 直接塞进去，`ref->vtable[0x50]` 取到的东西
`[+0x1F0]` **根本不是管理器表里的键** ⇒ `Remove` 永远返回 false ⇒
`Deactivate` 永远不执行 ⇒ 描边永远摘不掉。**这就是用户三条反馈的共同根因。**

`Get3D` 的位置还有旁证：commonlibsf 的 `TESObjectREFR.h` 里第 **0xAC** 号虚函数
注释正好是 `// 0xAC - Get3D(NiPointer<NiAVObject>&)?`，而 `0xAC * 8 = 0x560`，
与引擎自己 `call [rax + 0x560]` 完全吻合。

### 11.3 v2.3 的修法

```cpp
// ① 取 3D 根：走引用虚表下标 0xAC（顺手校验目标在主模块 .text 内，防虚表布局变了）
RE::NiPointer<RE::NiAVObject> root;
((RefGet3D_t)((void**)*ref)[0xAC])(ref, &root);
// ② visitor = { vtable = 0x4B2F9F0, &g_outlineManagers[state] }
// ③ 引擎自己递归整棵 3D 图：0x24181E0(&visitor, root)
```

- 三个新符号（`0x24181E0` / `0x4B2F9F0` / 虚表下标 `0xAC`）都做校验：
  `0x24181E0` 首 16 字节签名；`0x4B2F9F0` 的 `+0x08` 与 `+0x10` 必须都等于 `0x653F60`；
  `0xAC` 槽取出来的地址必须落在主模块范围内。任一不过就整体禁用并打 WARN。
- **删掉「11 个状态全扫一遍」的兜底**：现在只调「状态表里记的那个」和「配置的那个」
  各一次。这不是省事，是省钱 —— 每次尝试都是一次真的引擎调用（虚调用 + 哈希查找 +
  **整棵 3D 图递归遍历**），v2.2 里单轮 64 个目标 × 最多 11 次尝试 = 700 多次调用，
  实测 `timing max=438ms` 里一大半是它。

### 11.4 顺手治掉的「31ms 假卡顿」

`scan#1 ... cand=0 sel=0 ... timing: scan avg=31ms ops=0` —— 第一轮扫描几乎什么都没干
（引用数组长度第一次见就直接 return），却花了 31ms。那一段里唯一的重活是
`ValidateCellRefs` 里的 6~7 次 `VirtualQuery`。

`VirtualQuery` 要走内核对 VAD 树，在这种几 GB、VAD 碎片极多的进程里单次就是毫秒级；
而它要回答的问题（这几个地址能不能读）对「同一个 cell 的同一批对象」几乎是常量。

⇒ `IsReadable` 加**「已验证可读区间」缓存**（8 槽直接映射，TTL 3 秒；
cell 变化 / 读档时整体作废；从没验证过的地址永远返回 false）。
统计日志新增 `vq=N/scan`，可以直接看到内核询问次数降到多少。

另外把 `Rescan` 拆成 `shape / loop / sync` 三段分别计时（`timing2:` 行），
下次实测不用再猜「47ms 花在哪」。

### 11.5 下次实测要看什么

```
  outline remove: rmOk=NN unhMiss=NN removeMiss=NN removeReady=1 unhighlightReady=1 graphRemove=1 mapCnt=NN
  timing2: shape avg=0ms max=1ms vq=0/scan | loop avg=1ms max=2ms | sync avg=0ms max=0ms (unh avg=0ms add avg=0ms)
```

- `graphRemove=1` 是前提（=0 会打 WARN，说明校验没过，摘除仍不准）；
- **`rmOk` 应该跟着 `outline` 一起涨落**，`unhMiss` 不再增长；
- **`mapCnt` 应该能看到回落**（走远 / 按 F8 之后）；
- `shape` / `loop` 都应该是 **0~2ms**；`vq` 应该接近 0；
- 观感：F8 → 描边**立刻全灭**；走远 → 远处的东西**会熄灭**。

---

## 十二、v3.1 追加逆向（2026-09-17）—— 「开着 MOD 又举扫描仪」会不会冲突

> 起因：用户提问「开着这个 MOD，同时又用扫描仪扫描，是否会产生冲突」。
> 因为两条路共用同一套 outline 引擎管线，这次把**引擎侧的写入方与销毁方**全部找了出来
> （不靠推测，全部是 xref + 反汇编实证）。结论：**没有功能级冲突，只有一个「观感级」的
> 互相覆盖 + 一个低概率的「整表清空」风险**，详见 12.3 / 12.4。

### 12.1 共用的是同一张表、同一组管理器

MOD 的 `OutlineRef` 走 `LookupOrAdd(0x17D5BE0)` + `Set(0x17D52B0)`，而引擎写 outline
状态**全镜像只有这一条路**（下面 12.2 的 xref 结果）。所以「MOD 的高亮」和「扫描仪的高亮」
不是两套系统，而是**同一张 `map<TESObjectREFR*, state>` + 同一组 11 个 HighlightManager
的两个写入方**。这是分析一切冲突的前提。

### 12.2 引擎侧到底谁在写、谁在清（本次新挖出来的地址）

用新工具 `tools/re/findrefs.py`（按数据地址查全部 RIP 相对引用，读完 capstone 复核）
把相关的三处查了个遍：

| 目标 | 结果 |
| --- | --- |
| `0x17D52B0`（Set）的直接调用者 | **只有 `0x159ED90`**（两处：`+0x595` / `+0x8C4`） |
| `0x17D5BE0`（LookupOrAdd）的调用者 | `0x159ED90`、`0x17D4CD0`（Set 内部）、`0x17D4F10`、`0x17D5590`（Set 内部「换状态」分支）、`0x17D5830`、`0x17D5D40`（后两个是「按引用查/摘」的公开包装） |
| 「全部销毁」函数 `0x17D4B30` 的调用者 | **只有 `0x1598910`**（= Monocle HUD 对象的析构路径） |
| Monocle HUD 对象的全局指针 RVA `0x61EEA28` | **全镜像 21 处引用 = 4 写 + 17 读**（4 写分别落在 `0x1597E80` 构造、`0x1598910` / `0x15F86A0` / `0x15F8490` 三条析构/重置路径上） |

`0x159ED90` 是「**取一个引用 + 位置，算它该用什么状态，然后写表**」的评估函数：
开头是一段 `[数据 + 0x8C]/[+0x94]` 的位置读取、距离平方与点积（视角/距离判定），
然后调 `LookupOrAdd` + `Set`。它**逐个引用**处理、不做全表遍历、不写 `12` 兜底。

而 `0x17D4B30` 是「**把整张表清干净**」：

```
0x17D4B30  lea rbx, [g_outlineManagers]      ; 11 个槽逐个：
           mov rdi, [rbx] / mov [rbx], 0      ;   · 拿指针、槽清 0
           call [rax]                         ;   · 虚调用析构
           ...                                ;   · operator delete
           然后遍历「引用→状态」红黑树：每个节点
             ref->vtable[0x560]() = Get3D     ;   · 取 3D 根
             visitor{0x4B2F9F0, &managers[...]}; 0x24181E0(...)  ; 摘掉渲染侧的高亮
           最后 0x17D6250（递归销毁树）→ 树头归零
```

### 12.3 结论一：两个写入方在「非扫描模式」互不干扰

- 引擎的评估函数 `0x159ED90` **只在扫描仪自己的求值循环里被调用**
  （其调用者 `0x159EA70` 干的是「遍历容器的 `word [r14+0x12]` 索引」这类循环；
  实测也早就证明：不举扫描仪时 MOD 的描边能正常显示 ⇒ 引擎那条路没在跑）；
- 所以不举扫描仪时，**只有 MOD 在写状态表，不存在冲突**；
- 举着扫描仪时，引擎只对**它自己的候选集合**（约 10 米 + 屏幕中央圆圈的判定）逐个求值，
  `0x159ED90` 是「单引用求值」的，**不会顺手把 MOD 挂在 50 米处的那些引用改成 12**。

### 12.4 结论二：真正的互相影响只有两类

**① 扫描仪打开期间，视野内重叠的目标会被原版覆盖颜色（观感级，预期内）**

扫描仪对自己范围内的物品按原版规则写状态（已扫描 → 1/2/3 等），MOD 给它们的静态
`OutlineState=0` 会被覆盖。MOD 的 `outlined` 表仍认为「已挂」⇒ 不会立刻纠正。
但这是**原版本来就会发生的行为**（就是原版的「已扫描变色」），MOD 只是不参与；
按 F8 / 走远时 MOD 会按表里的真实状态把它们摘干净（`UnoutlineRef` 是**先读状态表里记的
值**再摘，不是硬编码 `OutlineState`），所以不会留残留。

**② 低概率：原版把整张表清空 → MOD 的 `outlined` 表变成悬空记录**

`0x17D4B30`（销毁 11 个管理器 + 清空状态表 + 摘掉渲染侧高亮）由 Monocle HUD 对象的
析构路径调用，**不是每次收扫描仪都会发生**：

- Monocle HUD 对象的全局指针 RVA `0x61EEA28` 全镜像只有 **4 处写**：
  `0x1597E80`（构造，`+0x40`）与 `0x1598910`（析构，`+0x319`）是一对
  （构造写入、析构清空，两者在同一个对象体系里）；
  `0x15F86A0`（`+0x46`，先读再清，是「把当前单例摘下来」的那条）与
  `0x15F8490`（`+0x14A`，重建/交换时写回新实例）。
- 也就是说这个对象**一次只会存在一个实例**：创建一次、销毁（或交换）一次，
  不存在「每次开扫描仪都重建」的实现。
- ★ 实证旁证：用户从 v2.3 起的多次实测（反复开关扫描仪、走动、换场景）都没有出现
  「高亮全灭」—— 与「收扫描仪不清表」一致。

⇒ 实操影响：正常游玩（开关扫描仪）**不触发**；只有「回到主菜单 / 读另一个存档 /
UI 大重建」这类时点可能触发。**一旦触发**，MOD 的 `outlined` 表会认为 256 个引用
「已挂」而实际已被清空，默认 `ReassertMs=0` 下要等它们掉出半径 + 宽限期走完才重挂，
表现为「高亮集体消失一阵」。

### 12.5 建议的防御（尚未实现，见 `docs/99` 待办）

1. **管理器消失 ⇒ 整表重挂**（最便宜）：`Tick` 里比较 `CountLiveManagers()` 与上一轮的值，
   从「满编」跌下来就 `MarkAllForRemoval(now)`（清空 `outlined`）⇒ 下一轮按
   `MaxOutlineOpsPerScan` 分批重挂。零额外引擎调用，专门治上面第 ② 类。
2. **`OutlineRef` 幂等化**（可选）：写表前先 `LookupOrAdd` 读一眼，等于 `wantState` 就跳过
   `Set` —— 省掉原版扫描后的一批重复写入。
   ⚠️ **v3.2 起不要再做这件事**：自愈依赖「重挂时真的调一次 `Set`」，
   幂等化会在「表里状态还对、但管理器里已经没了」时跳过修复。
3. 与扫描仪叠加时的观感取舍（可选）：用 `RegisterForMenuOpenCloseEvent("MonocleMenu")`
   感知玩家举着扫描仪，选择「让位原版」（暂停 MOD 的新挂）或「无视」（现状）。

---

## 十三、v3.2 修正（2026-09-17）—— ★ 「用完原版扫描仪后高亮全灭」的根因与自愈

> **用户实测反馈**：*「在打开原版扫描仪并关闭后，MOD 的扫描效果会消失，得按两下 F8
> 才会重新开启，似乎是官方扫描仪的关闭覆盖了本 MOD 的效果。」*

### 13.1 根因：第十二节预判的「整表清空」其实**每次开关扫描仪都会发生**

第十二节 12.4 的结论是「`0x17D4B30`（销毁 11 个管理器 + 清空状态表）由 Monocle HUD
的析构路径调用，**不是每次收扫描仪都会发生**」，并把它列为「低概率风险」。

**这次实测推翻了那个结论**：它就是正常开关扫描仪时发生的。用户描述的三件事
（① 高亮消失；② 不自愈；③ 按两下 F8 才回来）正好是这条链的完整症状：

| 步骤 | 引擎侧 | MOD 侧 |
| --- | --- | --- |
| 举起扫描仪 | Monocle HUD 存在，管理器在 | 我们的 256 条记录仍标记为「已挂」 |
| 放下扫描仪 | HUD 析构 → `0x17D4B30`：**11 个管理器被销毁、状态表被清空**、渲染侧高亮被摘 | `g_state.outlined` **不知道**，还认为 256 条都挂着 |
| 之后的每轮扫描 | 管理器已被我们 `autoEnsure` 重建（但**是空的**） | `SyncNativeOutline` 见到「已在其实已经不存在的表里」⇒ **什么都不做**（`ReassertMs=0` 默认不重申） |
| 按第一次 F8（关） | — | `ClearAllNativeOutline()` ⇒ 账本清空 |
| 按第二次 F8（开） | — | 重新 `OutlineRef` ⇒ 高亮回来 |

⇒ **不是官方的关覆盖了我们的效果，而是官方把整张表拆了，而我们的账本没跟着作废。**
第十二节 12.5 的建议 ①（管理器消失 ⇒ 整表重挂）正是治它的药，本次补上。

### 13.2 v3.2 的实现（`plugin/src/AlwaysScan.cpp`）

新增 `DetectEngineOutlineLoss()`（**每帧**在 `Tick` 里跑，在 200ms 扫描节流之前 ——
切换要即时被看到），两路判据都**不调用任何引擎函数**，只改我们自己的时间戳：

| # | 判据 | 说明 |
| --- | --- | --- |
| ① | **`MonocleMenu` 由开变关的那一刻** | 直接对应「放下扫描仪」。菜单名有 B 社自己的实证：`ScanTempleScript.psc` / `MQ_Temple_SubScript.psc` 用 `RegisterForMenuOpenCloseEvent("MonocleMenu")` + `abOpening` 判断「有没有举着扫描仪」。DLL 侧直接 `RE::UI::IsMenuOpen("MonocleMenu")` 轮询（和已有的 `LoadingMenu`/`FaderMenu` 同一套） |
| ② | **存活管理器数下跌** | `CountLiveManagers()` 从上一轮的值跌下来 ⇒ 引擎清过表（`0x17D4B30` 会把 11 个槽清 0）。也兜住「读档 / 回主菜单 / UI 大重建」这类时点 |

命中的动作是 `MarkAllForReassert(reason)`：把 `outlined` 里每条改成
`dropAt=0`（别当掉队摘掉）+ `reassertMs=0`（下一轮就重挂）。真正重挂复用已有的
`SyncNativeOutline` 第 3 步 ⇒ **天然受 `MaxOutlineOpsPerScan` 预算约束**
（默认 64 条/轮，256 个目标约 0.8 秒铺回来），零突发、零卡顿。

**举着扫描仪期间不做 ②**（那期间引擎自己在管管理器，避免和原版抢），
放下那一刻已经有 ① 兜住。

### 13.3 新增配置 / 日志

| 项 | 值 |
| --- | --- |
| INI `ResyncOnScannerClose` | `1`（默认开）。0 = 关掉这条自愈（回到「按两下 F8」的行为，仅用于对照） |
| 启动日志 | `config: … autoEnsure=1 resyncOnScannerClose=1` |
| 触发日志 | `native outline: 引擎侧高亮疑似丢失（MonocleMenu 关闭）-> N 个已挂目标转入「待重申」（按每轮 64 条预算重挂，累计 M 次）` |
| 统计行 | `… retired=N resync=M monocle=0/1`（`resync` 应该只在用扫描仪之后 +1） |

### 13.4 验收

1. **举扫描仪 → 扫描 → 放下**：高亮应该在 1 秒内自己回来（不再需要按 F8）；
2. 日志里出现一次上表那行「引擎侧高亮疑似丢失（MonocleMenu 关闭）」，
   且统计行 `resync` 随之 +1；
3. `monocle=1` 只在你举着扫描仪的那些统计窗口里出现；
4. 反复开关扫描仪若干次、走动、换场景**不卡顿**（重挂是分批的，
   `timing2: sync` 不应出现大值）；
5. F8 开关照旧（关 = 立刻全灭，开 = 1 秒内铺满）。

### 13.5 顺带修正的一条待办

第十二节 12.5 的建议 ②（`OutlineRef` 幂等化：写表前先读状态、相等就跳过 `Set`）
**不要做了** —— 自愈正是靠「真的再 `Set` 一次」。若将来有人按那条建议去做，
「表里状态还对但管理器里已经没了」这种情况会被静默跳过，本 bug 会原样复活。

---

## 十四、v4.0：分类分色（每个类别一个状态）

### 14.1 机制

第三节说过状态值 `0..11` 分成 `0..1` / `2..3` / `7..8` / `9` 几组，各组配色不同。
v4.0 把「全局一个 `OutlineState`」换成**按物品类别分别指定状态**：

```
ClassifyBase(base)            // base form 类型 → Category（-1 = 不高亮）
  → Candidate.state = cfg.stateByCategory[Category]
  → SyncNativeOutline：OutlineRef(ref, candidate.state)
```

`OutlineEntry` 也记了挂上去时用的 `state`（**只作诊断**）。★ 摘除时仍然
**先读引擎状态表里的真实值**（`UnoutlineRef` 里的 `stateInTable`），不是读我们记的那个 ——
原版扫描仪可能把重叠目标改成别的状态，用我们记的值会摘不干净。这一点从 v2.3 起就没变。

类别划分（`Category` 枚举）与默认状态见 `docs/99` 的 v4.0 段 / INI 注释。

### 14.2 两个容易踩的点

1. **状态变了要先把旧的摘掉**：重申时若发现 `entry.state != candidate.state`
   （正常只在改过 INI 之后才会发生），必须先 `UnoutlineRef` 再挂新的。直接挂新的会让
   同一个引用同时留在**两个管理器**里 ⇒ 两层描边，而且旧的那层要到掉出选中集合才消失。
2. **管理器是按需创建的**：`OutlineRef` 里的 `EnsureManagerFor(state)` 一旦发现目标状态
   的管理器不存在，就会调用引擎自己的重建函数 —— 那个函数是**一次性建满 11 个**的
   （见第 2.5 节），所以第一次挂任何一个类别都会让 `CountLiveManagers()` 由 0 跳到 11。
   ★ 这会影响 v3.2 那个「管理器数量下跌 = 引擎清过表」的判据：启动初期是**上涨**，
   不触发，正常。

### 14.3 诊断（对着日志调颜色）

| 日志 | 含义 |
| --- | --- |
| `config: stateByCategory: loot=0, container=1, ...` | 启动时实际生效的「类别 → 状态」映射 |
| `native outline managers[install] = 11/11 alive \| stateByCategory: loot=0(ok) ...` | 每个类别的管理器是否已存在 |
| `category (本轮选中): loot=N, container=N, ...` | 每 5 秒统计：本轮选中的目标按类别各多少个 |

颜色不理想时**只改 INI** 的 `StateLoot`…`StateOther`（`0/1/2/3/7/8/9` 各试一遍最直观），
不用改代码、不用重新编译。

### 14.4 ★ 颜色到底存在哪（v4.0.1 追加逆向）

实测反馈「分类分色无效、颜色都一样」之后，把「引擎怎么取颜色」这一段挖到了底：

**两张每状态参数表**（都是 stride **`0xA0`**，同一个状态的 5 个字段各占 `0x20`）：

| 表 | RVA | 谁读它 | 用途 |
| --- | --- | --- | --- |
| 引用参数表 | **`0x5919B08`** | `0x17D4CD0`（把引用挂进管理器时） | `+0x00` |
| 管理器参数表 | **`0x591E088`** | `0x17D47B0`（建/刷新 HighlightManager 时） | `+0x00`=High、`+0x20`=Low、`+0x40`=插值除数 |

关键指令（实证）：

```
; 挂引用时（0x17D4CD0）
017D4E2C  lea rax,[rbx+rbx*4] ; rbx = 状态
017D4E30  shl rax,5           ; ×160 = 状态*0xA0
017D4E34  vmovd xmm0,[rax + r15 + 0x5919b08]      ; ← 取该状态的颜色 dword

; 建/刷新管理器时（0x17D47B0，循环 11 次，r14 每次 +0xA0）
017D48FE  vmovd xmm1,[r14 - 0x60]   ; 表+0x00 = High
017D492D  vmovd xmm0,[r14 - 0x40]   ; 表+0x20 = Low
017D495C  vmovss xmm0,[r15]         ; 脉冲相位
017D4961  vdivss xmm6,xmm0,[r14-0x20]
017D49B8… vinsertps ×3              ; 逐通道 lerp(High, Low, t) → 打包成 RGBA→字节
```

- dword 的字节序 = **`R,G,B,A`**（`vmovd` 后按 `&0xFF / >>8 / >>16 / >>24` 分别解 R/G/B/A）。
- 颜色不是常量：引擎在 High/Low 之间按脉冲相位插值，这就是原版那个「呼吸」效果。
- ★ **这两张表不是 0 初始化的** —— 进程启动的静态初始化函数就把原版默认配色写进去了
  （反汇编实证：`0xF2AD3E` 橙、`0xFFE872` 金、`0x695B11` 橄榄 …… 顺序与
  `aHighlightScannableOutlineColorHigh/Low_<变体>` 的 11 个变体对应）。之后再由
  `:Monocle` 那套设置刷新。
- ★ 所以「11 个状态本来就该是不同颜色」；如果实测所有类别一个颜色，最可能是
  **管理器建得太早**（早于设置刷新进参数表），而 `0x17D47B0` 对**已存在**的管理器是
  「只刷新参数」⇒ v4.0.1 在**进入世界后的第一次扫描**再调一次它（`RefreshOutlineParamsOnce`）。
- 诊断：`LogOutlineColors()` 把 11 个状态的 `ref / mgrHigh / mgrLow / alpha` 直接打进日志
  （启动时一次 `[install]`、进世界后一次 `[world-ready]`，两条对比即可判断）。

**可选的自定义颜色**（INI `ColorLoot=RRGGBB` …，默认留空 = 不改）：
就是把 RGB 写进上面两张表的 `+0x00` 与 `+0x20`（alpha 保留原值），再调一次 `0x17D47B0`
刷新管理器、并把已挂目标整批重申（引用参数是「挂的时候读一次」）。
⚠️ 这是写**引擎的全局表**，原版手持扫描仪用的是同一张表 ⇒ 同一个状态的原版描边颜色
会跟着变。这是「精确指定颜色」的唯一途径，所以做成可选项而非默认。

