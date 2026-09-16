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

