// ============================================================================
//  Starfield Always Scan - 核心逻辑（SAS_AlwaysScan.dll）
//
//  目标（见 AGENTS.md）：
//    ① 不装备手持扫描仪，也能一直有「扫描仪高亮」效果；
//    ② 高亮不再受原版那个「屏幕中央圆圈」限制，改成玩家周围一个半径；
//    ③ 一个快捷键开关。
//
//  ★ v4.0 架构（旧方案已整体删除，见下）：
//    · **本文件（C++ / SFSE）**：从头到尾一个人干完 —— 遍历玩家所在 cell 的引用数组
//      （引擎自带的引用数组，几千个引用只有微秒级）、按「距离 + base form 类型」
//      筛出该亮的对象、然后**直接驱动引擎原生的 outline 高亮**（和原版手持扫描仪
//      走的是同一套渲染，见 docs/03）。
//    · **Papyrus 侧（SAS_Bridge.psc）**：只剩两件事，且都跟「高亮」无关 ——
//      ① 老存档里 v2.4~v3.0 留下的面包屑珠子做一次性清理；
//      ② 按 DLL 写进 `SAS_Notify` (GLOB) 的标记弹一条 HUD 提示（开/关）。
//      它**不再参与任何视觉**（EFSH / 信箱 / 游标全部删除）。
//
//  为什么还留着 ESM 与这个脚本（而不是变成纯 DLL）：
//    老存档里 SAS_AlwaysScanQuest 的脚本实例是**按 FormID 归档**的。一旦 quest 的
//    FormID 位移，那个实例就对不上，`GuideArrayReady` 读回来永远是 False ⇒
//    一次性清理不会再跑 ⇒ 老存档地上的珠子永久残留。
//    所以 0x800..0x806 那几条旧记录**保持创建顺序不动**（它们现在只是 FormID 占位），
//    新记录一律**追加在最后**（0x807 SAS_Notify）。见 build_sas.pas 顶部说明。
//
//  几个「不要再踩」的坑（全部来自上一代项目的实测）：
//    ① **绝不缓存 BSTArray 的 data()/capacity()**：清表可能释放/搬移缓冲，
//       缓存下来就是悬空指针，往那儿写 8 字节 → HEAP_CORRUPTION 延迟崩溃。
//       本文件每次现读，运行期零缓存。
//    ② **只看主线程**：SFSE 的 AddPermanentTask 挂在 Command_Process 上，
//       读档期间加载线程也会调它；非主线程直接返回。
// ============================================================================

#include "PCH.h"

#include "AlwaysScan.h"

#include "RE/F/FormTypes.h"
#include "RE/N/NiAVObject.h"
#include "RE/N/NiPoint.h"
#include "RE/N/NiSmartPointer.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESGlobal.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/T/TESObjectREFR.h"
#include "RE/U/UI.h"

#include "SasDisplayCases.h"  // ★ v4.9：展示柜（Display Case）容器白名单（脚本生成）

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SAS
{
	namespace
	{
		// ====================================================================
		// 常量
		// ====================================================================
		// 官方脚本 HumanHeight 4.0 ≈ 1.2 米 → 1 米 ≈ 3.4286 单位（上一代项目实测标定）
		constexpr float kUnitsPerMeter = 3.4286f;

		// ★ v4.0：旧方案（EFSH + FormList 信箱 + 游标 + Epoch）已整体删除，
		//   本 MOD 现在**只用一条 DLL → Papyrus 的通道**：一个 GLOB。
		//
		//   0x807 SAS_Notify —— DLL 写（1 = 刚打开 / 2 = 刚关闭），桥脚本轮询到之后
		//   弹一条 HUD 提示并立刻归零。**只用于游戏内提示**，不参与任何逻辑。
		constexpr std::uint32_t kLocalNotify = 0x807;
		// 为什么不用引擎的 `RE::DebugNotification`：commonlibsf 里它的 REL::ID 是 0
		// （未移植；实测该 ID 在 1.16.244.0 的 Address Library 里指向的不是它），
		// 调用等于 call 到模块基址。所以走「GLOB + 脚本轮询」这条零风险的路。

		// 运行时前缀由 quest 的 FormID 高字节推出（quest 的 EDID 实测可查）。
		constexpr const char* kBindQuestEdid = "SAS_AlwaysScanQuest";

		// ★ 0x800..0x806 是旧方案的记录与桥任务，**保留在 ESM 里不动**（只是占位）：
		//   0x800 EFSH SAS_HighlightFXS / 0x801 FLST SAS_OpList / 0x802 GLOB SAS_OpCursor
		//   0x803 FLST SAS_StopList      / 0x804 GLOB SAS_StopCursor
		//   0x805 GLOB SAS_Epoch         / 0x806 QUST SAS_AlwaysScanQuest
		//   理由见文件头「为什么还留着 ESM 与这个脚本」。

		// ================================================================
		// ★★★ 绝不能信 commonlibsf 声明的 TESObjectCELL 成员偏移 ★★★
		//
		// 2026-09-16 实锤（转储 Starfield_09-16-09-54.dmp）：
		//   载入存档后第一次扫描 → 崩在 SAS_AlwaysScan.dll+0x10E20，
		//   符号化成 AlwaysScan.cpp:515 = `cell->ForEachReference(...)`，
		//   异常码 0xC0000005、访问地址 0x0。
		//
		//   反汇编这条指令流（就是 ForEachReference 的内联展开）：
		//       lea rbx, [rdi + 0x128]          ; BSAutoReadLock(lock)
		//       mov rsi, [rdi + 0x90]           ; references._data
		//       mov eax, [rdi + 0x88]           ; references._size
		//       lea rdi, [rax*8 + rsi]
		//       cmp qword ptr [rsi], 0          ; ← 这里读 [nullptr] 崩
		//   也就是说**编译器算出来的**偏移是 references@0x88 / lock@0x128，
		//   而**真实游戏**是 references@0x80 / lock@0x120（上一代项目 v20/v22/v27/v33
		//   反复实测；本文件遇到的坏值 size=0x467E0000 也正是一个「指针低 32 位」，
		//   与上一代 v20 记录的 size=0x1EDE0000 是同一现象）。
		//
		//   根因：TESObjectCELL 里 cellFlags 用的 REX::TEnumSet<> 在声明中占的尺寸
		//   与引擎里不同，导致它**之后的所有成员整体位移 +8**
		//   （sizeof(TESObjectCELL)==0x150 仍然成立，所以 static_assert 抓不到）。
		//   这与本项目已修过的 BGSListForm::arrayOfForms(0x30→0x38)、
		//   TESGlobal::value(0x40→0x48) 是**同一类**问题。
		//
		//   ⇒ 规则：TESObjectCELL 的成员一律**按下面的实测偏移手工读**，
		//     永不调用 cell->ForEachReference() / cell->IsAttached()。
		// ================================================================
		// 候选偏移（按优先级）：
		//   0x80 = 上一代项目 v21~v38 反复实测的真实值（他们的 mod 长期跑这个值）；
		//   0x88 = commonlibsf 声明算出来的值（本轮已用 static_assert 钉死）。
		// 只用「形状校验」说话：谁通过就用谁，都不过就这一轮不扫描。
		constexpr std::size_t kCellRefsOffCandidates[] = { 0x80, 0x88 };
		// 形状校验的抽查样本数（每个样本一次 VirtualQuery，只在候选试探时用）
		constexpr std::uint32_t kRefsShapeSamples = 4;

		// BSTArray 内部布局：_size@+0 / _capacity@+4 / _data@+8（见 commonlibsf BSTArray.h）
		constexpr std::size_t kOffArraySize     = 0x00;
		constexpr std::size_t kOffArrayCapacity = 0x04;
		constexpr std::size_t kOffArrayData     = 0x08;

		// TESForm 基类内的偏移（这部分已被上一代实测确认是对的，不受上面那 +8 影响）
		constexpr std::size_t  kOffFormID    = 0x28;  // ★ v4.3：校验「读到的条目是不是真物品」要用
		constexpr std::size_t  kOffFormType  = 0x2E;
		constexpr std::uint8_t kFormTypeREFR = static_cast<std::uint8_t>(RE::FormType::kREFR);
		constexpr std::uint8_t kFormTypeACHR = static_cast<std::uint8_t>(RE::FormType::kACHR);

		// ================================================================
		// ★ v4.2：尸体 / 搜空（判死 + 判空）
		// ================================================================
		// 【判死】「预先放置的尸体」在数据里的形态（离线实证：扫全量 Starfield.esm，
		//   脚本 out/achr_flags_scan.py，1.16.244.0）：
		//     · ACHR 记录 flags 0x00000200 'Starts Dead'        —— **2283 条**（占 9530 个 ACHR 的 24%）
		//       （其中 1044 条同时带 0x800 'Initially Disabled'，会被 IsDisabled() 跳过）
		//     · ACHR 记录 flags 0x00002000 'Starts Unconscious' —— 193 条（默认**不当尸体**，见下）
		//   运行时还有第三个信号：`Actor::boolBits @+0x208` 的 kDead 位 —— 打死敌人时由
		//   引擎的战死路径置位（上一代项目用 tools/re/scan_disp.py 反查过
		//   `or dword ptr [reg+0x208], 0x800` / Resurrect 清位，语义吻合）。
		//   三者**取或**：任一成立就是「可搜刮的尸体」。
		//
		//   ★ 为什么「Starts Dead」这个**记录标志**也要看：它是静态数据，
		//     不依赖引擎在运行期有没有把 kDead 位置上（上一代项目实测过一具
		//     明确可搜刮、却没读到 kDead 位的身体 —— 日志 `corpse=21/0`）。
		//   ★ 「Starts Unconscious」（0x2000）**默认也算尸体**（v4.2.1 起，用户要求）：
		//     离线统计这 193 条引用的 26 个唯一 base，**22 个是炮塔 / 机器人的报废体**
		//     （`LvlRobotModelA_*` / `LvlTurretShort_*` / `LvlTurretCompact` /
		//      `LvlTurretQuadrapod` / `LvlMiniBotA` / `JasmineRobot` / `LvlSecurity_UC` …）
		//     —— 就是玩家说的「炮塔 / 机器人尸体」，能搜刮；
		//     代价是另外 4 个活物 base（MS01 的两个伤员、一个 UC 平民、一只 swarmer）
		//     也会被点亮 —— 它们倒地时同样能搜刮，观感上可接受；
		//     真要关掉：INI 里 `CorpseUnconscious=0`（不用换 DLL）。
		constexpr std::size_t   kOffFormFlags = 0x20;      // TESForm::formFlags（u32）
		constexpr std::uint32_t kFormFlagStartsDead        = 0x00000200u;  // ACHR: Starts Dead
		constexpr std::uint32_t kFormFlagStartsUnconscious = 0x00002000u;  // ACHR: Starts Unconscious
		// ★★ v4.5：TESForm 记录标志 bit2 = **Non-Playable（0x04）** = 「玩家拿不走的东西」
		//   —— 这是「搜空还不熄灭」的**真根因**（2026-09-18 实测取证 + 离线对照）：
		//
		//   ① `loot probe` 把尸体的库存条目逐条摊开后，残留物**无一例外**全是
		//      `Spacesuit_Assault_01_NOTPLAYABLE`（0x192294）/
		//      `Spacesuit_Assault_Backpack_01_NoBoostpack_NOTPLAYABLE`（0x12AAC6）/
		//      `Spacesuit_Assault_Helmet_01_NOTPLAYABLE`（0x192299）/
		//      `Clothes_ScienceLabTec_CORPSE_FROZEN_NOTPLAYABLE`（0x75796）…
		//      —— 这类「NPC 穿在身上的隐形装备」**在搜刮面板里不显示**；
		//   ② 离线对照（同一件装备的两个版本）：
		//        玩家版 `Spacesuit_Assault_01`          0x2265AD  flags=0x40  ← 不含 0x04
		//        玩家版 `Spacesuit_Assault_Backpack_01` 0x169F59  flags=0x40  ← 不含 0x04
		//        NPC 版 `..._NOTPLAYABLE`               0x192294  flags=0x44  ← 含 0x04
		//        NPC 版 `..._NoBoostpack_NOTPLAYABLE`   0x12AAC6  flags=0x44  ← 含 0x04
		//   ③ 可拿物品实测都是 **0x00**：`Credits`(MISC 0x0F)、`Digipick`(MISC 0x0A)、
		//      `Ammo777mm`(AMMO 0x4AD3E)、`Food_ButchersBest_Veal`(ALCH 0x2C7245)。
		//
		//   ⇒ 玩家把面板里的东西拿光后面板显示「空」，但库存里这些 0x04 条目的
		//     count 仍然 > 0 ⇒ `RefLootState` 返回「有东西」⇒ **描边永不熄灭**
		//     （预置尸体与打死的敌人完全一样，与用户两次实测描述一字不差）。
		//   ⇒ 判空时**跳过**这类条目：`object->formFlags & 0x04` ⇒ 不算「有东西」。
		//     回退开关：INI `SkipNonPlayableLoot=0`（不改判定，只关这一条）。
		constexpr std::uint32_t kFormFlagNonPlayable = 0x00000004u;
		constexpr std::size_t   kOffActorBoolBits = 0x208;                 // Actor::boolBits（u32）
		constexpr std::uint32_t kActorDeadBit     = 1u << 11;              // Actor::BOOL_BITS::kDead
		// 诊断：每个会话最多打几条尸体探针（一个 cell 也就几十个 Actor，够对号入座）
		constexpr std::uint32_t kCorpseProbeMax = 12;

		// ================================================================
		// ★★ v4.4：引擎自己的「生命状态」枚举 —— 判死的**权威判据**
		// ================================================================
		// 起因：用户 v4.3 实测两条 ——「打死的敌人（有东西）不亮」「拿空的尸体还亮」。
		//   日志里 `kDead位=0` 全程为 0（450 次尸体判定里没有一个读到 kDead 位），
		//   ⇒ 与上一代项目 v35 的实测完全一致：**引擎对「可搜刮的倒地者」不置 kDead 位**。
		//
		// 2026-09-18 反汇编实证（Starfield.exe 1.16.244.0，脚本 `out/probe_literal*.py`）：
		//   `[Actor + 0xF8]` 是一个 u32，**bits 17..20（掩码 0x1E0000）就是一个 4 位枚举**，
		//   而 Papyrus 三个 native 判死函数读的全是它：
		//
		//   · `IsUnconscious()`  实现 @ RVA 0x1FD1470：
		//       `mov eax,[r8+0xF8]; and eax,0x1E0000; cmp eax,0x60000; sete al`
		//       ⇒ **(v == 3) 即昏迷**
		//   · `IsDead()`         实现 @ RVA 0x1FC7FB0 → `jmp qword ptr [rax+0x868]`
		//       （= Actor vtable slot **0x10D**，实现 @ RVA 0x18E4540，参数 dl=1）：
		//       严格的 `dl=1` 分支：`v ∈ {1(0x20000), 2(0x40000), 5(0xA0000)}` ⇒ 死
		//       宽松的 `dl=0` 分支：再加 `7(0xE0000)`
		//   · `IsBleedingOut()`  实现 @ RVA 0x1FC7F50：
		//       `v ∈ {7(0xE0000), 8(0x100000)}` ∨ `middleHigh(+0x59C) != 0` ⇒ 出血（返回 1/2）
		//
		//   （这三个 native 的注册点在同一段代码里：`lea r9,[rip+实现]` +
		//     `lea rdx,[rip+名字]` + `call`，名字池在 .rdata 0x4D12390 一带。）
		//
		// ⇒ 从此判死**不再只看 kDead 位**（它只是「引擎某一时刻的缓存」），
		//   而是直接读引擎用来判死的那个枚举 —— 三路取或：
		//     死（1/2/5） ∨ 昏迷（3） ∨ 出血（7/8） ∨ kDead 位 ∨ Starts Dead 标志
		//   其中「昏迷 / 出血」共用 INI 开关（`CorpseUnconscious` / `CorpseBleedout`），
		//   万一将来发现某个状态不该亮，改 INI 即可，不用换 DLL。
		//
		//   ★ 为什么 5（0xA0000）也算死：它是引擎 `IsDead()` 严格分支的成员；
		//     7（0xE0000）只在宽松分支算死，但 `IsBleedingOut` 认它 ⇒ 归到「出血」开关下。
		constexpr std::size_t   kOffActorLifeState   = 0xF8;       // [Actor+0xF8]：lifeState 所在 u32
		constexpr std::uint32_t kActorLifeStateMask  = 0x1E0000u;  // bits 17..20
		constexpr unsigned      kActorLifeStateShift = 17;
		// 枚举值（引擎三函数实证）
		constexpr std::uint32_t kLifeStateDeadA      = 1;  // 0x20000  ┐
		constexpr std::uint32_t kLifeStateDeadB      = 2;  // 0x40000  ├ IsDead(dl=1) ⇒ 死
		constexpr std::uint32_t kLifeStateDeadC      = 5;  // 0xA0000  ┘
		constexpr std::uint32_t kLifeStateUnconscious = 3; // 0x60000    IsUnconscious ⇒ 昏迷
		constexpr std::uint32_t kLifeStateBleedA     = 7;  // 0xE0000  ┐ IsBleedingOut ⇒ 出血
		constexpr std::uint32_t kLifeStateBleedB     = 8;  // 0x100000 ┘

		// ================================================================
		// ★ v4.3（诊断）：Actor 的「AI 进程 → 状态对象」链路
		// ================================================================
		// 背景：v4.2 的判死只看 `boolBits.kDead` + 两个记录标志，而上一代项目
		//   （`高亮物品` v35）实测过「引擎根本没给某具可搜刮的身体标 kDead」的案例
		//   —— 2026-09-18 用 `out/scan_actor_vfns.py` 在 Actor 的 vtable 里扫出：
		//     `mov rax, [rcx + 0x228]`            → currentProcess（AIProcess*）
		//     `mov rcx, [rax + 0x10]`             → 进程里的状态对象（**可能为 null**）
		//     `cmp dword ptr [rcx + 0x264], 3/4/7` → 一个「多值状态枚举」（很像生命状态）
		//   但 0..7 各是什么语义**还没有实证**（不能靠猜），所以本轮只把它做成
		//   **探针**（`actor probe:` 行）——进游戏对「活人 / 该亮的尸体 / 该亮没亮的
		//   尸体」各看一眼，用数据定语义之后再进判定。★ 探针是纯内存读 + 形状校验，
		//   读不到就打 `-`，绝不参与过滤。
		constexpr std::size_t   kOffActorProcess = 0x228;  // Actor::currentProcess（AIProcess*）
		// ★ v4.4：`IsBleedingOut()` 的第二个条件 —— `AIProcess::middleHigh(+0x08) + 0x59C`
		//   是个 bool（反汇编 0x1FC7F50 实证）。只作**探针**，先不进判定：
		//   它是「出血动画中」的瞬时标志，怕在活人身上短暂为真（红线是「活人一个都不亮」）。
		constexpr std::size_t   kOffMiddleHigh  = 0x08;
		constexpr std::size_t   kOffMhBleedFlag = 0x59C;
		// 每个会话最多打几条「半径内所有 ACHR（不论死活）」的探针。
		//   比 corpse probe 宽：那个只覆盖「已判定为尸体」的，这个覆盖全部 ACHR ——
		//   「该亮没亮」的样本（被判成活人）只有这个探针能看到。
		constexpr std::uint32_t kActorProbeMax = 32;

		// ★ v4.4：每个会话最多打几条「ACHR 判决发生变化」的探针（活人→尸体、尸体→活人…）。
		//   用途：用户打死了敌人之后，「那一刻」的状态会被立刻记下来 ——
		//   一次实测就能定死「引擎到底给没给它置位 / lifeState 到底变成什么」。
		constexpr std::uint32_t kActorChangeProbeMax = 32;
		// ★ v4.4：每个会话最多打几条「库存明细」探针（只打近距离的尸体）。
		//   用途：「拿空还亮」的根因 —— 把判 notEmpty 的那具尸体身上的**残留条目**
		//   （object / 类型 / count / item flags）直接打出来。
		constexpr std::uint32_t kLootProbeMax = 16;
		// 库存明细探针只对「玩家多近」的目标打（米）。
		constexpr float kLootProbeRangeMeters = 20.0f;

		// ★★ v4.8 诊断：`cont probe:` —— 容器「判空链路快照」探针（背景与用法见
		//   ContProbe 函数顶部的长注释）。这三个常量控制它的数量与范围：
		//     kContProbeMax          —— 本会话最多打几条（0 = 关掉这组探针）
		//     kContProbeChangeMax    —— 同一个引用在「首次快照」之外最多再打几条
		//       （**判决变化**时各一条）。★ 首次快照不计入这个上限 —— 否则近处容器一多，
		//       额度全被「首次」吃掉，用户开关箱子的那两条关键记录就打不出来了。
		//       而「打开 → 关掉」正好是 2 次变化 ⇒ 上限 2（共最多 3 条/引用）。
		//     kContProbeRangeMeters —— 只对这么近的容器打（米）★ 比 loot probe 近：
		//       用户就站在那个箱子前面，近一点能少浪费额度（营地场景容器很多）
		constexpr std::uint32_t kContProbeMax         = 64;
		constexpr std::uint32_t kContProbeChangeMax   = 2;
		constexpr float         kContProbeRangeMeters = 15.0f;

		// ================================================================
		// ★★ v4.9：「展示柜（Display Case）」容器 —— 判空规则对它们不适用
		// ================================================================
		// 起因（用户实测）：*「武器箱关闭的时候没有高亮，只有打开的时候才开始高亮」*
		//   —— 两张截图：关着的武器箱不亮；一打开（搜刮界面）箱体橙描边 + 箱内
		//   弹药/武器蓝描边（这些颜色就是本 MOD 的 container / loot 两色）。
		//
		// 取证（2026-09-19 11:27~11:29 的用户日志，Kreet，v4.8 的 `cont probe:`）：
		//     11:27:09 关着   ref=0033FCCA base=00246224  inv=ok size=0
		//     11:29:22 打开   size=2
		//                     [i=0 ft=31 id=0004AD3E n=1 c=8 fl=20]
		//                     [i=1 ft=30 id=00028A02 n=1 c=1 fl=20]
		//     11:29:29 关上   size=0
		//   · `fl=0x20` = `BGSInventoryItem::Flag::kTemporary`（见
		//     commonlibsf `RE/B/BGSInventoryItem.h`，常量 kInvItemFlagTemporary）
		//     —— 这两条是**临时条目**：引擎只在搜刮界面打开期间把展示柜的内容
		//     「投影」进 `inventoryList`，关掉界面就收走。
		//     ⇒ **关闭状态下判空读到的 size=0 不是「空」**，而是「内容根本不在
		//     库存里」。判空逻辑本身没有错，错的是「对这类容器用它」。
		//   · 离线反查 base（`out/probe_case.py` dump Starfield.esm）：
		//     `00246224` = `Loot_Display_WeaponsCase_Rifles_Common`，记录里带
		//     `BFCB "BGSDisplayCase"` + `DCSD`/`DCED` —— **展示柜组件**。
		//   · 全量统计（`tools/re/gen_display_cases.py`）：707 个 CONT 里**恰好
		//     118 个**带该组件（武器箱 / 武器架 / 头盔架 / 背包架 / 数据板架 /
		//     前哨展示柜…，含 `_EMPTY` 变体）⇒ 数量可控，生成**静态白名单**
		//     （`SasDisplayCases.h`，产物入库）。
		//
		// ⇒ 判定：base 命中白名单（或运行期学习集合，见下）的**容器**不因
		//   「读到空」被跳过 ⇒ 关闭状态照常亮。INI `SkipDisplayCaseEmpty=0` 可
		//   退回旧行为（只在排查用）。
		//
		//   ★ 运行期学习（兜底第三方 mod 新增的展示柜）：任何容器只要在打开期间
		//     读到过 `kTemporary` 条目，就把它的 base 记进内存集合
		//     （`State::displayCaseRuntime`）—— 之后这个 base 的所有实例都享受
		//     同等待遇。会话级（不落盘）；上限见 kDisplayCaseRuntimeMax。
		//
		//   ★ 如实记录的副作用：展示柜**被拿空后关着也会亮**（关闭状态读不到
		//     内容，与「没打开过」在数据上不可区分）；「打开时」照旧按实际投影
		//     判空（打开着被发现是空的 ⇒ 熄灭）。
		constexpr std::uint32_t kDisplayCaseRuntimeMax = 256;  // 运行期学习集合上限（防呆）

		// 【判空】容器 / 尸体的「库存列表」——
		//   `TESObjectREFR::inventoryList` 是 `BSGuarded<BGSInventoryList*, BSReadWriteLock>`，
		//   而 commonlibsf 头文件里 BSGuarded 的成员顺序标着 "??"（data 在前还是锁在前
		//   不确定），所以**两个候选偏移都探测**（0xA0 / 0xA8），谁通过形状校验就用谁。
		//   ★ 绝不调用 `BSGuarded::LockRead()` / `ForEachInventoryItem()`：那会按
		//     「猜的成员顺序」去加锁，猜错就是往真正的数据指针里写线程号（灾难）。
		//     我们只**读指针**，然后自己按形状校验解引用。
		//   ★ 标定不通过 → 永久降级（不判空，容器/尸体照常亮），
		//     绝不让没验证过的偏移进热路径（上一代 v32 的教训）。
		constexpr std::size_t   kOffInvCand[2]{ 0xA0, 0xA8 };  // BSGuarded<BGSInventoryList*>::m_data 候选
		constexpr std::size_t   kOffInvData         = 0x28;    // BGSInventoryList::data（BSTArray）
		constexpr std::size_t   kOffInvItemSize     = 0x28;    // sizeof(BGSInventoryItem)
		constexpr std::size_t   kOffInvItemStacks   = 0x10;    // BGSInventoryItem::stacks（BSTArray<Stack>）
		constexpr std::size_t   kOffInvItemObject   = 0x00;    // BGSInventoryItem::object
		// ★ v4.4：BGSInventoryItem::flags（u32 @+0x20）—— 逐位语义见下面 v4.6 的常量。
		constexpr std::size_t   kOffInvItemFlags    = 0x20;
		// ================================================================
		// ★★ v4.6：「穿在身上的装备」也要跳过（`*_NOTPLAYABLE` 之外的第二个隐形容器）
		// ================================================================
		// 位定义（commonlibsf `RE/B/BGSInventoryItem.h` 的 `BGSInventoryItem::Flag`）：
		//   bit0..2 = kSlotIndex1/2/3（合起来是 kSlotMask）—— **非 0 = 这件东西正被穿着 /
		//             装备着**（头文件里的 `IsEquipped()` 就是 `flags.any(kSlotMask)`）；
		//   bit3    = kEquipStateLocked（装备状态锁定）；
		//   bit5    = kTemporary（引擎临时条目）。
		//
		// ★ 起因：v4.5 实测（用户原话）「**场景预置尸体拿空了会熄灭，但活体敌人的尸体
		//   还是不会**」。v4.4/v4.5 的日志（`loot probe:` / `actor probe (changed):`）
		//   把残留物摊开成三类：
		//     ① `ff` 含 0x04 的 `*_NOTPLAYABLE` 隐形装备 —— v4.5 已跳过；
		//     ② **`fl` 非 0（装备槽位被置位）而 `ff` 不含 0x04 的普通装备**：
		//        实测样本 `Spacesuit_CrimsonFleet_Assault`(0x66821) /
		//        `..._Helmet`(0x66822) / `..._Backpack`(0x66825)，离线 `esmrec.py`
		//        逐条查过 = **记录标志 0x40 的玩家版**，可它们在尸体库存里 `fl=1`
		//        （= 还穿在身上）；这类只有「活着被打死」的尸体才会有
		//        （预置尸体用的多是 `*_NOTPLAYABLE`）⇒ 正是上面那句反馈的数据形态；
		//     ③ `fl=0` 的散落物（Credits / 弹药 / Digipick / 掉落的护甲）—— **真能拿**。
		//
		//   ★ 旁证（用户本次下载的第三方 mod `SimpleImmersiveLooting`，Nexus 12677）：
		//     它的整个卖点就是 **「扒取装备」= `Actor.UnequipAll()`** —— 先把尸体身上
		//     穿着的装备**卸下来**，面板里才拿得走；并且它把 GMST
		//     `fEquippedArmorChanceToDrop` 从 **0.1 覆盖成 0.0**（= 死亡时引擎**不再
		//     自动掉落**身上装备）。⇒ 引擎的模型就是：**装备着的（fl≠0）不算可搜刮物，
		//     只有被「掉落 / 卸下」的（fl=0）才进搜刮面板**。
		//     `fl=0` 的护甲样本日志里也有：`[ft=22 id=002470D1 … fl=0 ff=9 np=0]`。
		//
		//   ⇒ 判空时把「装备中」的条目也算作「拿不走」（默认跳过；`SkipEquippedLoot=0`
		//     可退回旧行为）。跳过之后两件事自动一致：
		//       · 打死的敌人（穿着玩家版宇航服 + 面板拿空）⇒ 判空 ⇒ 熄灭 ✅；
		//       · 用 `SimpleImmersiveLooting` 的「扒取装备」卸下后 `fl` 归零 ⇒ 重新
		//         算作「有东西」⇒ 照常亮，直到你把它们拿走 ✅。
		constexpr std::uint32_t kInvItemFlagSlotMask    = 0x07;  // kSlotIndex1|2|3（非 0 = 装备中）
		constexpr std::uint32_t kInvItemFlagEquipLocked = 0x08;  // kEquipStateLocked
		constexpr std::uint32_t kInvItemFlagTemporary   = 0x20;  // kTemporary
		constexpr std::size_t   kOffStackSize       = 0x10;    // sizeof(BGSInventoryItem::Stack)
		constexpr std::size_t   kOffStackCount      = 0x08;    // BGSInventoryItem::Stack::count
		constexpr std::uint32_t kInvMaxItems        = 4096;    // 条目数超过它 = 一定读错了
		constexpr std::uint32_t kInvMaxStacks       = 64;      // 单个条目的 stack 数上限
		constexpr std::uint32_t kInvWalkItemsMax    = 128;     // 判空最多逐条走多少个条目
		// ★ v4.3：标定改成「跨轮累计 + 强样本」，不再「第一轮 6 个样本一锤定音」。
		//   起因：v4.2 的标定只在**前 1500 个引用**里找**容器**样本，凑够 6 个就
		//   下结论；如果玩家的第一个场景容器少 / 容器排得靠后 ⇒ 直接 FAILED 且
		//   **永久降级**（容器 / 尸体拿空了也不熄灭）。现在：
		//     · 样本来源放宽到「任何拥有库存的引用」（CONT 优先，ACHR 也算）；
		//     · 每轮最多采样 kInvCalibPerRound 个（避免一轮里烧太多 VirtualQuery），
		//       累计到 kInvCalibNeedOk 票才采纳 ⇒ 换场景 / 走几步自然会攒够；
		//     · 采纳还要求至少 kInvCalibNeedStrong 个**强样本**（见 IsPlausibleFormPtr）——
		//       只有强样本才能证明「读到的条目真的是物品」，光靠指针可读太弱；
		//     · 只有累计到 kInvCalibMaxRounds 轮仍然不通过才永久降级（打 WARN）。
		constexpr std::uint32_t kInvCalibNeedOk     = 3;       // 至少 3 个样本通过形状校验
		constexpr std::uint32_t kInvCalibNeedStrong = 1;       // 其中至少 1 个是「强样本」
		constexpr std::uint32_t kInvCalibMaxBad     = 2;       // 且坏样本不超过 2 个
		constexpr std::uint32_t kInvCalibPerRound   = 4;       // 每轮扫描最多采样几个引用
		constexpr std::uint32_t kInvCalibScanCap    = 4000;    // 单轮最多看多少个引用去找样本
		constexpr std::uint32_t kInvCalibFastRounds = 40;      // 前多少轮「每轮都采样」（之后降频）
		constexpr std::uint64_t kInvCalibSlowMs     = 4000;    // 降频后每多少毫秒采样一次（**永不放弃**）

		// ---- 偏移探针 ----
		// 这两个 static_assert 记录的是「commonlibsf 声明算出来的值」，它们和真实值
		// **差 8 字节**，正是本 bug 的根源（TESObjectCELL 的基类 TESHandleForm 在声明
		// 里比真实大 8 字节，导致其后所有成员整体后移）。
		// 哪天有人把 TESHandleForm 的声明修对了，这里会编译失败 —— 那是好事，
		// 提醒我们重新核对 kOffCellRefs，届时就可以恢复用头文件成员访问了。
		static_assert(offsetof(RE::TESObjectCELL, references) == 0x80 + 0x08,
			"commonlibsf 的 TESObjectCELL 偏移又变了：请重新核对 kOffCellRefs（真实应为 0x80）");
		static_assert(offsetof(RE::TESObjectCELL, lock) == 0x120 + 0x08,
			"commonlibsf 的 TESObjectCELL 偏移又变了：请重新核对（真实 lock 应为 0x120）");

		// ★ v4.2：TESForm 基类 / Actor 的成员偏移探针（判死那条路要用）。
		//   这几个成员都在 **TESForm 基类**或 Actor 自己里（不受上面那个 +8 位移影响），
		//   而且本项目已长期实测「formType @+0x2E / formID @+0x28」的读法是对的。
		//   commonlibsf 哪天改了声明这里会编译失败 ⇒ 提醒重新核对。
		static_assert(offsetof(RE::TESForm, formFlags) == 0x20,
			"commonlibsf 的 TESForm::formFlags 偏移变了：请重新核对 kOffFormFlags（应为 0x20）");
		static_assert(offsetof(RE::TESForm, formType) == 0x2E,
			"commonlibsf 的 TESForm::formType 偏移变了：请重新核对 kOffFormType（应为 0x2E）");
		static_assert(offsetof(RE::Actor, boolBits) == 0x208,
			"commonlibsf 的 Actor::boolBits 偏移变了：请重新核对 kOffActorBoolBits（应为 0x208）");

		// 合理性上限：一个 cell 的引用数不可能超过这个数（落到这里就是读到垃圾了）
		constexpr std::uint32_t kMaxRefsSanity = 200000;

		// 换 cell / 读档后先静置这么久再开始扫描（引擎此时还在重建世界）
		constexpr std::uint64_t kSettleAfterSceneChangeMs = 2000;
		// 引用数组长度连续两轮一致才认（长度突变 = 加载线程还在往数组里塞）
		constexpr std::uint32_t kRefsStableRounds = 2;

		// ================================================================
		// ★★ v4.7：外景连续性 —— 跨 cell 边界不再「整批熄灭」
		// ================================================================
		// 起因（用户实测 2026-09-19 + 当次日志实证）：
		//   「星球表面地图上，原本已经高亮的物体突然不亮了；行走时似乎也不扫描」。
		//   日志把真凶写在明面上 —— 玩家在 Kreet 表面来回穿越**两个相邻外景 cell**
		//   （`0032588C` EDID `LC003KreetBaseRoof` grid(2,1) 与 `0032588B` grid(2,0)，
		//   两条都是外景：CELL DATA=0x0002、有 XCLC），而本 MOD 每次
		//   `player->parentCell` 一变就当成「换场景」：
		//     ① `ResetForNewScene()` ⇒ **全部已挂高亮转入「待摘」**（当次日志 15 次
		//        `reset (cell changed) outline=20~111`，也就是每次跨边界全灭）；
		//     ② 静置 2 秒**完全不扫描**（`settleUntilMs`）；
		//     ③ 之后只把**新 cell 自己**的引用当候选（`ref->parentCell != cell` 直接跳过）
		//        ⇒ 边界对面那些还在眼前的引用**永远不亮**。
		//   ⇒ 外景里 cell 只是「世界的一块」：跨过边界走一步不是换场景。把内景
		//     （进门 = 载入画面）那一套照搬过来，观感就是「走着走着全灭、停下才铺满」。
		//
		// 判据（不依赖「cell 是不是外景」这种要读新偏移的信息）：
		//   **上一个 cell 还在「近期 cell 环」里 ⇒ 世界是连续的** ⇒ 走「连续过渡」：
		//     · 不整批摘（账本不动，谁真的离开半径就按宽限期自然淘汰）
		//     · 静置缩短到 kSettleOnCellCrossMs
		//     · 当前 cell + 环里其它 cell 一起参与扫描（见 Rescan / kRingCellMax）
		//   反过来：读档 / 进门这些经过载入画面的切换会把环清空（Tick 里那条
		//   「载入画面由开变关」），于是仍然走原来的「换场景」路径（整批待摘 + 2 秒静置）。
		//   回退开关：INI `ExteriorContinuous=0`（退回 v4.6 行为，只用于对照）。
		constexpr std::uint64_t kSettleOnCellCrossMs = 300;

		// 「近期 cell 环」：只装玩家真正待过的 cell。
		//   用途 ①：判断「这次换 cell 是不是连续过渡」（见上）；
		//   用途 ②：让**边界对面的引用**也参与扫描 —— 外景里玩家经常在两个 cell 之间
		//           来回走，只扫当前 cell 会让边界一侧的东西时亮时灭
		//           （当次日志：站在 `…8B` 时只有 20~27 个目标，隔壁 `…8C` 的 100 多个
		//            全灭，来回走一次就换一批）。
		//   淘汰：连续 kRingCellMissMax 轮形状校验不过（= 这个 cell 已卸载）→ 踢掉；
		//         环满 / 引用总数超过 kRingRefsCap → 淘汰最旧的。
		//   ★ 安全性：环里的 cell 每轮都要过 IsReadable + 形状校验，**并且**抽查
		//     「ref->parentCell == 这个 cell」（最强的一条自洽判据，垃圾数组过不了）。
		constexpr std::size_t   kRingCellMax     = 8;
		constexpr std::uint32_t kRingCellMissMax = 10;
		constexpr std::uint32_t kRingRefsCap     = 60000;

		// ★★ v4.7：引用数「轻微变化」的容差（治「行走时不扫描」的第二个来源）
		//   旧判据：`arr.size != lastRefsSize` ⇒ 立刻 `stableRounds = 0` 并 return，
		//   而 `stableRounds` 要连续 2 轮才放行 ⇒ **每次长度变化要跳过约 3 轮（≈600ms）**。
		//   外景 / 城市里走动时加载线程一直在流式增删引用，于是扫描被反复打断。
		//   现在：变化量 ≤ 这个容差就只记一笔（`streamMoves`）继续扫；超过（换场景 /
		//   第一次进世界的批量填充）仍然按突变处理（等稳定轮数）。
		constexpr std::uint32_t kRefsStreamJumpMax = 256;

		// ★★ v4.7：3D 复检 —— 引擎侧描边被静默丢掉之后自动重挂
		//   背景见 SyncNativeOutline 里的「4) 3D 复检」段。每轮最多复检这么多个已挂目标。
		constexpr std::uint32_t kVerify3DPerScan = 32;

		// ★ v4.7：Tick（主循环任务）间隔超过这么久就记一笔 —— 用来区分
		//   「扫描没发生」是「Tick 根本没被引擎调用」（暂停 / 载入 / 主线程忙）
		//   还是「Tick 调了但被早退挡住」。实测里出现过 39 秒一个 scan 都没有的窗口，
		//   当时两种可能分不出来（这就是加它的原因）。
		constexpr std::uint64_t kTickGapLogMs = 1500;

		constexpr std::uint64_t kStatsLogIntervalMs = 5000;
		constexpr std::uint64_t kBindWarnIntervalMs = 5000;

		// 偏航角 → 前向量的符号（见 Rescan 里 OnlyInFront 的说明）。
		// Gamebryo/NetImmerse 常规：yaw=0 面向 +Y，forward=(sin(z), cos(z))。
		constexpr float kYawForwardSign = 1.0f;


		// ====================================================================
		// 配置（Data\SFSE\Plugins\SAS_AlwaysScan.ini）
		// ====================================================================
		// ====================================================================
		// ★ v4.0：类别（决定用哪个 outline 状态 = 哪种配色）
		// --------------------------------------------------------------------
		// 分类只依赖 base form 的类型（`RE::TESForm::GetFormType()`），零成本。
		// 下标顺序 = INI 里 StateLoot / StateContainer / ... 的顺序。
		// ====================================================================
		enum class Category : std::uint8_t
		{
			kLoot = 0,   // 可拾取（进背包）：MISC/BOOK/ARMO/WEAP/AMMO/ALCH/INGR/KEYM/NOTE/SLGM
			kContainer,  // 容器：CONT
			kDevice,     // 可交互设备：ACTI / TERM
			kDoor,       // 门：DOOR
			kFlora,      // 植物：FLOR
			// ★ v4.1：其它 = **MSTT（MovableStatic，可移动静态物）**。
			//   这一类**绝大多数是不能拾取进背包的装饰物**（纸箱 / 桌椅 / 吧台 /
			//   飞船模块 …），原版扫描仪也不会高亮它们，所以**默认关闭**
			//   （`EnableOther=0`，见 Config::categoryEnabled 的完整说明）。
			kOther,
			// ★ v4.2：尸体（可搜刮的「身体」）。
			//   两种形态都归这一类：
			//     ① **ACHR 引用**（= Actor 对象）—— 打死的敌人、预先摆放的尸体；
			//     ② **base 是 NPC_ / LVLN 的普通 REFR** —— Starfield 用来摆
			//        「姿势固定的尸体道具」（它不是 Actor，不会动，只能搜刮）。
			//   判死/判活见 ClassifyRef()；活着的 NPC / 生物**一个都不亮**（红线）。
			//   容器 / 尸体都要「搜空即熄灭」——见 RefLootState()。
			kCorpse,
			kCount
		};
		constexpr std::size_t kCategoryCount = static_cast<std::size_t>(Category::kCount);

		constexpr const char* kCategoryName[kCategoryCount] = {
			"loot", "container", "device", "door", "flora", "other", "corpse"
		};

		// ★ v4.0.1：INI 没写自定义颜色时用的哨兵值（写 0 表示「用引擎原生配色」）
		constexpr std::uint32_t kColorUnset = 0xFFFFFFFFu;

		struct Config
		{
			bool          startEnabled    = true;
			int           hotkeyVk        = VK_F8;
			float         radiusMeters    = 50.0f;
			// ★ v2.3：米 → 游戏单位 的换算系数。默认沿用上一代项目实测标定的 3.4286
			//   （HumanHeight 4.0 ≈ 1.2 米）。它只影响「RadiusMeters 到底折合多少游戏单位」，
			//   所以做成 INI 可调：改完重进游戏即可，不用重新编译。
			//   日志里会同时打出「米」和「游戏单位」两个数，方便按实际观感校准。
			float         unitsPerMeter   = 3.4286f;
			int           maxTargets      = 256;
			int           scanIntervalMs  = 200;
			bool          logStats        = true;

			// --- 视觉层：原生 outline（见 docs/03-原生outline高亮实现.md）---
			// 1 = 只高亮玩家正前方的目标（水平夹角在 FrontFovDeg 之内）
			bool          onlyInFront     = true;
			float         frontFovDeg     = 110.0f;
			// 每隔多久向引擎重申一次已挂的高亮（毫秒）。0 = 不重申（默认）。
			// ★ v2.1 默认改成 0：高亮一旦挂上就一直有效（挂/摘是我们自己在管），
			//   周期性重申只会往引擎的高亮请求通道里灌重复请求，反而可能造成
			//   「描边一个一个慢慢出来」的观感。只有确认「高亮会莫名消失」时才打开。
			int           reassertMs     = 0;
			// 1 = 引擎的 12 个 HighlightManager 不存在时，自己调用引擎的
			//     「重建管理器」函数把它们建出来（不开扫描仪时引擎就不会建）。
			bool          autoEnsureManagers = true;

			// --- ★ v3.2：用原版手持扫描仪之后**自动重挂** ---
			// 1 = 玩家放下扫描仪（`MonocleMenu` 关闭）时，把已挂的高亮整批转入
			//     「待重申」，由 SyncNativeOutline 按每轮预算重新挂一遍。
			//   ★ 为什么需要（实测症状，见 docs/03 第十三节）：
			//     引擎在扫描仪的生命周期里会拆掉 Monocle HUD（RVA 0x17D4B30：
			//     **销毁 11 个 HighlightManager + 清空「引用→状态」表**），
			//     于是 g_state.outlined 里的记录全部变成「悬空记录」——
			//     我们以为还挂着、引擎那边其实已经没了。默认 ReassertMs=0 下
			//     没有任何自愈机制 ⇒ 表现为「用完原版扫描仪后高亮全灭，
			//     要按两下 F8（先关再开）才回来」。
			bool          resyncOnScannerClose = true;

			// --- ★ v2.2：治「轻微卡顿」的两个闸门 ---
			// 「掉队」宽限期（毫秒）：一个目标掉出选中集合后，先留着高亮这么久，
			// 到期还没回到集合里才真正摘掉。
			//   ★ 为什么需要它：走过去/转身时目标会在集合边缘反复进出，没有宽限期
			//     就是每 200ms 一轮的 Set/Remove 抖动，主线程被这些小调用磨出
			//     hitch（实测日志里 rmMiss 每秒涨 ~20 就是这个抖动）。
			//   期间如果它又回到集合里就什么都不用做（零引擎调用）。
			int           unhighlightGraceMs = 1500;
			// 单轮扫描最多向引擎发多少条「挂/摘」调用（0 = 不限）。
			// 换场景 / 读档一次会积压几百条待办，一口气做完就是一个长卡顿
			// （实测日志里出现过 18 秒的空档），这里摊到后面几轮里慢慢做。
			int           maxOutlineOpsPerScan = 64;

			// ================================================================
			// ★ v4.0：分类分色
			// ================================================================
			// 每个类别用一个 outline 状态（0..11；引擎按状态取不同配色/粗细）。
			// 状态语义（docs/03 第三节，从引擎算法反推）：
			//   0/1 = 通用（未扫描/已扫描）  2/3 = 可扫描组  7/8 = 另一组  9 = 追踪
			//   12 = 无高亮（管理器只有 0..10 共 11 个）
			// 下标 = Category，默认 2 蓝 / 9 橙 / 4 绿 / 10 红 / 5 绿 / 1 淡蓝白。
			// ★ 想换配色只改 INI 的 StateXxx；改完重进游戏生效。
			// ★ v4.0.2：默认值按「实测的引擎原生配色」挑的**视觉上真正区分得开**的 5 种色：
			//   2 蓝 / 9 橙 / 4 绿 / 0 青 / 5 绿 / 1 淡蓝白
			//   （v4.0.1 的默认 0/1/2/3 实测全是蓝色系，看起来「颜色都一样」——
			//     3 与 2 的 ref 色值完全相同。实测表见 docs/03 第十四节 14.5 / INI 注释。）
			// ★ v4.0.3：门从 0 青改为 10 红 —— 可拾取是 2 蓝，青/蓝对比太弱，红拉开最大。
			// ★ v4.2：尸体（下标 6）默认也是 **9 橙**（和容器同色，两者都是「搜刮目标」）；
			//   想区分开就改 INI 的 `StateCorpse`（可用的其它色见 INI 里那张实测配色表）。
			std::array<int, kCategoryCount> stateByCategory{ 2, 9, 4, 10, 5, 1, 9 };

			// ================================================================
			// ★ v4.1：每个类别一个「是否高亮」开关
			// ================================================================
			// 起因（用户实测反馈）：*「有些不能拾取进背包的物品也有蓝色边框」*，
			// 截图里被描边的是纸箱 / 桌椅 / 吧台 —— 它们在游戏数据里是 **MSTT
			// （MovableStatic，可移动静态物）**，这一类**绝大多数是不能拾取进背包的
			// 装饰物**（`CardboardBox*` / `Bar_*` / `IH_TableKit*` / 飞船模块 `SMOD_*` …），
			// 而且**原版手持扫描仪根本不会高亮它们**（它只亮能拿 / 能搜 / 能开 / 能采的）。
			//
			// 而真正能拾取进背包的杂物（`CoffeeMug01` / `Tool_Wrench01` / 各种玩具…）
			// 全是 **MISC**，走的是 `loot` 类 —— 这两类在数据里泾渭分明，所以
			// 「不亮 MSTT」既符合原版观感，也正好满足「不可拾取的不亮」。
			// ⇒ 默认 `EnableOther=0`（关掉 kOther = MSTT）；其余 5 类都是真目标，默认开。
			//
			// ★ 想恢复高亮 MSTT（或只想留其中几类）就改 INI 的
			//   `EnableLoot / EnableContainer / EnableDevice / EnableDoor /
			//    EnableFlora / EnableOther`，改完重进游戏生效。
			std::array<bool, kCategoryCount> categoryEnabled{ true, true, true, true, true, false, true };

			// ★ v4.0.1：可选的「自定义类别颜色」（INI 里写 ColorLoot=RRGGBB 之类）。
			//   kColorUnset = 不覆盖，完全用引擎那个状态的原生配色。
			//   设了就把 RGB 写进引擎的两张每状态配色表（alpha 保持原值），
			//   再让引擎刷新管理器 + 整批重挂（详见 ApplyColorOverrides）。
			std::array<std::uint32_t, kCategoryCount> colorOverride{
				kColorUnset, kColorUnset, kColorUnset, kColorUnset, kColorUnset, kColorUnset, kColorUnset
			};

			// ★ v4.0：按热键切换时弹一条 HUD 提示（DLL 写 GLOB → 桥脚本轮询）。
			bool          notifyOnToggle  = true;

			// ================================================================
			// ★ v4.2：尸体 / 搜空
			// ================================================================
			// 1 = 把「Starts Unconscious（0x2000）」的 ACHR 也当尸体点亮（**默认 1**）。
			//
			//   ★ v4.2.1 用户反馈后把默认从 0 改成 1：这一位在数据里的**主体是
			//     炮塔 / 机器人的报废体**（离线统计：193 条引用 / 26 个唯一 base，
			//     其中 22 个是 `LvlRobotModelA_*` / `LvlTurretShort_*` / `LvlTurretCompact` /
			//     `LvlTurretQuadrapod` / `LvlMiniBotA` / `JasmineRobot` / `LvlSecurity_UC` …），
			//     **它们就是玩家说的「炮塔 / 机器人尸体」，可以搜刮**；
			//   ★ 代价（如实记录）：另外 4 个 base 是活物 —— `MS01WoundedSoldier`、
			//     `MS01WoundedScientist`（任务里受伤倒地的两人）、`LvlCitizen_UC_Male`、
			//     `RL039_LvlSwarmerCritter`。它们被打倒 / 昏迷时点亮是合理的（能搜刮），
			//     万一有哪个站起来后还亮着，把这里设成 0 即可（不用换 DLL）。
			bool          corpseUnconscious = true;
			// 1 = 容器 / 尸体「库存为空」就不高亮 —— 搜空即熄灭（默认 1）。
			//   依赖 `inventoryList` 偏移的运行时标定；标定失败会自动降级成旧行为
			//   （不判空，容器 / 尸体照常亮），日志里有明确警告。
			bool          skipEmptyLoot     = true;

			// ★ v4.3（诊断）：每个会话最多打几条 `actor probe:`（半径内**所有** ACHR
			//   的判决明细，含被判成活人的）。用途：用户报「某具尸体该亮没亮」时，
			//   一次日志就能看到它被判成了什么、boolBits / formFlags 是什么。
			//   设 0 = 关掉这组探针（只留 corpse probe）。
			int           actorProbeMax     = static_cast<int>(kActorProbeMax);

			// ★ v4.3：库存指针是 **null** 时怎么算（默认 1 = 算「空」⇒ 不亮）。
			//   依据（2026-09-18 反汇编）：唯一分配 `BGSInventoryList` 的函数
			//   （`CreateInventoryList`）只有一个调用点（引用初始化的虚函数路径），
			//   没有「打开 UI 时才懒创建」的第二条路 ⇒ 没建 = 没东西 ⇒ 判空正确。
			//   ⚠️ 这是本轮**唯一**带假设的改动：万一实测发现「从没搜过的身体 / 容器
			//     因此不亮」，把它设 0 即可退回旧行为（null = 未知 ⇒ 照常亮）。
			bool          treatNullInvAsEmpty = true;

			// ================================================================
			// ★★ v4.4：判死改用**引擎自己的 lifeState 枚举**（背景见文件顶部常量区的长注释）
			// ================================================================
			// `CorpseLifeState`（默认 1）：读 `[Actor+0xF8]` 的 bits17..20，
			//   把引擎 `IsDead()` 认可的 {1,2,5} 当作「尸体」——**这是本轮修
			//   「打死的敌人不亮」的核心**（kDead 位不可靠，实测 450 次尸体判定里
			//   一个都没读到它）。设 0 = 退回只看 kDead 位 + Starts Dead 标志。
			bool          corpseLifeState   = true;
			// `CorpseBleedout`（默认 1）：把 lifeState ∈ {7,8}（引擎 `IsBleedingOut()`）
			//   也算「尸体」。**依据**：上一代项目 v35 的判定就是
			//   `IsDead() ∨ IsUnconscious() ∨ IsBleedingOut()`，用户实测确认
			//   「现在尸体会高亮了」⇒ 7/8（倒地出血）那一批是可搜刮的倒地者。
			//   设 0 = 严格模式（只认 1/2/5）。
			bool          corpseBleedout    = true;
			// ★ v4.4 诊断：`actor probe` 里那套链路（v4.3 的 life=）已被 lifeState 取代；
			//   这里控制「判决变化探针」的条数上限（0 = 关）。
			int           actorChangeProbeMax = static_cast<int>(kActorChangeProbeMax);
			// ★ v4.4 诊断：库存明细探针 `loot probe:` 的条数上限（0 = 关）。
			int           lootProbeMax      = static_cast<int>(kLootProbeMax);
			// ★ v4.8 诊断：容器「判空链路快照」探针 `cont probe:` 的条数上限（0 = 关）。
			//   用途：用户报「武器箱关着时不亮、一打开就亮」—— 这个探针会在
			//   「第一次见到这个容器」和「判决发生变化」时各打一条库存明细，
			//   开关前后的两条一对比，原因就定死了（见 ContProbe 顶部的说明）。
			int           contProbeMax      = static_cast<int>(kContProbeMax);

			// ================================================================
			// ★★ v4.5：判空时跳过「非玩家物品」（记录标志 0x04）—— 默认 1
			// ================================================================
			// 背景与实证见常量区 `kFormFlagNonPlayable` 的长注释（一句话版）：
			//   残留物全是 `*_NOTPLAYABLE` 的 NPC 隐形装备（玩家拿不走、面板也不显示），
			//   把它们算作「有东西」会让**搜空后永不熄灭**（用户两次实测的现象：
			//   预置尸体与打死的敌人一样）。
			//   设 0 = 退回旧行为（把它们也算「有东西」，只在排查时用）。
			bool          skipNonPlayableLoot = true;

			// ================================================================
			// ★★ v4.6：判空时也跳过「正穿在身上的装备」（`BGSInventoryItem::flags`
			//   的低 3 位 kSlotMask ≠ 0）—— 默认 1
			// ================================================================
			// 背景 / 实证见常量区 `kInvItemFlagSlotMask` 的长注释（一句话版）：
			//   引擎只在物品被「掉落 / 卸下」（`fl=0`）时才让它进搜刮面板；还穿在
			//   尸体身上的（`fl≠0`）面板不显示、玩家也拿不走，`count` 却永远 ≥ 1
			//   ⇒ v4.5 只跳过了 0x04 那一类，所以「活体敌人的尸体（穿着玩家版宇航服）」
			//     拿空后依然不熄灭。第三方 `SimpleImmersiveLooting` 的「扒取装备」=
			//   `UnequipAll()`（卸下后才可搜刮）正是同一条引擎规则的旁证。
			//   设 0 = 退回旧行为（只按 0x04 跳过），仅在排查时用。
			bool          skipEquippedLoot = true;

			// ================================================================
			// ★★ v4.9：展示柜（Display Case）容器**不判空** —— 默认 1
			// ================================================================
			// 背景与实证见常量区「v4.9 展示柜」长注释（一句话版）：
			//   展示柜（武器箱 / 武器架 / 头盔架…）的内容只在搜刮界面打开期间以
			//   `kTemporary` 条目投影进 `inventoryList`，关着时读到 size=0 ——
			//   不判空才能让「关着也亮」（否则就是用户实测的「打开才亮」）。
			//   设 0 = 退回旧行为（展示柜也按库存判空），仅在排查时用。
			bool          skipDisplayCaseEmpty = true;

			// ================================================================
			// ★★ v4.7：外景连续性 / 高亮丢失自愈（背景见常量区同名前缀的长注释）
			// ================================================================
			// 1 = 外景里跨 cell 边界当「连续过渡」处理：不整批熄灭、短静置、
			//     并且把「近期待过的 cell」一起扫描（边界对面的东西也能亮）。
			//   0 = 退回 v4.6 行为（每次跨边界全灭 + 2 秒不扫描），只用于对照排查。
			bool          exteriorContinuous = true;
			// 连续过渡时的静置时间（毫秒，0 ~ 2000）。引擎此刻正在往新 cell 的引用数组里
			// 塞东西，但世界并没有重建 ⇒ 给一小段就够（默认 300）。
			int           settleOnCellCrossMs = static_cast<int>(kSettleOnCellCrossMs);
			// 引用数「轻微变化」的容差（0 ~ 65536）：变化量不超过它就不再打断扫描
			// （外景 / 城市里走动时加载线程一直在流式增删引用）。设 0 = 退回旧判据
			// （任何长度变化都要重新等 kRefsStableRounds 轮）。
			int           streamJumpTolerance = static_cast<int>(kRefsStreamJumpMax);
			// 每轮最多复检多少个「已挂」目标的 3D 根（0 = 关掉这条自愈，仅排查用）。
			//   引擎侧描边是按 3D 图节点登记的，3D 被重建（外景流式加载 / LOD 切换）
			//   就会丢，而账本还记着「挂着」⇒ 从此不亮。这一项让 MOD 发现后重挂。
			int           verify3DPerScan = static_cast<int>(kVerify3DPerScan);
		};

		// ====================================================================
		// 运行期状态
		// ====================================================================
		// 原生 outline：一个已挂高亮的引用 + 下次该重申的时刻
		struct OutlineEntry
		{
			RE::NiPointer<RE::TESObjectREFR> ref;
			std::uint64_t                    reassertMs = 0;
			// 0 = 还在选中集合里；非 0 = 掉队时刻（到这个时刻之后才真正摘除）。
			// 见 Config::unhighlightGraceMs 的说明。
			std::uint64_t                    dropAt     = 0;
			// ★ v4.0：挂上去时用的 outline 状态（分类分色），只作诊断/日志用。
			//   摘除时**不读它** —— 先读引擎状态表里的真实值（原版可能覆盖过），
			//   这是 v2.3 起就守着的做法。
			std::uint32_t                    state      = 0xFF;
			// ★ v4.7：挂上（或最近一次重挂）时该引用的 3D 根节点地址。
			//   **只用于比较，绝不解引用**（3D 可能已经被销毁，那个地址随时可能失效）。
			//   引擎侧描边是按「3D 图节点」登记在 HighlightManager 里的（v2.3 实证：
			//   0x653F60 的 id 取自 3D 图叶子节点，不是 FormID）⇒ 3D 被重建
			//   （外景流式加载、LOD ↔ 真模型互换）就会丢，而账本还记着「挂着」。
			//   地址一变（或从 null 变非 null）就说明该重挂了 —— 见 SyncNativeOutline
			//   的「4) 3D 复检」段。
			const void*                      last3D     = nullptr;
		};

		struct State
		{
			// --- 表单绑定（★ v4.0：只剩「游戏内提示」用的一个 GLOB）---
			bool           bound         = false;
			std::uint32_t  prefix        = 0;
			RE::TESGlobal* notify        = nullptr;  // 0x807 SAS_Notify（DLL 写、脚本读）
			int            pendingNotify = 0;        // 0 = 无；1 = 刚开；2 = 刚关（Tick 里写走）

			// --- 开关 ---
			bool on      = true;
			bool keyDown = false;

			// --- 原生 outline：已挂上高亮的引用 ---
			//   （只在「新目标」和「周期性重申」时动手）
			//
			// ★ 这里存的是 NiPointer 而不是裸指针：摘高亮时要调引擎的函数，而它会
			//   解引用引用取 3D —— 裸指针在引用被销毁后就是野指针。
			std::unordered_map<RE::TESObjectREFR*, OutlineEntry> outlined;
			bool          nativeReady      = false;  // 核心三个引擎函数都已按签名校验通过
			// 「摘除」两个函数（Remove / Deactivate）是否可用。不可用时退化成
			// 「只把状态写回 12」——能少一点描边残留，但**摘不干净**（见上方长注释）。
			bool          nativeRemoveReady = false;
			std::uint64_t outlineRemoved   = 0;      // 成功摘除次数（诊断）
			std::uint64_t outlineRemoveMiss = 0;     // 调了 Remove 但一个管理器里都没有（诊断）
			std::uint64_t outlineUnhighlightMiss = 0; // 引擎的 0x653F60 也没动到任何管理器（诊断）
			std::uint64_t lastOutlineErrMs = 0;

			// --- ★ v3.2：扫描仪生命周期 → 自动重挂（诊断 + 检测状态）---
			bool          monocleOpen      = false;  // 上一轮 MonocleMenu 是否打开（= 举着扫描仪）
			std::uint32_t lastLiveManagers = 0;      // 上一轮存活的管理器数（下跌 = 引擎清过表）
			std::uint64_t outlineResyncs   = 0;      // 自动重挂次数（诊断：应该只在用扫描仪后 +1）

			// --- 节流 / 统计 ---
			std::uint64_t lastScanMs    = 0;
			std::uint64_t lastStatsMs   = 0;
			std::uint64_t lastBindWarnMs = 0;
			std::uint64_t lastMapDiagMs = 0;
			std::uint64_t scanCount     = 0;
			std::uint64_t tickCount     = 0;
			std::uint64_t candCount     = 0;
			std::uint64_t selCount      = 0;
			// ★ v4.0：分类命中数（诊断：每个类别实际挂了多少个）+ 提示通道计数
			std::array<std::uint32_t, kCategoryCount> categoryCounts{};
			std::uint64_t notifyWrites   = 0;  // 写进 SAS_Notify 的次数
			std::uint64_t loadGameResets = 0;  // 「载入画面由开变关」触发的重置次数
			bool          loadingSeen    = false;  // 上一帧载入画面是否开着（每帧都更新）
			bool          paramsRefreshed = false; // ★ v4.0.1：进世界后是否已做过一次配色刷新

			// --- 诊断：半径内、但 base form 类型不在白名单而被跳过的类型统计 ---
			//   统计窗口 = 两条统计日志之间，打完之后清空。
			//   用途：如果游戏里发现「某个东西该亮却没亮」，看这条日志就知道它的
			//   formType 是多少，再决定要不要加进 ClassifyBase 的白名单。
			//   ★ v2.2：从 unordered_map 换成定长数组 —— 这段代码对**每个被拒的
			//   引用**都要执行一次，密集场景里每秒 2 万次哈希表操作，纯属白烧主线程。
			//   现在下标就是 formType，一次自增完事。
			std::array<std::uint32_t, 256> rejectTypes{};
			// ★ v4.1：本轮**进入候选集合**（= 该亮的目标）的 base formType 直方图。
			//   用途：用户报「某某东西不该亮」时，直接从日志看它的 formType 是多少，
			//   再决定把它挪到哪个类别、或加一条排除规则（rejTypes 只能告诉我们
			//   「该亮没亮的」，这个是反向的）。
			std::array<std::uint32_t, 256> candTypes{};
			// ★ v4.1：窗口内因为「类别开关 = 0」而被跳过的引用数（诊断：确认开关真的生效）。
			std::uint64_t disabledSkips = 0;

			// --- ★ v4.2：尸体 / 搜空（诊断）---
			//   窗口内累加，统计日志打完后清空。这些计数每轮扫描都会累加
			//   （同一个尸体在半径内待 5 秒 ≈ 25 轮 ⇒ 计数会是它的 ~25 倍），
			//   所以只用来回答「有没有、大概多少」，精确数量看探针行。
			std::uint64_t emptySkips       = 0;  // 因为「库存为空」被跳过的候选数（容器 + 尸体）
			std::uint64_t lootUnknown      = 0;  // 想判空但读不到（未知 ⇒ 按「有东西」处理，照常亮）
			std::uint64_t lootNotEmpty     = 0;  // 判到「有东西」（正常，会亮）
			std::uint64_t lootNullInv      = 0;  // ★ v4.3：inventoryList 指针就是 null（库存还没被引擎创建）
			std::uint64_t lootBadShape     = 0;  // ★ v4.3：形状不像 BGSInventoryList（疑似标定选错了偏移）
			// ★ v4.6：判空时被「跳过」的条目数（= 面板拿不走的那两类），分两路来源计数。
			//   ★ 注意 RefLootState 找到第一件**真能拿**的东西就早退，所以这两个数只是
			//     「本轮走到的那部分」里的累计值 —— 用途是确认**规则有没有在生效**
			//     （非 0 且持续增长 = 生效）；精确的残留明细看 `loot probe:` 那几行。
			std::uint64_t lootSkipNonPlayable = 0;  // 记录标志 0x04（`*_NOTPLAYABLE`）
			std::uint64_t lootSkipEquipped    = 0;  // 装备中（kSlotMask ≠ 0，还穿在身上）
			std::uint64_t corpseSeen       = 0;  // 判定为尸体的引用数（ACHR 尸体 + 尸体道具）
			std::uint64_t corpseByBit      = 0;  // 其中靠运行时 kDead 位判定的
			std::uint64_t corpseByFlag     = 0;  // 其中靠记录标志 Starts Dead 判定的
			std::uint64_t corpseProps      = 0;  // 其中是「base = NPC_/LVLN 的普通 REFR」尸体道具
			std::uint64_t corpseUncSeen    = 0;  // 其中是「Starts Unconscious」（炮塔/机器人报废体、倒地者）
			std::uint64_t corpseUncSkipped = 0;  // 因为「Starts Unconscious 且没开 CorpseUnconscious」被跳过的
			std::uint32_t corpseProbes     = 0;  // 本会话已经打过的尸体探针数（上限 kCorpseProbeMax）
			// ★ v4.4：判死的「新来源」各自的计数（诊断：一眼看出是哪条路在起作用）
			std::uint64_t corpseByLife     = 0;  // 靠 lifeState ∈ {1,2,5}（引擎 IsDead()）判定的
			std::uint64_t corpseByBleed    = 0;  // 靠 lifeState ∈ {7,8}（引擎 IsBleedingOut()）判定的
			// ★ v4.4：判决缓存与探针节流
			//   achrVerdict：ref → 判决码（0=活 1=死 2=昏迷 3=出血）。**判决变化**时打一条
			//   `actor probe (changed)`（先只记录不打，避免新进入半径的 ACHR 刷屏）。
			std::unordered_map<const RE::TESObjectREFR*, std::uint8_t> achrVerdict;
			//   lootProbed：已经打过「库存明细」探针的引用（每个 ref 只打一次）。
			std::unordered_set<const RE::TESObjectREFR*> lootProbed;
			std::uint32_t lootProbes        = 0;  // 本会话已打的库存明细探针数（上限 cfg.lootProbeMax）
			std::uint32_t actorChangeProbes = 0;  // 本会话已打的「判决变化」探针数
			// ★ v4.8：容器探针 —— 每个 ref 记住「上次的判空结果」与已打的**变化**条数，
			//   判决变化时再补一条（首次快照 + 「打开 → 关掉」两次变化 = 前后对照）。
			//   ★ 首次快照不受 `kContProbeChangeMax` 限制（见常量区说明）。
			struct ContProbeRec
			{
				int           lastVerdict = 0x7FFFFFFF;  // 上次 RefLootState 的返回值
				std::uint32_t changes     = 0;           // 已为该 ref 打过的「变化」条数
				bool          seen        = false;       // 首次快照打过了没有
			};
			std::unordered_map<const RE::TESObjectREFR*, ContProbeRec> contProbed;
			std::uint32_t contProbes = 0;  // 本会话已打的容器探针数（上限 cfg.contProbeMax）
			// ★ v4.9：展示柜（见常量区「v4.9 展示柜」）——
			//   displayCaseRuntime：运行期**学习**到的「展示柜 base」集合
			//   （打开期间读到过 kTemporary 条目的容器；覆盖第三方 mod 新增的记录，
			//     与静态白名单 SasDisplayCases.h 取或）。
			//   displayCaseSkips：窗口内「因为白名单而没被『读到空』跳过」的次数
			//   （= 展示柜照常亮的轮数；非 0 且增长 = 规则在生效）。
			std::unordered_set<std::uint32_t> displayCaseRuntime;
			std::uint64_t                     displayCaseSkips = 0;
			// ★ v4.3：ACHR 判决全景（「该亮没亮」时最有用的一对数）
			std::uint64_t achrSeen         = 0;  // 窗口内半径内的 ACHR 数（不论死活）
			std::uint64_t achrLive         = 0;  // 其中被判成「活人」跳过的（红线；「尸体不亮」先看这里）
			std::uint32_t actorProbes      = 0;  // 本会话已经打过的 actor 探针数（上限 cfg.actorProbeMax）
			// ★ v4.3：第一层过滤的计数 —— 「某具尸体从没出现过」时用来确认它没被
			//   挡在更前面（Deleted/Disabled、或 parentCell 不是当前 cell）。
			std::uint64_t skipDeleted      = 0;
			std::uint64_t skipParentCell   = 0;

			// --- ★ v4.2：库存列表偏移的标定状态（见 CalibrateInventory）---
			//   ★ v4.3：改成「跨轮累计」——每轮最多采样 kInvCalibPerRound 个，
			//     攒够票才采纳；只有累计 kInvCalibMaxRounds 轮仍不通过才永久降级。
			bool          invCalibDone = false;
			std::uint32_t invVotes[2]{};   // 两个候选偏移各自通过形状校验的样本数
			std::uint32_t invStrong[2]{};  // 其中「强样本」数（读到的条目里 object 是合法表单）
			std::uint32_t invBad[2]{};     // 各自形状校验失败的样本数
			std::uint32_t invChecks    = 0;  // 一共采样了多少个引用（容器 + ACHR）
			std::uint32_t invRounds    = 0;  // 已经尝试过多少轮（到 kInvCalibMaxRounds 才放弃）

			// --- v2.2：耗时统计（每次统计日志之间重置，用来定位卡顿）---
			std::uint64_t scanMsTotal     = 0;  // 窗口内 Sum(每轮 Rescan 耗时)
			std::uint64_t scanMsMax       = 0;  // 窗口内最大单轮耗时（这才是卡顿的感觉来源）
			std::uint32_t scanMsSamples   = 0;
			std::uint64_t opsThisScan     = 0;  // 最近一轮实际发出的引擎调用数
			std::uint64_t opsDeferred     = 0;  // 因为预算不够而推迟到下一轮的调用数
			bool          loadingNow      = false;

			// --- ★ v2.3：把 Rescan 拆成几段分别计时（窗口内求和 / 求最大）---
			//   没有这一行就只能看到「scan avg=32ms」而不知道 32ms 花在哪。
			//   · shape = 读 cell + 引用数组形状校验（VirtualQuery 集中在这一段，看 vq 次数）
			//   · loop  = 遍历全部引用挑目标（纯内存读）
			//   · sync  = 挂/摘（SyncNativeOutline，引擎调用集中在这一段）
			//   · unh / add = 其中 UnoutlineRef / OutlineRef 各占多少
			std::uint64_t tShapeMs  = 0;
			std::uint64_t tShapeMax = 0;
			std::uint64_t tLoopMs   = 0;
			std::uint64_t tLoopMax  = 0;
			std::uint64_t tSyncMs   = 0;
			std::uint64_t tSyncMax  = 0;
			std::uint64_t tUnhMs    = 0;
			std::uint64_t tAddMs    = 0;
			std::uint64_t vqCalls   = 0;

			// --- 场景跟踪 ---
			RE::TESObjectCELL* lastCell = nullptr;

			// --- cell 引用数组的稳定性判据（见 Rescan 顶部）---
			std::size_t   cellRefsOff   = 0;  // 形状校验通过的偏移（0 = 还没定）
			std::uint32_t lastRefsSize  = 0;  // 上一轮看到的引用数
			std::uint32_t stableRounds  = 0;  // 长度连续相同的轮数
			std::uint64_t settleUntilMs = 0;  // 在此之前不扫描（换场景/读档后的静置期）
			std::uint64_t refsRejected  = 0;  // 引用数组校验不通过而跳过的次数

			// ================================================================
			// ★★ v4.7：近期 cell 环（外景连续过渡 + 边界对面也参与扫描）
			//   定义与淘汰规则见常量区 kRingCellMax 上方的长注释。
			// ================================================================
			struct RingCell
			{
				RE::TESObjectCELL* cell  = nullptr;
				std::uint64_t      seenMs = 0;  // 最近一次校验通过 / 玩家在其中的时刻
				std::uint32_t      miss   = 0;  // 连续校验失败次数（到 kRingCellMissMax 就踢）
				std::uint32_t      refs   = 0;  // 最近一次读到的引用数（诊断）
			};
			RingCell      ring[kRingCellMax]{};
			// --- 诊断（窗口内，打完成绩清零）---
			std::uint64_t cellChanges    = 0;  // 换 cell 次数
			std::uint64_t cellContinuous = 0;  // 其中走「连续过渡」的次数（应占绝大多数）
			std::uint64_t ringSkipped    = 0;  // 环里校验失败被跳过的 cell 次数（卸载中的 cell）
			std::uint32_t ringCells       = 0;  // 最近一轮实际参与扫描的「环内额外 cell 数」
			std::uint64_t streamMoves    = 0;  // 引用数轻微变化（流式）但照常扫描的次数

			// --- ★ v4.7：扫描被「哪一种原因」跳过的窗口计数（诊断）---
			//   用途：用户报「行走时不扫描」时，一次日志就能指出卡在哪一步。
			std::uint64_t skipOff       = 0;  // 功能关着（F8 OFF）
			std::uint64_t skipThrottle  = 0;  // 扫描间隔节流（正常路径，5/s）
			std::uint64_t skipLoading   = 0;  // 载入画面开着（不做任何引擎调用）
			std::uint64_t skipNoPlayer  = 0;
			std::uint64_t skipNoCell    = 0;
			std::uint64_t skipSettle    = 0;  // 换场景 / 连续过渡后的静置期内
			std::uint64_t skipUnstable  = 0;  // 引用数组还没定 / 长度突变
			std::uint64_t skipWarmup    = 0;  // 等稳定轮数（kRefsStableRounds）

			// --- ★ v4.7：Tick 间隔诊断（区分「Tick 没被调」和「被早退挡住」）---
			std::uint64_t lastTickMs     = 0;
			std::uint64_t tickGaps       = 0;  // 间隔 > kTickGapLogMs 的次数
			std::uint64_t tickGapMsMax   = 0;
			std::uint64_t tickGapMsTotal = 0;

			// --- ★ v4.7：3D 复检（引擎侧描边丢了就重挂）---
			std::size_t   verifyCursor       = 0;  // 轮转游标（覆盖所有已挂目标）
			std::uint64_t outline3DProbes    = 0;  // 复检了多少次
			std::uint64_t outline3DReasserts = 0;  // 其中发现 3D 变了、重挂了多少次

			// --- ★ v4.7：移动距离（诊断：把「行走」和「跳过」对起来看）---
			RE::NiPoint3 lastPos{};
			bool         havePos    = false;
			float        moveMeters = 0.0f;
		};

		Config               g_cfg;
		State                g_state;
		std::mutex           g_tickLock;
		std::atomic_uint32_t g_mainThreadId{ 0 };
		std::string          g_iniPath;

		// ====================================================================
		// ★ v4.0：读档信号 = 「载入画面由开变关」
		// --------------------------------------------------------------------
		// 旧方案靠 Papyrus 脚本在 OnPlayerLoadGame 里把 SAS_Epoch +1，DLL 轮询那个
		// GLOB 才知道「玩家读档了」。现在直接看引擎自己的 LoadingMenu / FaderMenu
		// —— 那条 Papyrus 依赖（以及 Epoch GLOB 与脚本里的 OnPlayerLoadGame）删掉。
		//
		// ★ 为什么不用 `RE::TESLoadGameEvent`：它定义在 `RE/E/Events.h`，而那个头
		//   依赖「在 umbrella 头里被按顺序包含」，单独 include 会在 DamageImpactData /
		//   HitData 那一段因为缺类型而编译失败（实测）。为一个信号把整个
		//   `RE/Starfield.h` 拖进来不值得，而载入画面这个信号**同样覆盖**读档，
		//   还顺带覆盖快速旅行 / 进出建筑（与「换 cell」判据重复触发是无害的）。
		// ====================================================================

		// 前置声明：定义在后面的「原生 outline」小节里（SetOn / ResetForNewScene 要先用到）
		void ClearAllNativeOutline();
		void MarkAllForRemoval(std::uint64_t a_deadlineMs);
		void MarkAllForReassert(const char* a_reason);

		// ====================================================================
		// 小工具
		// ====================================================================
		std::uint64_t NowMs()
		{
			using namespace std::chrono;
			return static_cast<std::uint64_t>(
				duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
		}

		// ★ v2.3：把 Rescan 拆段计时用的小工具（作用域结束自动累加）。
		//   用它以后，即使代码里中途 return，耗时也照样被记上。
		struct PhaseTimer
		{
			std::uint64_t* sum;
			std::uint64_t* max;
			std::uint64_t  t0;

			explicit PhaseTimer(std::uint64_t* a_sum, std::uint64_t* a_max = nullptr) :
				sum(a_sum), max(a_max), t0(NowMs())
			{}

			~PhaseTimer()
			{
				const auto dt = NowMs() - t0;
				if (sum) {
					*sum += dt;
				}
				if (max && dt > *max) {
					*max = dt;
				}
			}

			PhaseTimer(const PhaseTimer&) = delete;
			PhaseTimer& operator=(const PhaseTimer&) = delete;
		};

		// ----------------------------------------------------------------
		// 内存安全小工具
		// 凡是「按实测偏移读出来的指针 / 容器」，用之前一律先用它们验证；
		// 不通过就放弃（宁可这一轮不扫描，绝不乱读内存）。
		// ----------------------------------------------------------------
		bool IsPlausiblePointer(std::uint64_t a_ptr)
		{
			return a_ptr > 0x10000ULL && a_ptr < 0x7FFFFFFFFFFFULL;
		}

		// ----------------------------------------------------------------
		// ★ v2.3：IsReadable 加「已验证可读区域」缓存（治实测里的 31ms 假卡顿）
		// ----------------------------------------------------------------
		// v2.2 实测日志里最刺眼的一行：
		//     scan#1 ... cand=0 sel=0 ... timing: scan avg=31ms max=31ms ops=0
		// 第一轮扫描**什么活都没干**（引用数组长度第一次见 → 直接 return），却花了 31ms。
		// 那一段里唯一的重活就是 ValidateCellRefs 那 6~7 次 VirtualQuery。
		// VirtualQuery 要走内核对 VAD 树，在「几 GB、VAD 极度碎片化」的游戏进程里
		// 每次都是毫秒级开销；而它要回答的问题（这几个地址能不能读）对于
		// 「同一个 cell 的同一批对象」几乎是常量。
		// ⇒ 缓存「已验证可读」的区间：命中直接返回，不再问内核。
		//   区间带 TTL（3 秒）；cell 变化 / 读档时整体作废。
		//   底线不变：**没验证过的地址永远返回 false**（宁可不扫，绝不乱读）。
		struct ReadRegion
		{
			std::uintptr_t base      = 0;
			std::uintptr_t end       = 0;
			std::uint64_t  expiresMs = 0;
		};
		constexpr std::size_t kReadRegionCount = 8;
		constexpr std::uint64_t kReadRegionTtlMs = 3000;
		ReadRegion            g_readRegions[kReadRegionCount];
		std::size_t           g_readRegionNext = 0;
		std::uint64_t         g_vqCalls        = 0;  // 诊断：窗口内真的问了内核多少次

		void InvalidateReadRegions()
		{
			for (auto& r : g_readRegions) {
				r = ReadRegion{};
			}
		}

		bool IsReadable(const void* a_ptr, std::size_t a_len)
		{
			if (!IsPlausiblePointer(reinterpret_cast<std::uint64_t>(a_ptr))) {
				return false;
			}
			if (a_len > (1u << 20)) {
				a_len = 1u << 20;  // 防止下面的加法溢出 / 无谓的巨大区间
			}
			const auto start = reinterpret_cast<std::uintptr_t>(a_ptr);
			const auto end   = start + a_len;
			if (end < start) {
				return false;
			}
			const auto now = NowMs();
			for (const auto& r : g_readRegions) {
				if (r.base && now < r.expiresMs && start >= r.base && end <= r.end) {
					return true;
				}
			}

			MEMORY_BASIC_INFORMATION mbi{};
			++g_vqCalls;
			if (::VirtualQuery(a_ptr, &mbi, sizeof(mbi)) == 0) {
				return false;
			}
			if (mbi.State != MEM_COMMIT) {
				return false;
			}
			if (mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) {
				return false;
			}
			const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
			const auto rend = base + mbi.RegionSize;
			if (end > rend || rend <= base) {
				return false;
			}
			auto& slot = g_readRegions[g_readRegionNext];
			g_readRegionNext = (g_readRegionNext + 1) % kReadRegionCount;
			slot.base        = base;
			slot.end         = rend;
			slot.expiresMs   = now + kReadRegionTtlMs;
			return true;
		}

		// ================================================================
		// ★ v4.3：强判据 —— 这个地址真的指向一个「引擎表单（TESForm）」吗？
		// ================================================================
		// 用途：标定「库存列表偏移」时校验「读到的条目里 `object` 字段是不是真的物品」
		//   —— 标定选错偏移时，读到的多半是垃圾指针，这里会立刻失败。
		//   判据全是 TESForm 基类的固定成员（本项目长期实测读法正确，且有 static_assert 钉住）：
		//     formID @+0x28 非 0；formType @+0x2E ∈ (0, 0x60]（引擎的表单类型都在这个区间）。
		// ★ 什么时候**不能**用它：热路径（每轮扫描）。它会调 IsReadable（可能触发
		//   VirtualQuery）—— 那是 v2.3 实测过 31ms 假卡顿的来源。所以它只用于
		//   「标定」这种一次性/低频场景。
		// ★ 宁可少认：返回 false 的最坏结果只是「标定多等几轮」或「判空退回照常亮」。
		bool IsPlausibleFormPtr(std::uint64_t a_ptr)
		{
			if (!IsPlausiblePointer(a_ptr) || !IsReadable(reinterpret_cast<const void*>(a_ptr), 0x38)) {
				return false;
			}
			const auto* p  = reinterpret_cast<const std::uint8_t*>(a_ptr);
			const auto  id = *reinterpret_cast<const std::uint32_t*>(p + kOffFormID);
			const auto  ft = p[kOffFormType];
			if (id == 0) {
				return false;
			}
			if (ft == 0 || ft > 0x60) {
				return false;
			}
			return true;
		}

		// 一个「BSTArray 风格」的原始视图（size@+0 / capacity@+4 / data@+8）
		struct RawArray
		{
			std::uint32_t size{ 0 };
			std::uint32_t capacity{ 0 };
			std::uint64_t data{ 0 };

			[[nodiscard]] bool valid() const
			{
				if (size == 0) {
					return false;  // 空数组对调用方没意义，直接当「还不能扫」
				}
				if (size > kMaxRefsSanity || capacity < size || capacity > (1u << 20)) {
					return false;  // 形状明显不自洽 = 读到垃圾了
				}
				return IsReadable(reinterpret_cast<const void*>(data), 8);
			}
		};

		RawArray ReadRawArray(const void* a_base, std::size_t a_off)
		{
			const auto* p = static_cast<const std::uint8_t*>(a_base) + a_off;
			RawArray    a;
			a.size     = *reinterpret_cast<const std::uint32_t*>(p + kOffArraySize);
			a.capacity = *reinterpret_cast<const std::uint32_t*>(p + kOffArrayCapacity);
			a.data     = *reinterpret_cast<const std::uint64_t*>(p + kOffArrayData);
			return a;
		}

		// 「这个偏移上真的是 cell 的引用数组吗？」
		// 判据（都不依赖任何固定成员偏移，所以两种偏移假设下都成立）：
		//   ① BSTArray 头形状自洽（size/capacity 关系合理、data 可读）；
		//   ② 抽查前 kRefsShapeSamples 个元素：[元素可读]
		//      且 [element->formType(+0x2E) ∈ {REFR, ACHR}]。
		//      formType 在 TESForm 基类里，不受 TESObjectCELL 那 +8 位移影响。
		// 两条都过才认 —— 垃圾内存同时满足这两条的几率可以忽略。
		bool ValidateCellRefs(const RawArray& a_arr, std::uint32_t a_samples)
		{
			if (!a_arr.valid() || !IsReadable(reinterpret_cast<const void*>(a_arr.data), 32)) {
				return false;
			}
			auto* const*        list = reinterpret_cast<const void* const*>(a_arr.data);
			const std::uint32_t n    = std::min<std::uint32_t>(a_arr.size, a_samples);
			for (std::uint32_t i = 0; i < n; ++i) {
				const auto* elem = static_cast<const std::uint8_t*>(list[i]);
				if (!IsReadable(elem, 0x100)) {
					return false;
				}
				const auto ft = *reinterpret_cast<const std::uint8_t*>(elem + kOffFormType);
				if (ft != kFormTypeREFR && ft != kFormTypeACHR) {
					return false;
				}
			}
			return true;
		}

		// ================================================================
		// ★ v4.7：近期 cell 环（定义与淘汰规则见常量区 kRingCellMax 上方）
		// ================================================================
		void RingClear()
		{
			for (auto& r : g_state.ring) {
				r = State::RingCell{};
			}
		}

		std::uint32_t RingCount()
		{
			std::uint32_t n = 0;
			for (const auto& r : g_state.ring) {
				if (r.cell) {
					++n;
				}
			}
			return n;
		}

		bool RingContains(const RE::TESObjectCELL* a_cell)
		{
			if (!a_cell) {
				return false;
			}
			for (const auto& r : g_state.ring) {
				if (r.cell == a_cell) {
					return true;
				}
			}
			return false;
		}

		// 把 cell 放进环里（已在环里就刷新时间戳 / 引用数），必要时淘汰最旧的一个。
		void RingTouch(RE::TESObjectCELL* a_cell, std::uint64_t a_nowMs, std::uint32_t a_refs)
		{
			if (!a_cell) {
				return;
			}
			State::RingCell* free = nullptr;
			for (auto& r : g_state.ring) {
				if (r.cell == a_cell) {
					r.seenMs = a_nowMs;
					r.miss   = 0;
					r.refs   = a_refs;
					return;
				}
				if (!r.cell && !free) {
					free = &r;
				}
			}
			if (!free) {
				// 环满：淘汰最旧的那个
				free = &g_state.ring[0];
				for (auto& r : g_state.ring) {
					if (r.seenMs < free->seenMs) {
						free = &r;
					}
				}
			}
			*free = State::RingCell{};
			free->cell   = a_cell;
			free->seenMs = a_nowMs;
			free->refs   = a_refs;
		}

		// 引用总数超过上限时，从最旧的开始丢（防止每轮要遍历的引用数无界增长）。
		void RingTrimByRefs()
		{
			std::uint64_t total = 0;
			for (const auto& r : g_state.ring) {
				total += r.refs;
			}
			while (total > kRingRefsCap) {
				State::RingCell* oldest = nullptr;
				for (auto& r : g_state.ring) {
					if (r.cell && (!oldest || r.seenMs < oldest->seenMs)) {
						oldest = &r;
					}
				}
				if (!oldest) {
					return;
				}
				total -= oldest->refs;
				*oldest = State::RingCell{};
			}
		}

		// 「这个偏移上真的是这个 cell 的引用数组吗？」—— 环里那些 cell 用这一条。
		// 比 ValidateCellRefs 多一层**自洽性**判据：抽到的引用必须承认自己是这个 cell 的
		// （`ref->parentCell == cell`）。这条对「cell 已经被卸载 / 对象被释放但内存还
		// 可读」的极端情况特别有效 —— 垃圾数组几乎不可能同时满足
		// [BSTArray 头自洽] + [元素是 REFR/ACHR] + [元素自认属于这个 cell]。
		bool ValidateRingCellRefs(const RawArray& a_arr, const RE::TESObjectCELL* a_cell, std::uint32_t a_samples)
		{
			if (!ValidateCellRefs(a_arr, a_samples)) {
				return false;
			}
			auto* const*        list = reinterpret_cast<RE::TESObjectREFR* const*>(a_arr.data);
			const std::uint32_t n    = std::min<std::uint32_t>(a_arr.size, a_samples);
			for (std::uint32_t i = 0; i < n; ++i) {
				if (list[i]->parentCell != a_cell) {
					return false;
				}
			}
			return true;
		}

		std::string ModuleDir()
		{
			char    buf[MAX_PATH]{};
			HMODULE self = nullptr;
			::GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&ModuleDir), &self);
			if (!::GetModuleFileNameA(self, buf, MAX_PATH)) {
				return {};
			}
			std::string  path{ buf };
			const auto   pos = path.find_last_of("\\/");
			return pos == std::string::npos ? std::string{} : path.substr(0, pos);
		}

		bool GameWindowFocused()
		{
			const HWND hwnd = ::GetForegroundWindow();
			if (!hwnd) {
				return false;
			}
			DWORD pid = 0;
			::GetWindowThreadProcessId(hwnd, &pid);
			return pid == ::GetCurrentProcessId();
		}

		// ----------------------------------------------------------------
		// 「现在是不是卡在载入画面」
		// ----------------------------------------------------------------
		// ★ v2.2 新增。实测日志里出现过 18 秒的空档，正好落在换区域的时候：
		//   主线程在跑加载，我们这时候去调引擎（LookupOrAdd / Remove）每条都被拖慢
		//   几十毫秒，几百条叠起来就是一个长卡顿。载入期间**什么都不做**最省事。
		// 菜单名用 FO4/Skyrim 那一套（Starfield 沿用）：LoadingMenu / FaderMenu。
		// 菜单没注册时 IsMenuOpen 返回 false，所以不会误判。
		bool IsLoadingScreenUp()
		{
			static const RE::BSFixedString kLoadingMenu{ "LoadingMenu" };
			static const RE::BSFixedString kFaderMenu{ "FaderMenu" };
			auto*                           ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}
			return ui->IsMenuOpen(kLoadingMenu) || ui->IsMenuOpen(kFaderMenu);
		}

		// ----------------------------------------------------------------
		// 「玩家此刻有没有举着手持扫描仪」
		// ----------------------------------------------------------------
		// ★ v3.2 新增。依据是 B 社自己的脚本（`ScanTempleScript.psc` /
		//   `MQ_Temple_SubScript.psc`）：它们用 `RegisterForMenuOpenCloseEvent("MonocleMenu")`
		//   + `abOpening` 判断「扫描仪是否举着」，注释原话是
		//   *we'll need a way to check if you have the scanner up or not*。
		//   也就是说**举着扫描仪 == `MonocleMenu` 处于 open 状态**。
		//   我们只需要「开/关切换」这个时机，不需要每帧知道值，所以直接轮询即可。
		//   菜单没注册时 IsMenuOpen 返回 false（不会崩）。
		bool IsMonocleMenuOpen()
		{
			static const RE::BSFixedString kMonocleMenu{ "MonocleMenu" };
			auto*                           ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}
			return ui->IsMenuOpen(kMonocleMenu);
		}

		void LoadConfig()
		{
			g_iniPath          = ModuleDir() + "\\SAS_AlwaysScan.ini";
			const char* ini    = g_iniPath.c_str();

			auto getInt = [&](const char* a_key, int a_def) {
				return static_cast<int>(::GetPrivateProfileIntA("General", a_key, a_def, ini));
			};
			char buf[64]{};
			auto getFloat = [&](const char* a_key, float a_def) {
				buf[0] = '\0';
				::GetPrivateProfileStringA("General", a_key, "", buf, sizeof(buf), ini);
				if (buf[0] == '\0') {
					return a_def;
				}
				try {
					return std::stof(buf);
				} catch (...) {
					return a_def;
				}
			};

			g_cfg.startEnabled   = getInt("StartEnabled", 1) != 0;
			g_cfg.hotkeyVk       = getInt("HotkeyVK", VK_F8);
			g_cfg.maxTargets     = std::clamp(getInt("MaxTargets", 256), 8, 1024);
			g_cfg.scanIntervalMs = std::clamp(getInt("ScanIntervalMs", 200), 50, 2000);
			g_cfg.logStats       = getInt("LogStats", 1) != 0;
			g_cfg.radiusMeters   = std::clamp(getFloat("RadiusMeters", 50.0f), 5.0f, 500.0f);
			g_cfg.unitsPerMeter  = std::clamp(getFloat("UnitsPerMeter", 3.4286f), 0.1f, 200.0f);
			g_cfg.onlyInFront   = getInt("OnlyInFront", 1) != 0;
			g_cfg.frontFovDeg   = std::clamp(getFloat("FrontFovDeg", 110.0f), 20.0f, 360.0f);
			g_cfg.reassertMs    = std::clamp(getInt("ReassertMs", 0), 0, 60000);  // 0 = 不重申
			g_cfg.autoEnsureManagers = getInt("AutoEnsureManagers", 1) != 0;
			g_cfg.resyncOnScannerClose = getInt("ResyncOnScannerClose", 1) != 0;
			g_cfg.unhighlightGraceMs = std::clamp(getInt("UnhighlightGraceMs", 1500), 0, 60000);
			g_cfg.maxOutlineOpsPerScan = std::clamp(getInt("MaxOutlineOpsPerScan", 64), 0, 4096);

			// ★ v4.7：外景连续性 / 高亮丢失自愈（背景见常量区长注释）
			g_cfg.exteriorContinuous  = getInt("ExteriorContinuous", 1) != 0;
			g_cfg.settleOnCellCrossMs = std::clamp(getInt("SettleOnCellCrossMs", static_cast<int>(kSettleOnCellCrossMs)), 0, 2000);
			g_cfg.streamJumpTolerance = std::clamp(getInt("StreamJumpTolerance", static_cast<int>(kRefsStreamJumpMax)), 0, 65536);
			g_cfg.verify3DPerScan     = std::clamp(getInt("Verify3DPerScan", static_cast<int>(kVerify3DPerScan)), 0, 1024);

			// --- ★ v4.0：分类分色（每个类别一个 outline 状态 0..11）---
			//   （★ v4.2 追加 corpse，默认 9 = 与容器同色，理由见 Config::stateByCategory）
			const char* const kStateKeys[kCategoryCount] = {
				"StateLoot", "StateContainer", "StateDevice", "StateDoor", "StateFlora", "StateOther", "StateCorpse"
			};
			const int kStateDef[kCategoryCount] = { 2, 9, 4, 10, 5, 1, 9 };
			for (std::size_t i = 0; i < kCategoryCount; ++i) {
				g_cfg.stateByCategory[i] = std::clamp(getInt(kStateKeys[i], kStateDef[i]), 0, 11);
			}

			// --- ★ v4.1：类别开关（默认只有 kOther = MSTT 关着，理由见 Config 里的长注释）---
			{
				const char* const kEnableKeys[kCategoryCount] = {
					"EnableLoot", "EnableContainer", "EnableDevice", "EnableDoor", "EnableFlora", "EnableOther", "EnableCorpse"
				};
				const int kEnableDef[kCategoryCount] = { 1, 1, 1, 1, 1, 0, 1 };
				std::string s;
				for (std::size_t i = 0; i < kCategoryCount; ++i) {
					g_cfg.categoryEnabled[i] = getInt(kEnableKeys[i], kEnableDef[i]) != 0;
					if (i) {
						s += ", ";
					}
					s += kCategoryName[i];
					s += '=';
					s += g_cfg.categoryEnabled[i] ? '1' : '0';
				}
				REX::INFO("config: categoryEnabled: {}", s);
			}

			g_cfg.notifyOnToggle = getInt("NotifyOnToggle", 1) != 0;

			// --- ★ v4.2：尸体 / 搜空（v4.2.1 起 CorpseUnconscious 默认 1，理由见 Config 里的说明）---
			g_cfg.corpseUnconscious = getInt("CorpseUnconscious", 1) != 0;
			g_cfg.skipEmptyLoot     = getInt("SkipEmptyLoot", 1) != 0;
			// ★ v4.4：判死改用引擎自己的 lifeState 枚举（理由见常量区与 Config 的长注释）
			g_cfg.corpseLifeState   = getInt("CorpseLifeState", 1) != 0;
			g_cfg.corpseBleedout    = getInt("CorpseBleedout", 1) != 0;
			// ★ v4.3：ACHR 探针条数上限（0 = 关掉那组 `actor probe:` 日志）
			g_cfg.actorProbeMax = std::clamp(getInt("ActorProbeMax", static_cast<int>(kActorProbeMax)), 0, 256);
			// ★ v4.4：判决变化探针 / 库存明细探针的条数上限
			g_cfg.actorChangeProbeMax =
				std::clamp(getInt("ActorChangeProbeMax", static_cast<int>(kActorChangeProbeMax)), 0, 256);
			g_cfg.lootProbeMax =
				std::clamp(getInt("LootProbeMax", static_cast<int>(kLootProbeMax)), 0, 256);
			// ★ v4.8：容器「判空链路快照」探针的条数上限
			g_cfg.contProbeMax =
				std::clamp(getInt("ContProbeMax", static_cast<int>(kContProbeMax)), 0, 256);
			// ★ v4.3：库存指针是 null 时算「空」（默认 1；理由见 Config 里的说明）
			g_cfg.treatNullInvAsEmpty = getInt("TreatNullInvAsEmpty", 1) != 0;
			// ★ v4.5：判空跳过「非玩家物品（0x04）」（默认 1；理由见 Config 里的说明）
			g_cfg.skipNonPlayableLoot = getInt("SkipNonPlayableLoot", 1) != 0;
			// ★ v4.6：判空跳过「正穿在身上的装备（kSlotMask）」」（默认 1；理由见 Config 里的说明）
			g_cfg.skipEquippedLoot = getInt("SkipEquippedLoot", 1) != 0;
			// ★ v4.9：展示柜容器不判空（默认 1；理由见 Config 里的说明）
			g_cfg.skipDisplayCaseEmpty = getInt("SkipDisplayCaseEmpty", 1) != 0;
			REX::INFO("config: corpseUnconscious={} corpseLifeState={} corpseBleedout={} skipEmptyLoot={}",
				g_cfg.corpseUnconscious, g_cfg.corpseLifeState, g_cfg.corpseBleedout, g_cfg.skipEmptyLoot);
			REX::INFO("config: actorProbeMax={} actorChangeProbeMax={} lootProbeMax={} contProbeMax={} treatNullInvAsEmpty={} skipNonPlayableLoot={} skipEquippedLoot={} skipDisplayCaseEmpty={}",
				g_cfg.actorProbeMax, g_cfg.actorChangeProbeMax, g_cfg.lootProbeMax, g_cfg.contProbeMax,
				g_cfg.treatNullInvAsEmpty, g_cfg.skipNonPlayableLoot, g_cfg.skipEquippedLoot,
				g_cfg.skipDisplayCaseEmpty);

			// --- ★ v4.0.1：可选的自定义类别颜色（ColorLoot=RRGGBB …，留空 = 用引擎原生配色）---
			{
				const char* const kColorKeys[kCategoryCount] = {
					"ColorLoot", "ColorContainer", "ColorDevice", "ColorDoor", "ColorFlora", "ColorOther", "ColorCorpse"
				};
				std::string colorLog;
				for (std::size_t i = 0; i < kCategoryCount; ++i) {
					char hex[32]{};
					::GetPrivateProfileStringA("General", kColorKeys[i], "", hex, sizeof(hex), ini);
					char* p = hex;
					if (*p == '#') {
						++p;
					}
					if (*p == '\0') {
						continue;  // 没写 = 不覆盖
					}
					char*        end = nullptr;
					const auto   v   = std::strtoul(p, &end, 16);
					if (end == p || v > 0xFFFFFFu) {
						REX::WARN("config: {}='{}' 解析失败（要 6 位十六进制，例如 ColorLoot=FF8000）-> 忽略",
							kColorKeys[i], p);
						continue;
					}
					g_cfg.colorOverride[i] = static_cast<std::uint32_t>(v) & 0xFFFFFFu;
					if (!colorLog.empty()) {
						colorLog += ", ";
					}
					colorLog += kCategoryName[i];
					colorLog += "=#";
					char hexv[8];  // 不叫 buf：外层那个 buf[64] 是 getFloat 用的（C4456 遮蔽警告）
					std::snprintf(hexv, sizeof(hexv), "%06X", g_cfg.colorOverride[i]);
					colorLog += hexv;
				}
				if (!colorLog.empty()) {
					REX::INFO("config: colorOverride: {}", colorLog);
				}
			}

			REX::INFO("config: radius={:.1f}m targets={} hotkeyVK=0x{:X} startEnabled={}",
				g_cfg.radiusMeters, g_cfg.maxTargets, g_cfg.hotkeyVk, g_cfg.startEnabled);
			REX::INFO("config: unitsPerMeter={:.4f} -> 半径 {:.1f}m = {:.1f} 游戏单位",
				g_cfg.unitsPerMeter, g_cfg.radiusMeters, g_cfg.radiusMeters * g_cfg.unitsPerMeter);
			REX::INFO("config: onlyInFront={} frontFov={:.0f}deg reassert={} autoEnsure={} resyncOnScannerClose={} notifyOnToggle={}",
				g_cfg.onlyInFront, g_cfg.frontFovDeg,
				g_cfg.reassertMs ? std::to_string(g_cfg.reassertMs) + "ms" : std::string{ "off" },
				g_cfg.autoEnsureManagers, g_cfg.resyncOnScannerClose, g_cfg.notifyOnToggle);
			REX::INFO("config: unhighlightGrace={}ms maxOutlineOpsPerScan={}",
				g_cfg.unhighlightGraceMs, g_cfg.maxOutlineOpsPerScan);
			REX::INFO("config: exteriorContinuous={} settleOnCellCross={}ms streamJumpTolerance={} verify3DPerScan={}",
				g_cfg.exteriorContinuous, g_cfg.settleOnCellCrossMs,
				g_cfg.streamJumpTolerance, g_cfg.verify3DPerScan);

			// 分类分色：把「类别 → outline 状态」逐条打出来（调配色时一眼能对上）
			{
				std::string s;
				for (std::size_t i = 0; i < kCategoryCount; ++i) {
					if (i) {
						s += ", ";
					}
					s += kCategoryName[i];
					s += '=';
					s += std::to_string(g_cfg.stateByCategory[i]);
				}
				REX::INFO("config: stateByCategory: {}", s);
			}
		}

		// ====================================================================
		// 表单绑定（★ v4.0：只剩「游戏内提示」用的一个 GLOB）
		// ====================================================================
		// 只用一条通道：先按 EDID 拿 quest（上一代实测「QUST 的 EDID 能查到」），
		// 再从它的 FormID 取本 ESM 的 load order 前缀，最后 LookupByID 拿那个 GLOB。
		// 这样不依赖 TESDataHandler 的内存布局，也不依赖 GLOB 的 EDID 是否驻留。
		// ★ 绑定失败**不影响高亮**（高亮完全在 DLL 里），只是没有热键提示。
		bool BindNotify()
		{
			auto* quest = RE::TESForm::LookupByEditorID(RE::BSFixedString{ kBindQuestEdid });
			if (!quest) {
				const auto now = NowMs();
				if (now - g_state.lastBindWarnMs > kBindWarnIntervalMs) {
					g_state.lastBindWarnMs = now;
					REX::WARN("BindNotify: quest '{}' not found yet (plugin not loaded? wrong load order?)", kBindQuestEdid);
				}
				return false;
			}

			const std::uint32_t prefix = quest->GetFormID() & 0xFF000000u;
			auto*               notify = RE::TESForm::LookupByID<RE::TESGlobal>(prefix | kLocalNotify);
			if (!notify) {
				REX::WARN("BindNotify: SAS_Notify (prefix|0x{:X}) not found -> 热键提示不可用（高亮不受影响）",
					kLocalNotify);
				return false;
			}

			g_state.prefix = prefix;
			g_state.notify = notify;
			g_state.bound  = true;
			REX::INFO("forms bound: prefix={:08X} quest={:08X} notify={:08X} -> 热键提示通道就绪",
				prefix, quest->GetFormID(), notify->GetFormID());
			return true;
		}

		// 把「刚开 / 刚关」写进 SAS_Notify（桥脚本轮询到之后弹一条 HUD 提示并归零）。
		// ★ 为什么不让 DLL 自己弹提示：
		//   commonlibsf 的 `RE::DebugNotification` 的 REL::ID 是 0（未移植；实测该 ID
		//   在 1.16.244.0 的 Address Library 里指向的根本不是它），调用等于 call 到
		//   模块基址；走 VM 的 DispatchStaticCall 又要碰 BSTThreadScrapFunction 那一层，
		//   风险与收益不成比例。GLOB + 脚本轮询是零风险的等价做法。
		void FlushNotify()
		{
			if (g_state.pendingNotify == 0 || !g_state.notify) {
				return;
			}
			g_state.notify->value = static_cast<float>(g_state.pendingNotify);
			g_state.pendingNotify = 0;
			++g_state.notifyWrites;
		}

		// ====================================================================
		// 过滤规则（base form 类型 → 类别；-1 = 不高亮）
		// ====================================================================
		// NPC_ / ACHR / LVLN 一律排除 —— 「活人不亮」是红线。
		int ClassifyBase(const RE::TESForm* a_base)
		{
			if (!a_base) {
				return -1;
			}
			// ★「Glow*」家族（原版 MSTT 自发光网格）一律不高亮。
			//   它们本来是环境装饰用的自发光体（球 / 方块 / 圆盘 / 光锥），不是拾取物；
			//   另外 v2.4~v3.0 的自制面包屑珠子用的正是这几个形态 ——
			//   老存档里可能还留着几颗（脚本会做一次性清理，见 SAS_Bridge.psc），
			//   这里排除掉就不会把它们描上一圈。
			//   这几条 FormID 全在 Starfield.esm（高 8 位是 load order index，所以不会误伤）。
			switch (a_base->GetFormID()) {
			case 0x00098105u:  // MSTT GlowCube10x10
			case 0x00098106u:  // MSTT GlowBall10x10  <- v2.8 起面包屑用的形态
			case 0x0009811Cu:  // MSTT GlowDisc10x10
			case 0x0001760Fu:  // MSTT GlowLightCone36
				return -1;
			default:
				break;
			}
			switch (a_base->GetFormType()) {
			// ---- 可拾取 ----
			case RE::FormType::kMISC:
			case RE::FormType::kBOOK:
			case RE::FormType::kARMO:
			case RE::FormType::kWEAP:
			case RE::FormType::kAMMO:
			case RE::FormType::kALCH:
			case RE::FormType::kINGR:
			case RE::FormType::kKEYM:
			case RE::FormType::kNOTE:
			case RE::FormType::kSLGM:
				return static_cast<int>(Category::kLoot);
			// ---- 容器（搜刮的主要目标，值得单独一个颜色）----
			case RE::FormType::kCONT:
				return static_cast<int>(Category::kContainer);
			// ---- 可交互设备 ----
			case RE::FormType::kACTI:
			case RE::FormType::kTERM:
				return static_cast<int>(Category::kDevice);
			// ---- 门 ----
			case RE::FormType::kDOOR:
				return static_cast<int>(Category::kDoor);
			// ---- 植物（可采集）----
			case RE::FormType::kFLOR:
				return static_cast<int>(Category::kFlora);
			// ---- 其它：MSTT（MovableStatic）----
			//   ★ v4.1：默认被 `EnableOther=0` 关掉 —— MSTT 里是纸箱 / 桌椅 / 吧台 /
			//   飞船模块这类**不能拾取进背包**的装饰物（原版扫描仪也不亮它们）。
			//   能拾取进背包的杂物（咖啡杯 / 扳手 / 玩具…）全是 MISC，走上面的 kLoot。
			case RE::FormType::kMSTT:
				return static_cast<int>(Category::kOther);
			default:
				return -1;  // 其余（NPC_ / LVLN / …）一律不亮（红线）
			}
		}

		// ====================================================================
		// ★ v4.2：尸体 / 搜空
		// ====================================================================
		// 读 TESForm::formFlags（u32 @ +0x20）。
		//   ★ 只读**基类 TESForm 自己的成员**：本项目已验证 formType(+0x2E) / formID(+0x28)
		//     的运行期读法是准的（整个分类逻辑都建在它上面），formFlags 与它们同属 TESForm
		//     基类；而 ACHR 的「Starts Dead / Starts Unconscious」正是记录标志位。
		//     （下面有一条 static_assert 把 commonlibsf 声明的偏移钉死。）
		std::uint32_t RefFormFlags(const RE::TESObjectREFR* a_ref)
		{
			return *reinterpret_cast<const std::uint32_t*>(
				reinterpret_cast<const std::uint8_t*>(a_ref) + kOffFormFlags);
		}

		// 读 Actor::boolBits（u32 @ +0x208，bit11 = kDead）；只对 ACHR 引用有意义。
		std::uint32_t RefActorBoolBits(const RE::TESObjectREFR* a_ref)
		{
			return *reinterpret_cast<const std::uint32_t*>(
				reinterpret_cast<const std::uint8_t*>(a_ref) + kOffActorBoolBits);
		}

		// ★ v4.5：某个物品（TESForm*，来自 `BGSInventoryItem::object`）是不是
		//   「非玩家物品」（记录标志 bit2 = 0x04）—— 玩家拿不走、面板也不显示
		//   （判定依据与实证见常量区 `kFormFlagNonPlayable` 的长注释）。
		//   只对**已通过 IsPlausiblePointer 的指针**调用（与其它热路径读法一致）。
		bool IsNonPlayableForm(std::uint64_t a_obj)
		{
			return (*reinterpret_cast<const std::uint32_t*>(
						reinterpret_cast<const std::uint8_t*>(a_obj) + kOffFormFlags) &
					   kFormFlagNonPlayable) != 0;
		}

		// ★★ v4.4：读**引擎自己的 lifeState 枚举**（`[Actor+0xF8]` 的 bits 17..20，0..15）。
		//   `IsDead()` / `IsUnconscious()` / `IsBleedingOut()` 三个 Papyrus native 读的都是它
		//   （反汇编实证见文件顶部常量区的长注释），所以这是「引擎怎么判」的**权威来源**。
		std::uint32_t RefActorLifeState(const RE::TESObjectREFR* a_ref)
		{
			return (*reinterpret_cast<const std::uint32_t*>(
						reinterpret_cast<const std::uint8_t*>(a_ref) + kOffActorLifeState) &
					   kActorLifeStateMask) >>
				   kActorLifeStateShift;
		}

		// ★ v4.4：读「出血动画中」标志（`AIProcess::middleHigh(+0x08) + 0x59C`，bool）。
		//   `IsBleedingOut()` 的第二个条件。**只作探针**（理由见常量区注释）。
		//   读不到返回 -1。
		int RefMhBleedFlag(const RE::TESObjectREFR* a_ref)
		{
			const auto* raw  = reinterpret_cast<const std::uint8_t*>(a_ref);
			const auto  proc = *reinterpret_cast<const std::uint64_t*>(raw + kOffActorProcess);
			if (!IsPlausiblePointer(proc) ||
				!IsReadable(reinterpret_cast<const void*>(proc), kOffMiddleHigh + 8)) {
				return -1;
			}
			const auto mh = *reinterpret_cast<const std::uint64_t*>(
				reinterpret_cast<const std::uint8_t*>(proc) + kOffMiddleHigh);
			if (!IsPlausiblePointer(mh) ||
				!IsReadable(reinterpret_cast<const void*>(mh), kOffMhBleedFlag + 1)) {
				return -1;
			}
			return reinterpret_cast<const std::uint8_t*>(mh)[kOffMhBleedFlag] ? 1 : 0;
		}

		// ★ v4.4：ACHR 的「判决码」—— 判决变化探针（见 NoteAchrVerdict）用。
		//   0=活人 1=死/尸体 2=昏迷 3=出血。判定集合与 ClassifyRef 保持一致
		//   （这里不看 INI 开关：探针要能反映**引擎侧事实**）。
		std::uint8_t AchrVerdictCode(std::uint32_t a_ls, bool a_deadBit, bool a_startsDead, bool a_startsUnc)
		{
			if (a_deadBit || a_startsDead || a_ls == kLifeStateDeadA || a_ls == kLifeStateDeadB ||
				a_ls == kLifeStateDeadC) {
				return 1;
			}
			if (a_ls == kLifeStateUnconscious || a_startsUnc) {
				return 2;
			}
			if (a_ls == kLifeStateBleedA || a_ls == kLifeStateBleedB) {
				return 3;
			}
			return 0;
		}

		// ★ v4.3：前向声明 —— `ActorProbe` 要把「判空结果」也打进日志，而判空的
		//   实现（`g_invOff` / `RefLootState`）写在文件更下面（和 CalibrateInventory
		//   放在一起方便对照阅读）。同一个 TU 内前置声明即可。
		extern std::size_t g_invOff;  // NOLINT(readability-identifier-naming)
		int                RefLootState(const RE::TESObjectREFR* a_ref);

		// 诊断：本会话最多打 kCorpseProbeMax 条「尸体候选」探针。
		//   用途：用户报「某具尸体该亮却没亮 / 不该亮却亮了」时，把 ref / base / 原始
		//   formFlags / boolBits 打出来，和画面里的身体**按 FormID 对号入座**
		//   （上一代项目就是靠这类探针把「引擎没给它置 kDead 位」钉死的）。
		void CorpseProbe(const RE::TESObjectREFR* a_ref, const RE::TESForm* a_base,
			std::uint32_t a_flags, std::uint32_t a_bits, const char* a_kind)
		{
			if (g_state.corpseProbes >= kCorpseProbeMax) {
				return;
			}
			++g_state.corpseProbes;
			REX::INFO("corpse probe: ref={:08X} base={:08X} formFlags=0x{:08X} boolBits=0x{:08X} -> {}",
				a_ref->GetFormID(),
				a_base ? a_base->GetFormID() : 0u,
				a_flags,
				a_bits,
				a_kind);
		}

		// ★ v4.3 诊断：`actor probe:` —— 半径内**每一个 ACHR**（不论死活）的判决明细。
		//   为什么不止 corpse probe：用户报「有东西的尸体不亮」时，那具身体多半
		//   被判成了「活人」⇒ corpse probe 根本不会打它 ⇒ 日志里什么都看不到。
		//   这一条把 ACHR 的原始数据（formFlags / boolBits / lifeState / 库存指针）
		//   与最终结论一起打出来，和画面里的身体按 FormID 对号入座。
		//   ★ 纯内存读 + 形状校验；任何一步不过就打 `-`，**不影响判定**。
		//   ★ v4.4：v4.3 那条 `life=`（actor+0x228 → +0x10 → +0x264）已被**实证过的**
		//     `lifeState=`（[+0xF8] bits17..20）取代 —— 前者永远读不到（-1），因为
		//     真正的枚举在 Actor 自己身上，不在 AIProcess 里。另外把
		//     `IsBleedingOut()` 的第二个条件 `mh=`（middleHigh+0x59C）也带上。
		//   ★ `a_force = true` 时不受 `ActorProbeMax` 限制（判决变化探针专用）。
		void ActorProbe(const RE::TESObjectREFR* a_ref, float a_distSq, const char* a_verdict,
			bool a_force = false, const char* a_tag = "actor probe")
		{
			if (!a_force) {
				if (g_cfg.actorProbeMax <= 0) {
					return;
				}
				if (g_state.actorProbes >= static_cast<std::uint32_t>(g_cfg.actorProbeMax)) {
					return;
				}
				++g_state.actorProbes;
			}

			const auto  bits  = RefActorBoolBits(a_ref);
			const auto  flags = RefFormFlags(a_ref);
			const auto  ls    = RefActorLifeState(a_ref);
			const int   mh    = RefMhBleedFlag(a_ref);
			const auto* raw   = reinterpret_cast<const std::uint8_t*>(a_ref);

			std::uint64_t invPtr = 0;
			int           loot   = -3;
			if (g_invOff != 0) {
				invPtr = *reinterpret_cast<const std::uint64_t*>(raw + g_invOff);
				loot   = RefLootState(a_ref);
			}
			const auto* base = a_ref->data.objectReference.get();

			REX::INFO("{}: ref={:08X} base={:08X} d={:.1f}m formFlags=0x{:08X} boolBits=0x{:08X} "
					  "lifeState={} dead={} startsDead={} startsUnc={} mh={} inv={} loot={} -> {}",
				a_tag,
				a_ref->GetFormID(),
				base ? base->GetFormID() : 0u,
				std::sqrt(std::max(a_distSq, 0.0f)) / g_cfg.unitsPerMeter,
				flags, bits,
				ls,
				(bits & kActorDeadBit) ? 1 : 0,
				(flags & kFormFlagStartsDead) ? 1 : 0,
				(flags & kFormFlagStartsUnconscious) ? 1 : 0,
				mh,
				invPtr ? "ok" : "null",
				loot,
				a_verdict);
		}

		// ★ v4.4 诊断：`actor probe (changed):` —— ACHR 的判决**发生变化**时打一条。
		//   为什么需要它（这是本轮诊断的关键设计）：v4.3 的 `actor probe` 是
		//   「每会话前 N 条」，而它在**进入世界的第一轮**就被打了 24 条 ⇒ 用户之后
		//   打死敌人时，那具身体早就没有探针额度了 —— 日志里永远看不到「那一刻」。
		//   现在改成**事件驱动**：只在判决真的变了（活人→尸体 / 尸体→活人）时打，
		//   于是「开枪打死一个海盗」会立刻在日志里留下它当时的
		//   boolBits / lifeState / StartsDead 状态 —— 一眼就能分辨是「判死漏了」
		//   还是「判空误判」。
		//   ★ 首次见到某个 ACHR 只记录不打（否则新进入半径的单位会刷屏）。
		//   ★ 额度独立（`ActorChangeProbeMax`），不受 `ActorProbeMax` 影响。
		void NoteAchrVerdict(const RE::TESObjectREFR* a_ref, float a_distSq, std::uint8_t a_code,
			std::uint32_t a_ls, bool a_deadBit, bool a_startsDead, bool a_startsUnc)
		{
			auto it = g_state.achrVerdict.find(a_ref);
			if (it == g_state.achrVerdict.end()) {
				if (g_state.achrVerdict.size() > 8192) {
					g_state.achrVerdict.clear();  // 保险：正常换场景会清，这里兜住异常增长
				}
				g_state.achrVerdict.emplace(a_ref, a_code);
				return;
			}
			if (it->second == a_code) {
				return;
			}
			const auto prev = it->second;
			it->second      = a_code;
			if (g_cfg.actorChangeProbeMax <= 0 ||
				g_state.actorChangeProbes >= static_cast<std::uint32_t>(g_cfg.actorChangeProbeMax)) {
				return;
			}
			++g_state.actorChangeProbes;
			// 判决码 0=活人 1=死/尸体 2=昏迷 3=出血（与 AchrVerdictCode 一致）
			const char* const kCodeName[4] = { "活人", "尸体", "昏迷", "出血" };
			const auto        p            = prev < 4 ? kCodeName[prev] : "?";
			const auto        c            = a_code < 4 ? kCodeName[a_code] : "?";
			char              why[192];
			std::snprintf(why, sizeof(why),
				"判决变化：%s -> %s（lifeState=%u dead=%d startsDead=%d startsUnc=%d）",
				p, c, a_ls, a_deadBit ? 1 : 0, a_startsDead ? 1 : 0, a_startsUnc ? 1 : 0);
			ActorProbe(a_ref, a_distSq, why, /*a_force=*/true, "actor probe (changed)");
		}

		// ★ v4.2：按**引用自己**（而不是只看 base）分类。
		//   与 v4.1 的差别只有一条：ACHR 引用不再「一律不亮」，而是分活 / 死 ——
		//     ① 死（运行时 kDead 位 **或** 记录标志 Starts Dead）⇒ **尸体**（可搜刮）；
		//     ② 活 ⇒ 仍然一个都不亮（红线，不要放宽）。
		//   另外「base = NPC_/LVLN 的普通 REFR」= Starfield 摆的「姿势固定的尸体道具」
		//   （它不是 Actor、没有 AI/进程，不可能站起来），进尸体类。
		//   a_corpse 回传「这条是尸体」——调用方据此做**判空**（容器 / 尸体都要判空）。
		//   ★ v4.3：新增 `a_distSq`（仅用于 actor probe 的距离显示）——探针要能和
		//     画面里的身体对上号，「离玩家多远」是最快的定位手段。
		//   ★ v4.3：ACHR 不论死活都会打一条 `actor probe:`（每会话上限 cfg.actorProbeMax）——
		//     「该亮没亮的尸体」多半被判成了活人，只有这条探针能看到它。
		int ClassifyRef(const RE::TESObjectREFR* a_ref, const RE::TESForm* a_base, float a_distSq, bool& a_corpse)
		{
			a_corpse = false;
			if (!a_ref) {
				return -1;
			}
			const auto* raw     = reinterpret_cast<const std::uint8_t*>(a_ref);
			const auto  refType = raw[kOffFormType];

			if (refType == kFormTypeACHR) {
				const std::uint32_t bits       = RefActorBoolBits(a_ref);
				const std::uint32_t flags      = RefFormFlags(a_ref);
				const std::uint32_t ls         = RefActorLifeState(a_ref);
				const bool          deadBit    = (bits & kActorDeadBit) != 0;
				const bool          startsDead = (flags & kFormFlagStartsDead) != 0;
				const bool          startsUnc  = (flags & kFormFlagStartsUnconscious) != 0;
				// ★★ v4.4：引擎自己的生命状态（**权威判据** —— 反汇编实证见常量区注释）。
				//   这是本轮修「打死的敌人不亮」的核心：`kDead` 位不可靠（实测 450 次
				//   尸体判定里一个都没读到它），而 `lifeState` 是引擎 `IsDead()` 自己读的东西。
				const bool lsDead  = g_cfg.corpseLifeState &&
									(ls == kLifeStateDeadA || ls == kLifeStateDeadB || ls == kLifeStateDeadC);
				const bool lsUnc   = g_cfg.corpseUnconscious && (ls == kLifeStateUnconscious);
				const bool lsBleed = g_cfg.corpseBleedout &&
									 (ls == kLifeStateBleedA || ls == kLifeStateBleedB);

				++g_state.achrSeen;
				// ★ v4.4：判决变化探针（先记录，只有变化时才打日志；见 NoteAchrVerdict）
				NoteAchrVerdict(a_ref, a_distSq, AchrVerdictCode(ls, deadBit, startsDead, startsUnc),
					ls, deadBit, startsDead, startsUnc);

				if (deadBit || startsDead || lsDead || lsUnc || lsBleed) {
					a_corpse = true;
					++g_state.corpseSeen;
					if (deadBit) {
						++g_state.corpseByBit;
					}
					if (startsDead) {
						++g_state.corpseByFlag;
					}
					if (lsDead) {
						++g_state.corpseByLife;
					}
					if (lsBleed) {
						++g_state.corpseByBleed;
					}
					if (lsUnc) {
						++g_state.corpseUncSeen;
					}
					// 探针的文案按「优先级」给出最可能的来源（多路同时成立时按重要性取一条）
					const char* kind = "尸体";
					if (deadBit) {
						kind = "尸体（运行时 kDead 位）";
					} else if (lsDead) {
						kind = "尸体（lifeState = 引擎 IsDead）";
					} else if (startsDead) {
						kind = "尸体（记录标志 Starts Dead）";
					} else if (lsBleed) {
						kind = "尸体（lifeState = 引擎 IsBleedingOut：倒地出血）";
					} else if (lsUnc) {
						kind = "尸体（lifeState = 引擎 IsUnconscious：昏迷/报废体）";
					}
					CorpseProbe(a_ref, a_base, flags, bits, kind);
					ActorProbe(a_ref, a_distSq, kind);
					return static_cast<int>(Category::kCorpse);
				}
				if (startsUnc) {
					++g_state.corpseUncSkipped;
					if (g_cfg.corpseUnconscious) {
						a_corpse = true;
						++g_state.corpseSeen;
						++g_state.corpseUncSeen;
						CorpseProbe(a_ref, a_base, flags, bits,
							"尸体（Starts Unconscious：炮塔/机器人报废体、倒地可搜刮者）");
						ActorProbe(a_ref, a_distSq, "尸体（Starts Unconscious 标志）");
						return static_cast<int>(Category::kCorpse);
					}
					CorpseProbe(a_ref, a_base, flags, bits,
						"跳过：Starts Unconscious（倒地可搜刮，但被 CorpseUnconscious=0 关掉了）");
					ActorProbe(a_ref, a_distSq, "跳过：Starts Unconscious（CorpseUnconscious=0）");
					return -1;
				}
				// 活着的 Actor（正在行动的 NPC / 生物）—— 红线：不亮
				//   ★ v4.3：「某具尸体该亮却没亮」十有八九落在这里 ⇒ 计一笔 + 打探针。
				//   ★ v4.4：如果这一条**发生了 lifeState 变化**（比如刚被打死），
				//     NoteAchrVerdict 已经在上面把当时的完整状态打进日志了。
				++g_state.achrLive;
				ActorProbe(a_ref, a_distSq, "活人（不进候选）");
				return -1;
			}

			// ---- 非 ACHR：base 是 NPC_ / LVLN ⇒ 「姿势固定的尸体道具」----
			if (a_base) {
				const auto bt = static_cast<std::uint8_t>(a_base->GetFormType());
				if (bt == static_cast<std::uint8_t>(RE::FormType::kNPC_) ||
					bt == static_cast<std::uint8_t>(RE::FormType::kLVLN)) {
					a_corpse = true;
					++g_state.corpseSeen;
					++g_state.corpseProps;
					CorpseProbe(a_ref, a_base, RefFormFlags(a_ref), 0,
						"尸体道具（base = NPC_/LVLN 的普通 REFR）");
					return static_cast<int>(Category::kCorpse);
				}
			}
			return ClassifyBase(a_base);
		}

		// ★★ v4.4 诊断：`loot probe:` —— 「库存明细」探针（本轮的**判空取证主力**）。
		//   背景：用户报「预先放置的尸体拿空了物品还在高亮」。判空链路（RefLootState）
		//   只会给出「有/空/未知」，看不到**残留的是什么** —— 而这正是关键：
		//     · 若残留条目的 count 真的 > 0 ⇒ 引擎库存里确实还有东西（面板空只是
		//       「玩家拿不走 / 面板不显示」），要把它们从判定里排除（下一轮按这份数据修）；
		//     · 若读到的 count 全是 0 却仍返回「有」⇒ 是我们自己的读取 bug（这一行能证伪）。
		//   触发条件：**近距离（≤ kLootProbeRangeMeters）的尸体**、判 notEmpty、
		//   每个 ref 只打一次、每会话上限 `LootProbeMax`。
		//   打印：`ref / d / size（条目数）/ playable=总数 / equipped=总数 / nonPlayable=总数`
		//   （★ v4.6：三桶 = 真能拿 / 装备中 / 0x04 非玩家物品）
		//   + 前几个非空条目的 `[ft=类型 id=FormID n=stack数 c=count fl=条目flags ff=记录flags eq= np=]`。
		//   ★ 纯内存读 + 形状校验；读不到就打原因，**绝不改判定**。
		void LootProbe(const RE::TESObjectREFR* a_ref, float a_distSq)
		{
			if (g_cfg.lootProbeMax <= 0 || g_invOff == 0) {
				return;
			}
			if (g_state.lootProbes >= static_cast<std::uint32_t>(g_cfg.lootProbeMax)) {
				return;
			}
			const float limitUnits = kLootProbeRangeMeters * g_cfg.unitsPerMeter;
			if (a_distSq > limitUnits * limitUnits) {
				return;
			}
			if (g_state.lootProbed.find(a_ref) != g_state.lootProbed.end()) {
				return;
			}
			g_state.lootProbed.insert(a_ref);
			++g_state.lootProbes;

			const auto* raw = reinterpret_cast<const std::uint8_t*>(a_ref);
			const auto  fid = a_ref->GetFormID();
			const auto  dM  = std::sqrt(std::max(a_distSq, 0.0f)) / g_cfg.unitsPerMeter;

			const auto ptrRaw = *reinterpret_cast<const std::uint64_t*>(raw + g_invOff);
			if (ptrRaw == 0) {
				REX::INFO("loot probe: ref={:08X} d={:.1f}m inv=null（引擎没给这个引用建过库存）", fid, dM);
				return;
			}
			if (!IsPlausiblePointer(ptrRaw)) {
				REX::INFO("loot probe: ref={:08X} d={:.1f}m inv=垃圾指针 0x{:X}", fid, dM, ptrRaw);
				return;
			}
			const auto* inv  = reinterpret_cast<const std::uint8_t*>(ptrRaw);
			const auto  size = *reinterpret_cast<const std::uint32_t*>(inv + kOffInvData);
			const auto  cap  = *reinterpret_cast<const std::uint32_t*>(inv + kOffInvData + 4);
			const auto  data = *reinterpret_cast<const std::uint64_t*>(inv + kOffInvData + 8);
			if (size > kInvMaxItems || cap < size || cap > (1u << 20) ||
				(size != 0 && !IsPlausiblePointer(data))) {
				REX::INFO("loot probe: ref={:08X} d={:.1f}m 形状不对（size={} cap={} data=0x{:X}）",
					fid, dM, size, cap, data);
				return;
			}

			std::string   detail;
			// ★ v4.5 / ★ v4.6：把「有 count>0 的条目」拆成**三桶**（= 判空的真实口径）——
			//   `playable`    = 玩家**真能拿走**的（判空只看它：`fl=0` 且 `ff` 不含 0x04）
			//   `equipped`    = **装备中**（`fl` 低 3 位 ≠ 0）：v4.6 起判空跳过
			//   `nonPlayable` = 记录标志 0x04 的 NPC 隐形装备：v4.5 起判空跳过
			//   这样一行日志就能验证「修好了没有」：面板拿空后 `playable` 应变成 0，
			//   剩下的应该全落在 `equipped` / `nonPlayable` 这两桶里。
			std::uint32_t playable    = 0;
			std::uint64_t total       = 0;
			std::uint32_t equipped    = 0;
			std::uint64_t eqTotal     = 0;
			std::uint32_t nonPlayable = 0;
			std::uint64_t npTotal     = 0;
			const auto    items       = std::min<std::uint32_t>(size, kInvWalkItemsMax);
			for (std::uint32_t i = 0; i < items; ++i) {
				const auto* item = reinterpret_cast<const std::uint8_t*>(data) + i * kOffInvItemSize;
				const auto  obj  = *reinterpret_cast<const std::uint64_t*>(item + kOffInvItemObject);
				if (!IsPlausibleFormPtr(obj)) {
					continue;
				}
				const auto sn = *reinterpret_cast<const std::uint32_t*>(item + kOffInvItemStacks);
				if (sn == 0) {
					continue;
				}
				const auto sd = *reinterpret_cast<const std::uint64_t*>(item + kOffInvItemStacks + 8);
				if (!IsPlausiblePointer(sd)) {
					continue;
				}
				std::uint64_t sum = 0;
				for (std::uint32_t j = 0; j < sn; ++j) {
					sum += *reinterpret_cast<const std::uint32_t*>(
						reinterpret_cast<const std::uint8_t*>(sd) + j * kOffStackSize + kOffStackCount);
				}
				if (sum == 0) {
					continue;
				}
				const auto  objFlags  = *reinterpret_cast<const std::uint32_t*>(
					reinterpret_cast<const std::uint8_t*>(obj) + kOffFormFlags);
				// item flags（BGSInventoryItem::flags @+0x20，u32）：
				//   低 3 位 = 装备槽（非 0 = 正在装备中），bit3 = kEquipStateLocked，
				//   bit5 = kTemporary
				const auto itemFlags = *reinterpret_cast<const std::uint32_t*>(item + kOffInvItemFlags);
				const bool np = IsNonPlayableForm(obj);
				const bool eq = !np && (itemFlags & kInvItemFlagSlotMask) != 0;
				if (np) {
					++nonPlayable;
					npTotal += sum;
				} else if (eq) {
					++equipped;
					eqTotal += sum;
				} else {
					++playable;
					total += sum;
				}
				// 明细：真能拿的列前 4 条、装备中的列前 2 条、非玩家物品列前 2 条
				//   （每条都带 `fl=` 原始 flags、`eq=` 是否装备中、`np=` 是否非玩家物品）
				if ((!np && !eq && playable <= 4) || (eq && equipped <= 2) || (np && nonPlayable <= 2)) {
					const auto* objRaw  = reinterpret_cast<const std::uint8_t*>(obj);
					const auto  objType = objRaw[kOffFormType];
					const auto  objFid  = reinterpret_cast<const RE::TESForm*>(obj)->GetFormID();
					char buf[160];
					std::snprintf(buf, sizeof(buf), " [ft=%02X id=%08X n=%u c=%llu fl=%X ff=%X eq=%d np=%d]",
						static_cast<unsigned>(objType),
						static_cast<unsigned>(objFid),
						static_cast<unsigned>(sn),
						static_cast<unsigned long long>(sum),
						static_cast<unsigned>(itemFlags),
						static_cast<unsigned>(objFlags),
						eq ? 1 : 0,
						np ? 1 : 0);
					detail += buf;
				}
			}
			REX::INFO("loot probe: ref={:08X} d={:.1f}m size={} playable={} total={} equipped={} eqTotal={} nonPlayable={} npTotal={}{}",
				fid, dM, size, playable, static_cast<unsigned long long>(total),
				equipped, static_cast<unsigned long long>(eqTotal),
				nonPlayable, static_cast<unsigned long long>(npTotal), detail);
		}

		// ★★ v4.8 诊断：`cont probe:` —— 容器「判空链路快照」。
		//   背景（用户实测反馈）：*「这个武器箱子表现有点奇怪，关闭的时候没有高亮，
		//   只有打开的时候才会开始高亮」* —— 也就是：**关着的容器不进候选（不亮），
		//   一打开（搜刮界面出来）就亮**。要一次定位到环节：
		//     ① **判空误判**（最可疑）：关着时我们读到的就是「空」。细分四种形态：
		//        · `inv=null`（引擎没建库存；`TreatNullInvAsEmpty=1` ⇒ 判空）
		//        · `size=0`（数组里一条都没有）
		//        · 有条目但所有 stack 的 count 都是 0
		//        · 有条目、count 也 > 0，但**全被两条跳过规则跳过**
		//          （`SkipNonPlayableLoot` 的 0x04 / `SkipEquippedLoot` 的 kSlotMask）
		//          ★ 这两条规则**只在尸体上做过实证**（v4.5 / v4.6），
		//          容器里的物品是否同样成立 —— 这次正好第一次有数据。
		//     ② 别的环节（分类 / 类别开关 / 距离 / 3D）—— 那 probe 也不白打：
		//        「能读到条目明细」本身就证明它走到了判空这一步，是 ① 的证据。
		//   触发：`kContProbeRangeMeters` 内的**容器**（CONT）；
		//   每个 ref 打「首次快照」一条 + 判决变化最多 `kContProbeChangeMax` 条
		//   （走到箱子旁 → 打开 → 关掉，正好是 1 + 2 条），会话总量上限
		//   `cfg.contProbeMax`。对照「开关前后」的 `size / sum / skipNp / skipEq`
		//   即可定论；**关掉搜刮界面后判决有没有变回「空」**同样是关键证据。
		//   ★ 纯内存读 + 形状校验，**绝不改判定**；读不到就打原因。
		//   ★ v4.9：新增 `a_displayCase` —— `dc=1` 表示该容器是「展示柜」（白名单 /
		//     运行期学习，见常量区「v4.9 展示柜」）。它的 `size=0` 是**常态**
		//     （内容只在搜刮界面打开期间投影进来），不参与「搜空熄灭」判定。
		void ContProbe(const RE::TESObjectREFR* a_ref, const RE::TESForm* a_base, float a_distSq,
			int a_loot, bool a_displayCase)
		{
			if (g_cfg.contProbeMax <= 0) {
				return;
			}
			const float limitUnits = kContProbeRangeMeters * g_cfg.unitsPerMeter;
			if (a_distSq > limitUnits * limitUnits) {
				return;
			}
			if (g_state.contProbes >= static_cast<std::uint32_t>(g_cfg.contProbeMax)) {
				return;
			}
			auto it = g_state.contProbed.find(a_ref);
			if (it == g_state.contProbed.end()) {
				if (g_state.contProbed.size() >= 256) {
					return;  // 兜底：别让这张表无限长（正常远用不到）
				}
				it = g_state.contProbed.emplace(a_ref, State::ContProbeRec{}).first;
			}
			auto&      rec   = it->second;
			const bool first = !rec.seen;
			if (!first && (rec.lastVerdict == a_loot || rec.changes >= kContProbeChangeMax)) {
				return;  // 判决没变 / 这个 ref 的「变化」条数已经打够
			}
			rec.lastVerdict = a_loot;
			rec.seen        = true;
			if (!first) {
				++rec.changes;
			}
			++g_state.contProbes;

			const auto fid = a_ref->GetFormID();
			const auto bid = a_base ? a_base->GetFormID() : 0;
			const auto dM  = std::sqrt(std::max(a_distSq, 0.0f)) / g_cfg.unitsPerMeter;
			// ★ v4.9：`dc=1` = 该容器被判为「展示柜」（静态白名单或运行期学习集合，
			//   见常量区「v4.9 展示柜」）—— 它的「空」不会被用来熄灭高亮。
			const char* tag = a_displayCase ?
				(first ? " dc=1" : " dc=1 (changed)") :
				(first ? "" : " (changed)");
			const auto* raw     = reinterpret_cast<const std::uint8_t*>(a_ref);

			if (g_invOff == 0) {
				REX::INFO("cont probe{}: ref={:08X} base={:08X} d={:.1f}m loot={} -> 库存偏移还没标定（不判空，容器照常亮）",
					tag, fid, bid, dM, a_loot);
				return;
			}

			const auto ptrRaw = *reinterpret_cast<const std::uint64_t*>(raw + g_invOff);
			if (ptrRaw == 0) {
				REX::INFO("cont probe{}: ref={:08X} base={:08X} d={:.1f}m loot={} inv=null -> 判空原因=引擎没给这个引用建库存{}",
					tag, fid, bid, dM, a_loot,
					g_cfg.treatNullInvAsEmpty ? "（TreatNullInvAsEmpty=1 ⇒ 判「空」）" : "（TreatNullInvAsEmpty=0 ⇒ 按未知处理）");
				return;
			}
			if (!IsPlausiblePointer(ptrRaw)) {
				REX::INFO("cont probe{}: ref={:08X} base={:08X} d={:.1f}m loot={} inv=垃圾指针 0x{:X}（判空链路返回「未知」）",
					tag, fid, bid, dM, a_loot, ptrRaw);
				return;
			}
			const auto* inv  = reinterpret_cast<const std::uint8_t*>(ptrRaw);
			const auto  size = *reinterpret_cast<const std::uint32_t*>(inv + kOffInvData);
			const auto  cap  = *reinterpret_cast<const std::uint32_t*>(inv + kOffInvData + 4);
			const auto  data = *reinterpret_cast<const std::uint64_t*>(inv + kOffInvData + 8);
			if (size > kInvMaxItems || cap < size || cap > (1u << 20) ||
				(size != 0 && !IsPlausiblePointer(data))) {
				REX::INFO("cont probe{}: ref={:08X} base={:08X} d={:.1f}m loot={} 形状不对（size={} cap={} data=0x{:X}）",
					tag, fid, bid, dM, a_loot, size, cap, data);
				return;
			}
			if (size == 0) {
				REX::INFO("cont probe{}: ref={:08X} base={:08X} d={:.1f}m loot={} inv=ok size=0 -> 判空原因=库存数组为空（一条都没有）",
					tag, fid, bid, dM, a_loot);
				return;
			}

			// 逐条摊开（最多 kInvWalkItemsMax 条；明细只列前 4 条）。
			//   口径与 RefLootState 完全一致：先判 np（0x04）再判 eq（kSlotMask）。
			std::uint32_t withCount = 0;  // 有条目 count > 0 的条目数
			std::uint32_t keepCnt   = 0;  // 判空口径里「算数」的条目数（可拿）
			std::uint64_t sumRaw    = 0;  // 所有条目 count 之和
			std::uint64_t sumKeep   = 0;  // 判空口径里「算数」的 count 之和
			std::uint32_t skipNp    = 0;
			std::uint32_t skipEq    = 0;
			std::string   detail;
			char          buf[192];
			const auto    items = std::min<std::uint32_t>(size, kInvWalkItemsMax);
			for (std::uint32_t i = 0; i < items; ++i) {
				const auto* item = reinterpret_cast<const std::uint8_t*>(data) + i * kOffInvItemSize;
				const auto  obj  = *reinterpret_cast<const std::uint64_t*>(item + kOffInvItemObject);
				if (!IsPlausibleFormPtr(obj)) {
					continue;
				}
				const auto sn = *reinterpret_cast<const std::uint32_t*>(item + kOffInvItemStacks);
				std::uint64_t sum = 0;
				if (sn > 0 && sn <= kInvMaxStacks) {
					const auto sd = *reinterpret_cast<const std::uint64_t*>(item + kOffInvItemStacks + 8);
					if (IsPlausiblePointer(sd)) {
						for (std::uint32_t j = 0; j < sn; ++j) {
							sum += *reinterpret_cast<const std::uint32_t*>(
								reinterpret_cast<const std::uint8_t*>(sd) + j * kOffStackSize + kOffStackCount);
						}
					}
				}
				const auto itemFlags = *reinterpret_cast<const std::uint32_t*>(item + kOffInvItemFlags);
				const bool np        = IsNonPlayableForm(obj);
				const bool eq        = !np && (itemFlags & kInvItemFlagSlotMask) != 0;
				sumRaw += sum;
				if (sum > 0) {
					++withCount;
				}
				if (np) {
					++skipNp;
				} else if (eq) {
					++skipEq;
				} else {
					++keepCnt;
					sumKeep += sum;
				}
				if (i < 4) {
					const auto* objRaw  = reinterpret_cast<const std::uint8_t*>(obj);
					const auto  objType = objRaw[kOffFormType];
					const auto  objFid  = reinterpret_cast<const RE::TESForm*>(obj)->GetFormID();
					const auto  objFlg  = *reinterpret_cast<const std::uint32_t*>(objRaw + kOffFormFlags);
					// ★ v4.9：明细里加 `tp=`（kTemporary，fl 的 bit5）—— 展示柜的
					//   临时投影条目一眼可辨（`fl=20` 时 tp=1），见常量区「v4.9 展示柜」。
					std::snprintf(buf, sizeof(buf),
						" [i=%u ft=%02X id=%08X n=%u c=%llu fl=%X ff=%X eq=%d np=%d tp=%d]",
						static_cast<unsigned>(i),
						static_cast<unsigned>(objType),
						static_cast<unsigned>(objFid),
						static_cast<unsigned>(sn),
						static_cast<unsigned long long>(sum),
						static_cast<unsigned>(itemFlags),
						static_cast<unsigned>(objFlg),
						eq ? 1 : 0,
						np ? 1 : 0,
						(itemFlags & kInvItemFlagTemporary) ? 1 : 0);
					detail += buf;
				}
			}

			// 交叉核对：把「探针自己数出来的口径」与「RefLootState 的返回值」并排打出来。
			//   两边不一致 ⇒ 直接说明判定链路里有 bug（这行就是证据）。
			char whyBuf[192];
			if (a_loot == 1) {
				std::snprintf(whyBuf, sizeof(whyBuf), "有东西（进候选、会亮）");
			} else if (sumKeep > 0) {
				std::snprintf(whyBuf, sizeof(whyBuf),
					"探针数到 %llu 个可拿的东西，但判定 loot=%d ⇒ 两边不一致（把这行发出来）",
					static_cast<unsigned long long>(sumKeep), a_loot);
			} else if (a_displayCase) {
				std::snprintf(whyBuf, sizeof(whyBuf),
					"展示柜：读到「空」不算空（关闭时内容不在库存里）⇒ 跳过判空、照常亮");
			} else if (sumRaw == 0) {
				std::snprintf(whyBuf, sizeof(whyBuf),
					"所有条目 count 都是 0（条目数=%u，其中被跳过 np=%u eq=%u）",
					size, skipNp, skipEq);
			} else if (skipNp + skipEq > 0) {
				std::snprintf(whyBuf, sizeof(whyBuf),
					"有条目的 count>0，但全被跳过（np=%u eq=%u，原始合计=%llu）",
					skipNp, skipEq, static_cast<unsigned long long>(sumRaw));
			} else {
				std::snprintf(whyBuf, sizeof(whyBuf),
					"其它（条目=%u 有数=%u 原始合计=%llu 可拿合计=%llu）",
					size, withCount, static_cast<unsigned long long>(sumRaw),
					static_cast<unsigned long long>(sumKeep));
			}
			REX::INFO("cont probe{}: ref={:08X} base={:08X} d={:.1f}m loot={} size={} keep={}(sum={}) withCount={} sumRaw={} skipNp={} skipEq={} why={}{}",
				tag, fid, bid, dM, a_loot, size, keepCnt,
				static_cast<unsigned long long>(sumKeep), withCount,
				static_cast<unsigned long long>(sumRaw), skipNp, skipEq, whyBuf, detail);
		}

		// 【v4.2 / v4.3】标定「库存列表指针」在 TESObjectREFR 里的真实偏移（0xA0 / 0xA8）。
		//   为什么不能直接用头文件：`inventoryList` 是
		//   `BSGuarded<BGSInventoryList*, BSReadWriteLock>`，而 commonlibsf 里
		//   BSGuarded 的两个成员标着 "??"（data 在前还是锁在前不确定），两个成员都是
		//   8 字节 ⇒ 只有两个候选。**不猜**：采样做形状校验，谁全过就用谁。
		//
		//   ★ 2026-09-18 反汇编实证（`Starfield.exe` 1.16.244.0）：
		//     · vtable slot 0xF1 `DestroyInventoryList` 开头就是
		//       `lea rsi, [rcx+0xA0] / lea r15, [rsi+8]`，随后对 r15 加锁、
		//       用 `[rsi]` 读旧列表指针 ⇒ **数据指针在 ref+0xA0、锁在 ref+0xA8**；
		//     · 同一段里 `mov ecx, 0x40` 分配 `BGSInventoryList`，构造时
		//       `data(BSTArray) @+0x28 / ownerHandle @+0x38 / cachedWeight @+0x3C`
		//       —— 与 commonlibsf 声明完全一致。
		//     ⇒ 真实答案就是 **+0xA0**；0xA8 只是"万一偏移假设反了"的备胎。
		//
		//   形状校验（每个样本）：① ref+off 是合理指针；② 可读 0x38 字节；
		//   ③ +0x28 处的 BSTArray 三元组自洽（size <= cap <= 1M）；④ 非空时 data 可读；
		//   ⑤ ★ v4.3 新增**强证据**：第一条目的 `object` 必须是一个合法 TESForm
		//      （`IsPlausibleFormPtr`）——「指针碰巧可读」太弱，这一条才能证明
		//      「这个偏移上真的是一份物品清单」。
		//
		//   ★ v4.3 的两处行为修正（都是实测暴露的坑）：
		//     ① 不再「第一轮 6 个样本一锤定音 + 失败永久降级」——改成**跨轮攒样本**
		//        （每轮最多 4 个），**永不放弃**（长时间攒不到就降频到每 4 秒试一次）；
		//     ② 样本从「只认容器」放宽到「CONT 或 ACHR」——两者用的是同一个偏移
		//        （上面反汇编证实），玩家的第一个场景没有容器时也能标定出来。
		//   ★ 底线不变：标定没成功 ⇒ 一律返回「未知」⇒ 不判空（容器 / 尸体照常亮），
		//     **绝不让没验证的偏移进热路径**（上一代项目 v32 的教训）。
		std::size_t g_invOff = 0;  // 0 = 还没标定
		// ★ v4.3：采样节流 —— 长时间攒不到样本时降频（见 CalibrateInventory 顶部说明）
		std::uint64_t g_invCalibNextMs = 0;
		void CalibrateInventory(const RE::TESObjectREFR* const* a_list, std::uint32_t a_size)
		{
			auto& s = g_state;
			if (s.invCalibDone) {
				return;
			}
			// ★ v4.3：节流 + **永不放弃**。
			//   前 kInvCalibFastRounds 轮每轮都采样（正常场景 1~2 秒就能攒够）；之后
			//   降频到每 kInvCalibSlowMs 毫秒采一次。为什么不放弃：玩家可能一直在
			//   太空 / 飞船里（那里没有容器也没有 ACHR）—— 那时样本永远攒不够，
			//   但**落地 / 进屋 / 读档之后就该能用**，不能因为"开局在太空"就整局降级。
			//   降频后的开销：每 4 秒一次「扫几千个引用里的 4 个样本」≈ 微秒级。
			const auto now = NowMs();
			if (now < g_invCalibNextMs) {
				return;
			}
			g_invCalibNextMs = now + (s.invRounds >= kInvCalibFastRounds ? kInvCalibSlowMs : 0);
			++s.invRounds;

			// 只在前 kInvCalibScanCap 个引用里找样本：当前 cell 容器少时
			// （太空站内部、纯地形…）也不至于每轮把整张引用表扫一遍。
			//   ★ v4.3：样本来源从「只认容器」放宽到「任何有库存的引用」——
			//     CONT 与 ACHR 的 `inventoryList` 在**同一个偏移**上（2026-09-18
			//     反汇编 `DestroyInventoryList`/`CreateInventoryList` 已证实：
			//     数据指针 @+0xA0、锁 @+0xA8），所以两者都能当样本。
			//     只采样 kInvCalibPerRound 个/轮 —— 单轮最多这么几次 VirtualQuery。
			const std::uint32_t limit   = std::min<std::uint32_t>(a_size, kInvCalibScanCap);
			std::uint32_t       sampled = 0;
			for (std::uint32_t i = 0; i < limit && sampled < kInvCalibPerRound; ++i) {
				auto* ref = a_list[i];
				if (!ref || !IsPlausiblePointer(reinterpret_cast<std::uint64_t>(ref))) {
					continue;
				}
				const auto* refRaw  = reinterpret_cast<const std::uint8_t*>(ref);
				const auto  refType = refRaw[kOffFormType];
				const auto* base    = ref->data.objectReference.get();
				const bool  isCont  = base && base->GetFormType() == RE::FormType::kCONT;
				const bool  isActor = (refType == kFormTypeACHR);
				if (!isCont && !isActor) {
					continue;  // 只认「容器」与「Actor」（纯内存读判类型）
				}
				++sampled;
				++s.invChecks;
				for (int c = 0; c < 2; ++c) {
					const auto p = *reinterpret_cast<const std::uint64_t*>(refRaw + kOffInvCand[c]);
					if (!IsPlausiblePointer(p) || !IsReadable(reinterpret_cast<const void*>(p), 0x38)) {
						++s.invBad[c];
						continue;
					}
					const auto* inv  = reinterpret_cast<const std::uint8_t*>(p);
					const auto  size = *reinterpret_cast<const std::uint32_t*>(inv + kOffInvData);
					const auto  cap  = *reinterpret_cast<const std::uint32_t*>(inv + kOffInvData + 4);
					const auto  data = *reinterpret_cast<const std::uint64_t*>(inv + kOffInvData + 8);
					if (size > kInvMaxItems || cap < size || cap > (1u << 20)) {
						++s.invBad[c];
						continue;
					}
					if (size == 0) {
						// 空库存：数组头形状对，但拿不出「强证据」⇒ 只算弱票。
						++s.invVotes[c];
						continue;
					}
					if (!IsPlausiblePointer(data) ||
						!IsReadable(reinterpret_cast<const void*>(data), kOffInvItemSize + 8)) {
						++s.invBad[c];
						continue;
					}
					// ★ 强证据（v4.3 新增）：第一条目的 `object` 必须是一个合法 TESForm。
					//   标定选错偏移时读到的多半是垃圾指针，这一步会立刻失败。
					const auto obj = *reinterpret_cast<const std::uint64_t*>(data + kOffInvItemObject);
					if (!IsPlausibleFormPtr(obj)) {
						++s.invBad[c];
						continue;
					}
					++s.invVotes[c];
					++s.invStrong[c];
				}
			}

			for (int c = 0; c < 2; ++c) {
				if (s.invVotes[c] >= kInvCalibNeedOk && s.invBad[c] <= kInvCalibMaxBad &&
					s.invStrong[c] >= kInvCalibNeedStrong) {
					g_invOff       = kOffInvCand[c];
					s.invCalibDone = true;
					REX::INFO("inventory calibration: off=+0x{:X} ok={} strong={} bad={} checks={} rounds={} -> 搜空判空**启用**",
						g_invOff, s.invVotes[c], s.invStrong[c], s.invBad[c], s.invChecks, s.invRounds);
					return;
				}
			}

			// 没通过：**不放弃**，只是降频（理由见函数顶部的长注释）。
			//   注意：降频后这几条提示只在「还没成功」时才出现，成功那次会打上面那条 INFO。
			if (s.invRounds == 1 || s.invRounds == kInvCalibFastRounds) {
				REX::INFO("inventory calibration: 仍在攒样本（rounds={} checks={} votes=+0x{:X}:{} / +0x{:X}:{} strong={}/{} bad={}/{}）"
						  " -> 搜空判空**暂不可用**（容器 / 尸体照常亮）",
					s.invRounds, s.invChecks,
					kOffInvCand[0], s.invVotes[0], kOffInvCand[1], s.invVotes[1],
					s.invStrong[0], s.invStrong[1], s.invBad[0], s.invBad[1]);
			} else if (s.invRounds % 600 == 0) {
				REX::INFO("inventory calibration: 仍无足够样本（rounds={} checks={}；换个有容器 / 尸体的场景就会重试）",
					s.invRounds, s.invChecks);
			}
		}

		// ★ v4.9：这个 base 是不是「展示柜」？—— 静态白名单（`SasDisplayCases.h`，
		//   118 条原版记录）与**运行期学习集合**（打开期间读到过 `kTemporary` 条目的
		//   容器，覆盖第三方 mod 的记录）取或。
		//   完整背景（为什么这类容器不能判空）见常量区「v4.9 展示柜」长注释。
		bool IsDisplayCaseBase(const RE::TESForm* a_base)
		{
			if (!a_base) {
				return false;
			}
			const auto fid = a_base->GetFormID();
			if (std::binary_search(DisplayCases::kBaseIDs,
					DisplayCases::kBaseIDs + DisplayCases::kBaseIDCount, fid)) {
				return true;
			}
			return g_state.displayCaseRuntime.find(fid) != g_state.displayCaseRuntime.end();
		}

		// ★ v4.9：运行期学习 —— 某个容器里出现了 `kTemporary` 条目（`fl&0x20`）
		//   ⇒ 它也是展示柜（内容只在搜刮界面打开期间存在，见常量区）。
		//   把它的 base 记下来，之后这个 base 的所有实例都不再判空。
		//   ★ 只在「新学到」时打一条日志（重复调用是热路径，静默）。
		void RecordDisplayCaseRuntime(const RE::TESObjectREFR* a_ref)
		{
			const auto* base = a_ref ? a_ref->data.objectReference.get() : nullptr;
			if (!base || IsDisplayCaseBase(base)) {
				return;  // 静态白名单里已有 / 已经学过了 —— 不重复记
			}
			if (g_state.displayCaseRuntime.size() >= kDisplayCaseRuntimeMax) {
				return;  // 防呆上限（正常玩一辈子也遇不到 256 个「新」展示柜 base）
			}
			const auto fid = base->GetFormID();
			g_state.displayCaseRuntime.insert(fid);
			REX::INFO("display case (learned): base={:08X} 读到临时条目（fl&0x20 kTemporary）"
					  " -> 之后不再对它判空（本会话已学 {} 个）",
				fid, g_state.displayCaseRuntime.size());
		}

		// 【v4.2】「库存有没有东西」的返回值约定（v4.3 起细分）：
		//     1 = 有东西（照常亮）
		//     0 = 空（不进候选 ⇒ 约 1.5 秒后熄灭）
		//    -1 = 未知（形状不像库存 ⇒ 调用方按「有东西」处理，照常亮）
		//    -2 = `inventoryList` 指针**就是 null** 且 `TreatNullInvAsEmpty=0`
		//         （默认 1 ⇒ null 直接算「空」，见实现里的长注释）
		//    -3 = 标定还没完成或没启用（`g_invOff == 0`）
		//   ★ v4.3 把「null」与「形状坏」分开：前者多半只是「库存还没创建」，
		//     后者才可能是「标定选错了偏移」——排查「搜空还亮」时必须能区分。
		//   `g_invOff` 由 CalibrateInventory() 标定。
		//
		//   ★ 为什么还要逐条把 stack 里的 count 加起来，而不是只看 `data.size()`：
		//     物品被拿光之后，引擎**可能留下 count = 0 的空 stack / 空条目**
		//     （Starfield 里「0 件物品仍占一格」的现象），只看条目数会把已经搜空的箱子
		//     永远当成非空 ⇒ 高亮永不熄灭（这正是用户报的第 3 个问题）。
		//   ★ 全程纯内存读（**零引擎调用、零 VirtualQuery**）：先探指针，再校验
		//     BSTArray 头（size/cap 自洽），任何一步形状不对就返回「未知」。
		//     ⇒ 最坏结果是「和以前一样照常亮」，绝不会因为读错内存而崩。
		int RefLootState(const RE::TESObjectREFR* a_ref)
		{
			if (g_invOff == 0) {
				return -3;
			}
			const auto* raw    = reinterpret_cast<const std::uint8_t*>(a_ref);
			const auto  ptrRaw = *reinterpret_cast<const std::uint64_t*>(raw + g_invOff);
			if (ptrRaw == 0) {
				// ★ v4.3：空指针 = 引擎**从未给这个引用建过库存**（不是「读到垃圾」）。
				//   反汇编证据（2026-09-18）：唯一分配 `BGSInventoryList` 的函数
				//   （vtable slot 0xF0 `CreateInventoryList`）**只有一个调用点**
				//   （引用初始化路径的虚函数），没有找到「打开 UI / 走近时才懒创建」
				//   的第二条路径 ⇒ 没建 = 没东西。
				//   ⇒ 默认判「空」（拿空的尸体 / 容器会正常熄灭）；
				//     万一实测发现「没搜过的身体因此不亮」，把 INI 的
				//     `TreatNullInvAsEmpty=0` 即可退回「未知 ⇒ 照常亮」，不用换 DLL。
				return g_cfg.treatNullInvAsEmpty ? 0 : -2;
			}
			if (!IsPlausiblePointer(ptrRaw)) {
				return -1;
			}
			const auto* inv  = reinterpret_cast<const std::uint8_t*>(ptrRaw);
			const auto  size = *reinterpret_cast<const std::uint32_t*>(inv + kOffInvData);
			const auto  cap  = *reinterpret_cast<const std::uint32_t*>(inv + kOffInvData + 4);
			if (size > kInvMaxItems || cap < size || cap > (1u << 20)) {
				return -1;  // 形状不对劲 = 读到垃圾，不判空
			}
			if (size == 0) {
				return 0;  // 一条都没有 = 空的
			}
			const auto data = *reinterpret_cast<const std::uint64_t*>(inv + kOffInvData + 8);
			if (!IsPlausiblePointer(data)) {
				return -1;
			}

			const std::uint32_t items = std::min<std::uint32_t>(size, kInvWalkItemsMax);
			bool                complete = (size <= kInvWalkItemsMax);
			std::uint32_t       budget = 1024;  // 单次判空最多看多少个 stack（防呆，正常远用不到）
			for (std::uint32_t i = 0; i < items; ++i) {
				const auto* item = reinterpret_cast<const std::uint8_t*>(data) + i * kOffInvItemSize;
				const auto  obj  = *reinterpret_cast<const std::uint64_t*>(item + kOffInvItemObject);
				if (!IsPlausiblePointer(obj)) {
					return -1;
				}
				// ★★ v4.9：见到 `kTemporary`（`fl&0x20`）⇒ 这是**展示柜**的临时投影条目
				//   （只在搜刮界面打开期间存在；实证见常量区「v4.9 展示柜」）⇒
				//   把它的 base 记进运行期学习集合（第三方 mod 新增的展示柜靠这一步兜底）。
				//   注意：这一条**不改变返回值**，只做学习；条目本身照旧参与判空
				//   （打开状态下「有东西 / 拿空了」该怎么判还怎么判）。
				if ((*reinterpret_cast<const std::uint32_t*>(item + kOffInvItemFlags) & kInvItemFlagTemporary) != 0) {
					RecordDisplayCaseRuntime(a_ref);
				}
				// ★★ v4.5：跳过「非玩家物品」（记录标志 0x04）—— NPC 穿在身上的隐形装备
				//   （`*_NOTPLAYABLE`），玩家拿不走、搜刮面板也不显示。把它们算作
				//   「有东西」正是「拿空还亮」的根因（实证见常量区 kFormFlagNonPlayable）。
				//   跳过之后：面板拿空 ⇒ 这里数到的 count 全 0 ⇒ 判「空」⇒ 熄灭。
				//   ★ 只有 `SkipNonPlayableLoot=1`（默认）时才跳；设 0 = 退回旧行为。
				if (g_cfg.skipNonPlayableLoot && IsNonPlayableForm(obj)) {
					++g_state.lootSkipNonPlayable;
					continue;
				}
				// ★★ v4.6：跳过「正穿在身上的装备」（`BGSInventoryItem::flags` 的低 3 位
				//   kSlotMask ≠ 0）—— 引擎只在物品被「掉落 / 卸下」（fl=0）时才让它进
				//   搜刮面板；还穿在尸体身上的（fl≠0）面板不显示、玩家也拿不走，
				//   `count` 却永远 ≥ 1 ⇒ v4.5 只跳了 0x04 那一类，剩下的这一类正是
				//   「活体敌人的尸体（穿玩家版宇航服）拿空后依然不熄灭」的根因
				//   （完整实证 + `SimpleImmersiveLooting` 的旁证见常量区 kInvItemFlagSlotMask）。
				//   跳过之后：面板拿空 ⇒ 判「空」⇒ 熄灭；一旦用「扒取装备」卸下
				//   （fl 归零）⇒ 立刻重新算作「有东西」⇒ 照常亮到拿走为止。
				//   ★ 只有 `SkipEquippedLoot=1`（默认）时才跳；设 0 = 退回旧行为。
				if (g_cfg.skipEquippedLoot &&
					(*reinterpret_cast<const std::uint32_t*>(item + kOffInvItemFlags) & kInvItemFlagSlotMask) != 0) {
					++g_state.lootSkipEquipped;
					continue;
				}
				const auto sn = *reinterpret_cast<const std::uint32_t*>(item + kOffInvItemStacks);
				const auto sc = *reinterpret_cast<const std::uint32_t*>(item + kOffInvItemStacks + 4);
				if (sn == 0) {
					continue;  // 没有 stack 的条目：不计
				}
				if (sn > kInvMaxStacks || sc < sn || sc > (1u << 20)) {
					return -1;
				}
				const auto sd = *reinterpret_cast<const std::uint64_t*>(item + kOffInvItemStacks + 8);
				if (!IsPlausiblePointer(sd)) {
					return -1;
				}
				for (std::uint32_t j = 0; j < sn; ++j) {
					const auto cnt = *reinterpret_cast<const std::uint32_t*>(
						reinterpret_cast<const std::uint8_t*>(sd) + j * kOffStackSize + kOffStackCount);
					if (cnt > 0) {
						return 1;  // 有东西，早退
					}
					if (--budget == 0) {
						return -1;  // 条目/stack 多得离谱 ⇒ 不敢断言「空」
					}
				}
			}
			if (!complete) {
				return -1;  // 条目太多没走完 ⇒ 不敢断言「空」
			}
			return 0;  // 所有条目 / stack 的 count 都是 0 = 空的
		}

		// ====================================================================
		// 状态重置
		// ====================================================================
		// 换 cell / 读档：把这批引用从引擎的高亮表里摘掉（分批，见下）。
		void ResetForNewScene(std::uint64_t a_nowMs, const char* a_reason)
		{
			REX::INFO("reset ({}) outline={}", a_reason, g_state.outlined.size());

			// ★ v4.4：换场景 / 读档 ⇒ 判决缓存与探针去重集合都作废
			//   （旧世界的引用随时可能被销毁，留着它们只会是野键）。
			g_state.achrVerdict.clear();
			g_state.lootProbed.clear();
			g_state.contProbed.clear();  // ★ v4.8

			// 换场景 / 读档：把这批引用从引擎的高亮表里摘掉。
			// ★ v2.2 改成**不立刻动手**：这一步原本会在一帧里发出 200~300 条引擎调用，
			//   实测日志里出现过 18 秒的空档（正好卡在载入新区域时，主线程在加载，
			//   每条调用都被拖慢）。现在只打「待摘」时间戳，真正的摘除由
			//   SyncNativeOutline 按每轮预算慢慢做完（见 Config::maxOutlineOpsPerScan）。
			//   这里存的都是 NiPointer（引用保活），所以晚一点摘也是安全的。
			const auto deadline = std::max(g_state.settleUntilMs, a_nowMs) + 500;
			MarkAllForRemoval(deadline);
		}

		// ====================================================================
		// 开关
		// ====================================================================
		void SetOn(bool a_on, std::uint64_t a_nowMs)
		{
			if (a_on == g_state.on) {
				return;
			}
			g_state.on = a_on;
			REX::INFO("AlwaysScan {} (hotkey)", a_on ? "ON" : "OFF");

			if (!a_on) {
				// 把这批引用从引擎的高亮表里**真正摘掉**
				// （Remove(managers[state]+0x18, &id) → Deactivate(id) → 状态写回 12）
				ClearAllNativeOutline();
			}

			// ★ v4.0：游戏内提示（Tick 里写进 SAS_Notify，桥脚本轮询到之后弹 HUD）
			if (g_cfg.notifyOnToggle) {
				g_state.pendingNotify = a_on ? 1 : 2;
			}
			(void)a_nowMs;
		}

		void PollHotkey(std::uint64_t a_nowMs)
		{
			if (g_cfg.hotkeyVk <= 0) {
				return;
			}
			const bool down = (::GetAsyncKeyState(g_cfg.hotkeyVk) & 0x8000) != 0;
			if (down && !g_state.keyDown && GameWindowFocused()) {
				SetOn(!g_state.on, a_nowMs);
			}
			g_state.keyDown = down;
		}

		// ====================================================================
		// 扫描目标
		// ====================================================================
		struct Candidate
		{
			RE::TESObjectREFR* ref   = nullptr;
			float              d2    = 0.0f;  // 世界单位平方距离
			bool               lit   = false; // 已经在高亮里
			float              key   = 0.0f;  // 排序键
			// ★ v4.0：这个目标该用哪个 outline 状态（= 分类分色），由 ClassifyBase 决定
			std::uint32_t      state = 0;
			std::uint8_t       cat   = 0;  // Category（诊断：分类命中数）
		};

		// ====================================================================
		// ★ 原生 outline 高亮（和原版手持扫描仪走同一套渲染）
		// --------------------------------------------------------------------
		// 逆向结论（游戏 1.16.244.0，完整推导见 docs/03-原生outline高亮实现.md）：
		//   引擎里有一张「引用指针 → outline 状态(0..11，12=无)」的表，外加 12 个
		//   HighlightManager（一个状态一个，各自带那组配色/描边参数）。游戏在扫描时
		//   调用 SetOutlineState(&ref, state) 把引用塞进对应状态的 HighlightManager，
		//   renderer 的 HighlightRenderPass 就给这些对象画**原版描边**。
		//   任务追踪目标 / 工坊 / 制作台的高亮走的都是这一套。
		//
		//   所以本 MOD 完全不需要自己造视觉：只要把「哪些引用、用什么状态」换成
		//   我们的判定，剩下的引擎自己会做。
		//
		// SetOutlineState 有个前提：该引用**必须已经在那张表里**，否则它直接返回。
		// 所以正确顺序是（与引擎自己 `mov [rax], ebp` 完全一致）：
		//     p = LookupOrAddOutlineState(&ref);  *p = state;  SetOutlineState(&ref, state);
		//
		// 注意：这几个函数所有参数都是**引用指针的地址**（TESObjectREFR**），
		//      不是引用指针本身。
		// ====================================================================
		constexpr std::uintptr_t kRvaOutlineLookupOrAdd = 0x17D5BE0;
		constexpr std::uintptr_t kRvaOutlineSet         = 0x17D52B0;
		constexpr std::uintptr_t kRvaOutlineClear       = 0x17D4F10;
		// ★ 12 个 HighlightManager 的指针数组（每个 outline 状态一个）。
		//   0 表示该状态的管理器当前不存在 —— 这时候调 SetOutlineState 会空指针崩，
		//   所以每次动手之前必须检查（这是本方案唯一真正的崩溃风险点）。
		constexpr std::uintptr_t kRvaOutlineManagers    = 0x5F39CF0;
		constexpr std::uint32_t  kOutlineManagerCount   = 12;
		// 引擎实际会建/会被用到的管理器个数（状态 0..10）。0x17D47B0 的循环次数与
		// 0x17D4CD0 的 `state < 11` 都指向 11，第 12 个槽永远是 NULL。
		constexpr std::uint32_t  kOutlineManagerUsed    = 11;
		// 引擎自己的「确保 12 个管理器都存在并把配色参数刷成当前 GMST 值」函数。
		// 不开扫描仪时引擎可能还没建它们，我们就自己叫一次（幂等）。
		// ★ 2026-09-16 复核：这个函数**只创建缺失的管理器**（`if (managers[i]) 跳过创建，
		//   仅刷新参数`，见 0x17D47B0 里 `cmp eax,[rcx+0x30]; je next` 那段），
		//   所以反复调用**不会**清空高亮 —— 想清空必须走下面的 Remove。
		constexpr std::uintptr_t kRvaOutlineEnsureManagers = 0x17D47B0;

		// ================================================================
		// ★ v4.0.1：每状态「高亮参数」的两张静态表（颜色就住在这里）
		// ----------------------------------------------------------------
		// 逆向推导见 docs/03 第十四节。每张表 stride = 0xA0，颜色在 +0x00 / +0x20
		// 两个 dword（引擎按脉冲相位在 High/Low 之间插值），字节序 = R,G,B,A。
		//   · 0x591E088：建/刷新 HighlightManager 时读（`0x17D47B0` 里的
		//     `movsxd rbp, r8d ... vmovd xmm0,[rax + r15 + 0x5919b08]` 同族）
		//   · 0x5919B08：把引用挂进管理器时读（`0x17D4CD0` 里
		//     `lea rax,[rbx+rbx*4]; shl rax,5; vmovd xmm0,[rax+r15+0x5919b08]`）
		// ★ 这两张表**不是 0 初始化**：进程启动的静态初始化函数就把原版默认配色写进去了
		//   （反汇编实证：0xF2AD3E 橙、0xFFE872 金、0x695B11 橄榄 …），之后由
		//   `:Monocle` / `aHighlightScannableOutlineColorHigh|Low_<变体>` 设置刷新。
		//   动态调试时用 `LogOutlineColors()` 直接把 11 个状态的颜色打出来对着看。
		constexpr std::uintptr_t kRvaOutlineMgrParams = 0x591E088;
		constexpr std::uintptr_t kRvaOutlineRefParams = 0x5919B08;

		// ================================================================
		// ★★★ 摘掉高亮真正需要的两个引擎函数（v2.1 修正「F8 关不掉」的根因）★★★
		//
		// 上一版以为「把状态写回 12 + 调 0x17D4F10」就是摘高亮。2026-09-16 复核发现**错**：
		//
		//   ① 0x17D4F10 的行为其实是「**如果该引用还不在状态表里**，就建一条并写 12」
		//      —— 反汇编里 `cmp rcx, rdx(head) / jne <epilogue>` 表示「已经存在 → 直接返回」。
		//      也就是说它对**已经挂上高亮的引用什么都不做**。
		//   ② 真正在画描边的是「状态 → HighlightManager → 内嵌哈希表（键 = FormID）」，
		//      只改状态位根本不会把 id 从哈希表里删掉，渲染器照画 ⇒ 按 F8 没反应。
		//
		// 引擎自己的摘除流程在 **0x653F60**，就三条指令：
		//      if (Remove(map, &id))  Deactivate(id);      // 先摘管理器，再通知渲染器
		// 两个被调函数分别是：
		//      0x6535B0  bool Remove(void* map, std::uint32_t* id)   // 哈希表删 id，返回是否删到
		//      0x653040  void Deactivate(std::uint32_t id)           // 渲染器侧注销（内部自带
		//                                                              id 合法性校验，无效 id 直接返回）
		//      map = managers[state] + 0x18（管理器内嵌哈希表，见 kOffManagerMap 的推导）
		//
		// ⇒ 摘除顺序：Remove(managers[state]+0x18, &id) 成功 → Deactivate(id) → 状态写回 12。
		constexpr std::uintptr_t kRvaOutlineRemove     = 0x6535B0;
		constexpr std::uintptr_t kRvaOutlineDeactivate = 0x653040;

		// ================================================================
		// ★★★ v2.2：改用**引擎自己的**「摘掉一个引用」包装函数 0x653F60 ★★★
		//
		// 实测反馈：v2.1 上线后日志里 `rmOk=0 rmMiss=6332` —— 我们自己拼的
		// `Remove(map, &FormID)` **一次都没命中过**。也就是说那个管理器哈希表里的
		// 键**不是** `TESForm::GetFormID()`（我们一直在拿一个错的 id 去删）。
		//
		// 0x653F60 的 id 是它自己取的，不依赖我们对 id 的任何猜测：
		//
		//   00653F60  void Unhighlight(ctx, TESObjectREFR* ref)
		//     push rdi / sub rsp,0x20
		//     rax = [rdx]                      ; rdx = ref → 取 vtable
		//     rcx = rdx                        ; this = ref
		//     call [rax + 0x50]                ; 引擎自己的一层间接（返回内部对象）
		//     if (!rax) return                 ; ← 它自己会处理「取不到」的情况
		//     id  = [rax + 0x1F0]              ; ★ id 由引擎自己算，不经我们的手
		//     rax = [rdi + 8]                  ; rdi = ctx
		//     rcx = [rax]                      ; ★ *(ctx+8) 必须指向「管理器指针」本身
		//     rcx += 0x18                      ; 内嵌哈希表
		//     if (Remove(rcx, &id)) Deactivate(id)
		//
		// 所以只要我们给的 ctx 满足 `*(ctx+8) == 管理器指针的地址`，它就等价于引擎
		// 正常摘除。我们直接传 `&g_outlineManagers[state]`（那个槽本身就是管理器指针）。
		// ★ 唯一的要求：那个槽非空（为空时它会去读 +0x18 那一带 → 崩）。
		constexpr std::uintptr_t kRvaOutlineUnhighlight    = 0x653F60;
		constexpr std::uint8_t   kSigOutlineUnhighlight[16] = {
			0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x02, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0xCA, 0xFF
		};

		// ================================================================
		// ★★★ v2.3：为什么 0x653F60 也删不掉 —— 它的参数是「3D 节点」不是「引用」★★★
		// ================================================================
		// v2.2 实测日志：
		//     outline remove: rmOk=0 unhMiss=3694 removeMiss=3694 unhighlightReady=1
		// 连引擎自己的 0x653F60 都一次没删到。把 0x17D4CD0（Set 的内部实现）整段
		// 读完后才看清：**0x653F60 的第二个参数是「引用 3D 图里的一个节点」**。
		//
		// 引擎挂高亮的真实流程（0x17D4CD0）：
		//     rcx = TESObjectREFR*                        ; 引用
		//     call [ref->vtable + 0x560]                  ; ★ Get3D(NiPointer<NiAVObject>&)
		//        （commonlibsf 的 TESObjectREFR.h 正好把 0xAC 号虚函数注成
		//          `Unk_AC` = Get3D(NiPointer<NiAVObject>&)；0xAC*8 = 0x560，对得上）
		//     → rsi = 3D 根节点
		//     call 0x24181E0(visitor, 根节点)              ; ★ 递归遍历 3D 图
		//         0x24181E0 的实现：
		//            node->vtable[0x20]()  → 子节点容器（是 NiNode 才有）
		//            有子节点 → 逐个 AddRef 后递归
		//            没有     → visitor->vtable[0x10](visitor, node)
		//
		// visitor 的两个形态（vtable 都在 .rdata，已用 tools/re/func.py vtable 核对）：
		//     摘除：vtable = 0x4B2F9F0（槽 +0x08 / +0x10 都是 0x653F60）
		//           数据 = `void** 管理器槽`（&g_outlineManagers[state]）
		//     挂上：vtable = 0x4B2FA10（槽 +0x08 / +0x10 都是 0x653FC0）
		//           数据 = { manager, manager->0x40 }
		//
		// 而 0x653F60 自己的实现正好印证了这一点：
		//     node->vtable[0x50]()        ; 由节点拿到「持有它的引用对象」
		//     id = [那个对象 + 0x1F0]      ; ★ id 从节点侧算出来（不是我们的 FormID）
		//     rax = [ctx + 8]             ; ctx = visitor
		//     rcx = [rax] + 0x18          ; 管理器内嵌哈希表
		//     if (Remove(rcx, &id)) Deactivate(id)
		//
		// ⇒ 我们 v2.2 把 `TESObjectREFR*` 直接塞进去，`ref->vtable[0x50]` 取到的
		//   东西 `[+0x1F0]` 根本不是 FormID ⇒ Remove 永远命中不了 ⇒ 高亮永远摘不掉。
		//   症状完全对得上用户实测：
		//     · F8 关掉后「已高亮的不会熄灭」（只停止新增，旧的摘不掉）
		//     · 「走老远物体还亮着」（同上）
		//     · 「在已高亮区域也卡」（管理器哈希表只增不减，渲染器每帧过一遍）
		//     · 「像在重复高亮已高亮的物品」（我们的表 erase 了、引擎表还在 →
		//        下次进半径又被当新目标挂一次）
		constexpr std::uintptr_t kRvaOutlineVisit          = 0x24181E0;
		constexpr std::uintptr_t kRvaOutlineRemoveVisorVt  = 0x4B2F9F0;
		// TESObjectREFR 虚函数表里 Get3D(NiPointer<NiAVObject>&) 的下标（0xAC*8 = 0x560）
		constexpr std::uint32_t  kVtblIdxRefGet3D          = 0xAC;
		constexpr std::uint8_t   kSigOutlineVisit[16] = {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57
		};

		// HighlightManager(0x48 B) 内嵌哈希表对象的偏移。两处交叉确认：
		//   ctor 0x6532F0：`lea rax,[rdi+0x10]; lea rbx,[rax+8]` → rbx=manager+0x18，
		//                  随后 [rbx+8]=data / [rbx+0x10]=capacity / [rbx+0x18]=count；
		//   0x653F60：`mov rcx,[rax]; add rcx,0x18; call Remove` —— 引擎自己也是 +0x18。
		constexpr std::size_t kOffManagerMap  = 0x18;
		// 哈希表头需要连续可读的字节数（data@+8 / capacity@+0x10 / count@+0x18 / watermark@+0x20）
		constexpr std::size_t kSizeManagerMap = 0x28;

		// 状态 12 = 「无高亮」（0..11 才是真实样式）。
		// ★ 注意：管理器数组只有 11 个（0..10）—— `0x17D4CD0` 开头就是 `cmp ebp,0xB; jae ret`，
		//   0x17D47B0 的建表循环也是 `mov r12d, 0xB`。所以 11 = 全部。
		constexpr std::uint32_t kOutlineStateNone = 12;

		// 函数首 16 字节签名 —— 游戏版本一变，RVA 就不再可靠，这里直接拦下来。
		constexpr std::uint8_t kSigOutlineLookupOrAdd[16] = {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57
		};
		constexpr std::uint8_t kSigOutlineSet[16] = {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57
		};
		constexpr std::uint8_t kSigOutlineClear[16] = {
			0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48
		};
		constexpr std::uint8_t kSigOutlineEnsureManagers[16] = {
			0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x48, 0x08, 0x57
		};
		// bool Remove(void* map, std::uint32_t* id)
		constexpr std::uint8_t kSigOutlineRemove[16] = {
			0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x4C, 0x8B, 0x51, 0x10, 0x4C, 0x8B, 0xD9, 0x4D, 0x8B, 0xCA
		};
		// void Deactivate(std::uint32_t id)
		constexpr std::uint8_t kSigOutlineDeactivate[16] = {
			0x48, 0x83, 0xEC, 0x28, 0x81, 0xF9, 0xFF, 0xFF, 0xFF, 0x00, 0x74, 0x40, 0x4C, 0x8B, 0x0D, 0x25
		};

		using OutlineLookupOrAdd_t = std::uint32_t* (*)(void*, RE::TESObjectREFR**);
		using OutlineSet_t         = void (*)(RE::TESObjectREFR**, std::uint32_t);
		using OutlineClear_t       = void (*)(void*, RE::TESObjectREFR**);
		using OutlineEnsure_t      = void (*)(void*);
		using OutlineRemove_t      = bool (*)(void*, std::uint32_t*);
		using OutlineDeactivate_t  = void (*)(std::uint32_t);
		// void Unhighlight(ctx, TENode* node)；ctx（= visitor）布局见上面的长注释。
		// ★ 第二个参数是**3D 节点**，不是 TESObjectREFR —— 这是 v2.3 修掉的关键。
		using OutlineUnhighlight_t = void (*)(void*, RE::NiAVObject*);
		// bool Visit(void* visitor, NiAVObject* node)（0x24181E0，自带递归）
		using OutlineVisit_t = bool (*)(void*, RE::NiAVObject*);
		// void Get3D(NiPointer<NiAVObject>& a_out)（vtable 下标 0xAC）
		using RefGet3D_t = void (*)(RE::TESObjectREFR*, RE::NiPointer<RE::NiAVObject>*);

		OutlineLookupOrAdd_t  g_outlineLookupOrAdd  = nullptr;
		OutlineSet_t          g_outlineSet          = nullptr;
		OutlineClear_t        g_outlineClear        = nullptr;
		OutlineEnsure_t       g_outlineEnsure       = nullptr;
		OutlineRemove_t       g_outlineRemove       = nullptr;
		OutlineDeactivate_t   g_outlineDeactivate   = nullptr;
		OutlineUnhighlight_t  g_outlineUnhighlight  = nullptr;
		bool                  g_outlineUnhighlightReady = false;
		std::uintptr_t        g_outlineManagerArray = 0;
		// v2.3：走引擎那套「3D 图 visitor」摘除所需的三个东西
		OutlineVisit_t        g_outlineVisit        = nullptr;
		std::uintptr_t        g_outlineRemoveVisorVt = 0;
		bool                  g_outlineGraphRemoveReady = false;

		// 前置声明（定义在 ResolveNativeOutline 之后）
		std::uintptr_t OutlineManagerFor(std::uint32_t a_state);
		std::uint32_t  PrimaryState();
		void           LogManagerDiagnostics(const char* a_tag);
		void           LogManagerMapDiag(const char* a_tag, std::uint32_t a_state);
		void           LogOutlineColors(const char* a_tag);
		bool           EnsureManagerFor(std::uint32_t a_state);
		bool           OutlineUnhighlightRef(RE::TESObjectREFR* a_ref, std::uint32_t a_state);

		std::uintptr_t ModuleBase()
		{
			static const std::uintptr_t base =
				reinterpret_cast<std::uintptr_t>(::GetModuleHandleA(nullptr));
			return base;
		}

		template <std::size_t N>
		bool SigMatches(std::uintptr_t a_addr, const std::uint8_t (&a_sig)[N])
		{
			if (!a_addr || !IsReadable(reinterpret_cast<const void*>(a_addr), N)) {
				return false;
			}
			const auto* p = reinterpret_cast<const std::uint8_t*>(a_addr);
			for (std::size_t i = 0; i < N; ++i) {
				if (p[i] != a_sig[i]) {
					return false;
				}
			}
			return true;
		}

		void ResolveNativeOutline()
		{
			const auto base = ModuleBase();
			if (!base) {
				REX::WARN("native outline: module base unavailable");
				return;
			}
			const auto addrLookup = base + kRvaOutlineLookupOrAdd;
			const auto addrSet    = base + kRvaOutlineSet;
			const auto addrClear  = base + kRvaOutlineClear;

			const auto addrEnsure = base + kRvaOutlineEnsureManagers;

			if (!SigMatches(addrLookup, kSigOutlineLookupOrAdd) ||
				!SigMatches(addrSet, kSigOutlineSet) ||
				!SigMatches(addrClear, kSigOutlineClear) ||
				!SigMatches(addrEnsure, kSigOutlineEnsureManagers)) {
				REX::WARN("native outline: signature mismatch (game version changed?) -> native outline DISABLED."
						  " lookup=0x{:X} set=0x{:X} clear=0x{:X} ensure=0x{:X}. 请按 docs/03 重新核对 RVA。",
					kRvaOutlineLookupOrAdd, kRvaOutlineSet, kRvaOutlineClear, kRvaOutlineEnsureManagers);
				g_state.nativeReady = false;
				return;
			}

			g_outlineLookupOrAdd  = reinterpret_cast<OutlineLookupOrAdd_t>(addrLookup);
			g_outlineSet          = reinterpret_cast<OutlineSet_t>(addrSet);
			g_outlineClear        = reinterpret_cast<OutlineClear_t>(addrClear);
			g_outlineEnsure       = reinterpret_cast<OutlineEnsure_t>(addrEnsure);
			g_outlineManagerArray = base + kRvaOutlineManagers;
			g_state.nativeReady   = true;

			// 「摘除」用的两个函数：拿不到只是关不干净（描边残留），不该整体禁用，
			// 所以单独校验、单独降级。
			const auto addrRemove     = base + kRvaOutlineRemove;
			const auto addrDeactivate = base + kRvaOutlineDeactivate;
			if (SigMatches(addrRemove, kSigOutlineRemove) && SigMatches(addrDeactivate, kSigOutlineDeactivate)) {
				g_outlineRemove     = reinterpret_cast<OutlineRemove_t>(addrRemove);
				g_outlineDeactivate = reinterpret_cast<OutlineDeactivate_t>(addrDeactivate);
				g_state.nativeRemoveReady = true;
			} else {
				REX::WARN("native outline: remove/deactivate 签名不匹配（游戏版本变了？）→ 关闭后可能留残留描边。"
						  " remove=+0x{:X} deactivate=+0x{:X}，请按 docs/03 重新核对 RVA。",
					kRvaOutlineRemove, kRvaOutlineDeactivate);
			}

			REX::INFO("native outline ready: lookupOrAdd=+0x{:X} set=+0x{:X} clear=+0x{:X} ensure=+0x{:X} managers=+0x{:X} (sig verified)",
				kRvaOutlineLookupOrAdd, kRvaOutlineSet, kRvaOutlineClear,
				kRvaOutlineEnsureManagers, kRvaOutlineManagers);
			REX::INFO("native outline remove: remove=+0x{:X} deactivate=+0x{:X} managers[{}].map=+0x{:X} -> {}",
				kRvaOutlineRemove, kRvaOutlineDeactivate, kOutlineManagerUsed, kOffManagerMap,
				g_state.nativeRemoveReady ? "ok" : "UNAVAILABLE(将只能写状态 12)");

			// ★ v2.2：引擎自己的摘除包装函数（首选路径；拿不到就退回自己拼的那条）
			const auto addrUnhighlight = base + kRvaOutlineUnhighlight;
			if (SigMatches(addrUnhighlight, kSigOutlineUnhighlight)) {
				g_outlineUnhighlight      = reinterpret_cast<OutlineUnhighlight_t>(addrUnhighlight);
				g_outlineUnhighlightReady = true;
				REX::INFO("native outline unhighlight: +0x{:X} (sig verified) -> 摘除走引擎自己的路径（id 由引擎算）",
					kRvaOutlineUnhighlight);
			} else {
				REX::WARN("native outline unhighlight: +0x{:X} 签名不匹配（游戏版本变了？）-> 退回自己拼的 Remove(FormID)",
					kRvaOutlineUnhighlight);
			}

			// ★ v2.3：摘除的真正正确姿势需要三样东西（见文件上方那段长注释）：
			//   ① 0x24181E0 —— 递归遍历 3D 图的 visitor 访问器；
			//   ② 0x4B2F9F0 —— 摘除用 visitor 的 vtable（两个槽都必须是 0x653F60，
			//      这样 0x24181E0 里 `visitor->vtable[0x10](visitor, node)` 才会走摘除）；
			//   ③ 引用虚函数表下标 0xAC = Get3D(NiPointer<NiAVObject>&)。
			// 三者任一拿不准就只降级（不清空 = 观感问题，绝不能乱调）。
			const auto addrVisit  = base + kRvaOutlineVisit;
			const auto addrVisorVt = base + kRvaOutlineRemoveVisorVt;
			bool       visorVtOk   = false;
			if (IsReadable(reinterpret_cast<const void*>(addrVisorVt), 0x18)) {
				const auto* vt = reinterpret_cast<const std::uintptr_t*>(addrVisorVt);
				visorVtOk = (vt[1] == base + kRvaOutlineUnhighlight) &&
				            (vt[2] == base + kRvaOutlineUnhighlight);
			}
			if (visorVtOk && g_outlineUnhighlightReady && SigMatches(addrVisit, kSigOutlineVisit)) {
				g_outlineVisit             = reinterpret_cast<OutlineVisit_t>(addrVisit);
				g_outlineRemoveVisorVt     = addrVisorVt;
				g_outlineGraphRemoveReady  = true;
				REX::INFO("native outline graph-remove: visit=+0x{:X} visitorVtbl=+0x{:X} refGet3D=vtable[0x{:X}] -> 摘除走「3D 图 visitor」（这次才是对的）",
					kRvaOutlineVisit, kRvaOutlineRemoveVisorVt, kVtblIdxRefGet3D);
			} else {
				REX::WARN("native outline graph-remove: 不可用（visit={} visitorVtbl={} unhighlight={}）-> 摘除仍不准（F8 可能关不干净）",
					SigMatches(addrVisit, kSigOutlineVisit) ? "ok" : "sig-mismatch",
					visorVtOk ? "ok" : "bad-vtbl",
					g_outlineUnhighlightReady ? "ok" : "unavailable");
			}
			LogManagerDiagnostics("install");
			LogManagerMapDiag("install", PrimaryState());
			// ★ v4.0.1：把 11 个状态的实际配色打出来（分类分色出问题时的第一手证据）
			LogOutlineColors("install");
		}

		// 读「第 a_state 个 HighlightManager」指针；0 = 现在不存在
		std::uintptr_t OutlineManagerFor(std::uint32_t a_state)
		{
			if (!g_outlineManagerArray || a_state >= kOutlineManagerCount) {
				return 0;
			}
			const auto* arr = reinterpret_cast<const std::uintptr_t*>(g_outlineManagerArray);
			if (!IsReadable(arr, kOutlineManagerCount * sizeof(std::uintptr_t))) {
				return 0;
			}
			return arr[a_state];
		}

		std::uint32_t CountLiveManagers()
		{
			std::uint32_t n = 0;
			for (std::uint32_t i = 0; i < kOutlineManagerUsed; ++i) {
				if (OutlineManagerFor(i)) {
					++n;
				}
			}
			return n;
		}

		// ★ v4.0：配置里用到的「代表状态」= 可拾取那一类（旧版这里是全局 OutlineState）。
		//   分类分色之后每个类别各有状态，诊断行里逐个列出来更有用。
		std::uint32_t PrimaryState()
		{
			return static_cast<std::uint32_t>(std::clamp(g_cfg.stateByCategory[0], 0, 11));
		}

		void LogManagerDiagnostics(const char* a_tag)
		{
			std::string per;
			for (std::size_t i = 0; i < kCategoryCount; ++i) {
				if (i) {
					per += ' ';
				}
				per += kCategoryName[i];
				per += '=';
				per += std::to_string(g_cfg.stateByCategory[i]);
				per += OutlineManagerFor(static_cast<std::uint32_t>(g_cfg.stateByCategory[i])) ? "(ok)" : "(null)";
			}
			REX::INFO("native outline managers[{}] = {}/{} alive | stateByCategory: {}",
				a_tag, CountLiveManagers(), kOutlineManagerUsed, per);
		}

		// ====================================================================
		// ★ v4.0.1：每状态配色表（诊断 + 可选覆盖）
		// --------------------------------------------------------------------
		// 逆向结论（1.16.244.0，见 docs/03 第十四节）：
		//   引擎为「每个 outline 状态」各存一组高亮参数（0xA0 字节），颜色是**两个
		//   dword**（低/高，引擎按脉冲相位在两者之间插值），字节序 = R,G,B,A：
		//
		//     表 A（管理器参数）RVA 0x591E088 + state*0xA0  —— `0x17D47B0` 建/刷新
		//          HighlightManager 时读它（+0x00 = High、+0x20 = Low、+0x40 = 插值除数）
		//     表 B（引用参数）  RVA 0x5919B08 + state*0xA0  —— `0x17D4CD0` 把引用挂进
		//          管理器时读它（+0x00）
		//
		//   这两张表**都不是 0 初始化**：进程启动时的静态初始化函数就把原版默认配色
		//   写进去了（反汇编实证：0xF2AD3E 橙 / 0xFFE872 金 / 0x695B11 橄榄 …），
		//   之后由 `:Monocle` 那 11 组 `aHighlightScannableOutlineColorHigh/Low_<变体>`
		//   设置刷新。
		//
		//   所以「每个状态本来就是不同颜色」，本 MOD 只需要把类别映射到不同的状态
		//   （或进一步用 INI 覆盖成自己想要的颜色）。
		// ====================================================================
		constexpr std::size_t kOutlineParamStride = 0xA0;
		// 顺序 = 引擎静态初始化函数里的写入顺序（docs/03 第十四节有推导）。
		constexpr const char* kOutlineStateName[kOutlineManagerUsed] = {
			"Generic", "Scanned", "FullyScanned", "Tracked", "Bounty",
			"Social", "TargetGeneric", "TargetScannable", "TargetScanned",
			"TargetFullyScanned", "?"
		};

		std::uint8_t* OutlineParamTable(std::uintptr_t a_rva)
		{
			auto* p = reinterpret_cast<std::uint8_t*>(ModuleBase() + a_rva);
			if (!IsReadable(p, kOutlineManagerUsed * kOutlineParamStride)) {
				return nullptr;
			}
			return p;
		}

		void LogOutlineColors(const char* a_tag)
		{
			auto* mgrTab = OutlineParamTable(kRvaOutlineMgrParams);
			auto* refTab = OutlineParamTable(kRvaOutlineRefParams);
			if (!mgrTab || !refTab) {
				REX::WARN("outline colors[{}]: 配色表不可读（RVA 0x{:X}/0x{:X} 在这个版本上变了？）",
					a_tag, kRvaOutlineMgrParams, kRvaOutlineRefParams);
				return;
			}
			for (std::uint32_t i = 0; i < kOutlineManagerUsed; ++i) {
				const auto rd = [](const std::uint8_t* p) {
					return *reinterpret_cast<const std::uint32_t*>(p);
				};
				const auto hi = rd(mgrTab + i * kOutlineParamStride);
				const auto lo = rd(mgrTab + i * kOutlineParamStride + 0x20);
				const auto rf = rd(refTab + i * kOutlineParamStride);
				// dword 的布局是 `0x00RRGGBB`（用截图反证过：state 0 的值 = 青，
				// 与画面完全一致）。所以 R = >>16 / G = >>8 / B = &0xFF。
				const auto rgbOf = [](std::uint32_t v) {
					return static_cast<unsigned>(((v >> 16) & 0xFFu) << 16 | ((v >> 8) & 0xFFu) << 8 | (v & 0xFFu));
				};
				char buf[160];
				std::snprintf(buf, sizeof(buf),
					"  state=%2u %-18s ref=#%06X (%3u,%3u,%3u) mgrHigh=#%06X mgrLow=#%06X",
					i, kOutlineStateName[i], rgbOf(rf),
					(rf >> 16) & 0xFFu, (rf >> 8) & 0xFFu, rf & 0xFFu,
					rgbOf(hi), rgbOf(lo));
				REX::INFO("outline colors[{}]: {}", a_tag, buf);
			}
		}

		// 把 INI 里的自定义颜色写进引擎的两张配色表（只对「被类别用到的状态」）。
		//   dword 布局 = R | G<<8 | B<<16 | A<<24；alpha 保留引擎原值（我们不懂它的语义，
		//   不改动最安全 —— 只换 RGB）。
		//   写完调一次 `0x17D47B0` 让引擎把新参数刷进渲染器，并把已挂的目标整批重申
		//   （引用参数表是「挂的时候读一次」，所以必须重新 Set 才会生效）。
		void ApplyColorOverrides()
		{
			auto* mgrTab = OutlineParamTable(kRvaOutlineMgrParams);
			auto* refTab = OutlineParamTable(kRvaOutlineRefParams);
			if (!mgrTab || !refTab) {
				return;
			}
			std::uint32_t applied = 0;
			for (std::size_t c = 0; c < kCategoryCount; ++c) {
				const auto rgb = g_cfg.colorOverride[c];
				if (rgb == kColorUnset) {
					continue;
				}
				const auto st = static_cast<std::uint32_t>(std::clamp(g_cfg.stateByCategory[c], 0, 11));
				if (st >= kOutlineManagerUsed) {
					continue;
				}
				auto* mgr = mgrTab + st * kOutlineParamStride;
				auto* ref = refTab + st * kOutlineParamStride;
				const auto patch = [rgb](std::uint8_t* p) {
					auto v = *reinterpret_cast<std::uint32_t*>(p);
					v = (v & 0xFF000000u) | (rgb & 0xFFFFFFu);
					*reinterpret_cast<std::uint32_t*>(p) = v;
				};
				patch(mgr + 0x00);  // High
				patch(mgr + 0x20);  // Low（一起改，脉冲时不会变色）
				patch(ref + 0x00);
				++applied;
				char buf[96];
				std::snprintf(buf, sizeof(buf), "outline colors: 覆盖 %s 的颜色 -> state=%u #%06X",
					kCategoryName[c], st, rgb & 0xFFFFFFu);
				REX::INFO("{}", buf);
			}
			if (applied == 0) {
				return;
			}
			if (g_outlineEnsure) {
				g_outlineEnsure(nullptr);  // 让引擎拿新配色刷新 11 个管理器
			}
			// 引用参数是「挂的时候读一次」⇒ 已挂的必须重新 Set 才会用上新颜色
			MarkAllForReassert("颜色覆盖后重刷");
			REX::INFO("outline colors: 已覆盖 {} 个类别的颜色（并触发一次重挂）", applied);
		}

		// ★ v4.0.1：进入世界后**再做一次**配色刷新（只做一次）。
		//   为什么需要：我们的管理器是在第一次扫描时建的，而 `:Monocle` 那 11 组配色
		//   有可能在那之前还没被引擎刷进参数表（那就会「11 个状态一个颜色」）。
		//   而 `0x17D47B0` 对**已存在**的管理器是「只刷新参数」（docs/03 第九节 9.4）
		//   ⇒ 再调一次即可把正确的每状态配色补上。
		//   两行 `outline colors[...]` 日志（install / world-ready）可以用来对比。
		void RefreshOutlineParamsOnce()
		{
			if (g_state.paramsRefreshed) {
				return;
			}
			g_state.paramsRefreshed = true;
			LogOutlineColors("world-ready");
			if (g_outlineEnsure) {
				g_outlineEnsure(nullptr);  // 幂等：已有管理器只刷新参数
			}
			ApplyColorOverrides();
		}

		// --------------------------------------------------------------------
		// ★ v2.2 诊断：把某个管理器内嵌哈希表的「家底」打出来
		// --------------------------------------------------------------------
		// 目的：v2.1 的 `rmOk=0 rmMiss=6332` 说明我们拿去删的 id 根本不是表里的键。
		// 这一行会告诉我们表里到底有哪些键（以及表有多大），从此不用再猜 ——
		// 如果里面的键是 0x04xxxxxx 这种 FormID，就说明键确实是 FormID，问题在别处；
		// 如果是别的形态，就说明要按那种形态去删。
		// 哈希表布局（ctor 0x6532F0 + Remove 0x6535B0 交叉确认）：
		//   元素 12 字节 = { key(u32), next(u32), prev(u32) }，+4 == 0xFFFFFFFF 表示空槽。
		// --------------------------------------------------------------------
		void LogManagerMapDiag(const char* a_tag, std::uint32_t a_state)
		{
			const auto mgr = OutlineManagerFor(a_state);
			if (!mgr) {
				REX::INFO("manager map[{}] state={} : 管理器不存在", a_tag, a_state);
				return;
			}
			const auto* head = reinterpret_cast<const std::uint32_t*>(mgr + kOffManagerMap);
			if (!IsReadable(head, kSizeManagerMap)) {
				REX::INFO("manager map[{}] state={} : 表头不可读", a_tag, a_state);
				return;
			}
			const std::uint32_t cap   = head[0x10 / 4];
			const std::uint32_t c18   = head[0x18 / 4];
			const std::uint32_t c20   = head[0x20 / 4];
			const auto          data  = reinterpret_cast<std::uintptr_t>(head) + 8;
			const auto          dataP = *reinterpret_cast<const std::uint64_t*>(data);

			REX::INFO("manager map[{}] state={} : mgr={:#x} cap={} count18={} count20={} data={:#x}",
				a_tag, a_state, mgr, cap, c18, c20, dataP);

			if (!cap || cap > (1u << 16) || !IsReadable(reinterpret_cast<const void*>(dataP), 12)) {
				return;
			}
			const std::uint32_t slots = std::min<std::uint32_t>(cap, 4096);
			if (!IsReadable(reinterpret_cast<const void*>(dataP), static_cast<std::size_t>(slots) * 12)) {
				return;
			}
			const auto* elems = reinterpret_cast<const std::uint32_t*>(dataP);
			char        tmp[48]{};
			std::string keys;
			std::uint32_t found = 0;
			for (std::uint32_t i = 0; i < slots && found < 8; ++i) {
				const std::uint32_t key  = elems[i * 3 + 0];
				const std::uint32_t next = elems[i * 3 + 1];
				if (next == 0xFFFFFFFFu && key == 0xFFFFFFFFu) {
					continue;  // 空槽
				}
				std::snprintf(tmp, sizeof(tmp), "%s0x%08X", keys.empty() ? "" : ", ", key);
				keys += tmp;
				++found;
			}
			REX::INFO("manager map[{}] state={} : 前 {} 个非空键 = [{}]", a_tag, a_state, found, keys);
		}

		// 管理器内嵌哈希表当前的「元素数」字段（诊断用，不作为逻辑判据）。
		std::uint32_t ManagerMapCount(std::uintptr_t a_mgr)
		{
			if (!a_mgr) {
				return 0;
			}
			const auto* p = reinterpret_cast<const std::uint32_t*>(a_mgr + kOffManagerMap + 0x18);
			return IsReadable(p, sizeof(std::uint32_t)) ? *p : 0;
		}

		// ★ v2.3：把「取引用的 3D 根节点」这一步单独抽出来（引擎在 0x17D4CD0 里就是
		//   这么干的：`call [ref->vtable + 0x560]`，commonlibsf 里那个槽就是
		//   `TESObjectREFR::Unk_AC` = Get3D(NiPointer<NiAVObject>&)）。
		//   返回值是 NiPointer（我们自己持一份引用），用完自动减回去。
		RE::NiPointer<RE::NiAVObject> RefGet3D(RE::TESObjectREFR* a_ref)
		{
			RE::NiPointer<RE::NiAVObject> out;
			if (!a_ref || !IsReadable(a_ref, 8)) {
				return out;
			}
			// 引用虚函数表（TESObjectREFR 的第一个成员就是 vptr）
			void** vptr = *reinterpret_cast<void***>(a_ref);
			if (!IsPlausiblePointer(reinterpret_cast<std::uint64_t>(vptr))) {
				return out;
			}
			const auto fn = reinterpret_cast<std::uintptr_t>(vptr[kVtblIdxRefGet3D]);
			// 形状校验：目标必须是**主模块 .text 里的代码地址**。如果哪天游戏改了
			// 虚表布局，这里取到的多半是个数据指针（比如 0x7FF... 之外的堆地址），
			// 拦下来就不会去 call 一个数据地址（那是必崩）。
			const auto base = ModuleBase();
			if (fn < base || fn >= base + 0x10000000) {
				return out;
			}
			reinterpret_cast<RefGet3D_t>(fn)(a_ref, &out);
			return out;
		}

		// 用引擎自己的那套「3D 图 visitor」把一个引用从 state 对应的管理器里摘掉。
		//
		//   visitor = { vtable = 0x4B2F9F0, 数据 = &g_outlineManagers[state] }
		//   0x24181E0(visitor, 引用3D根节点)  → 递归整棵 3D 图，每个节点调一次 0x653F60
		//
		// ★ 再说一遍：0x653F60 的第二个参数必须是**节点**（它的 `node->vtable[0x50]()`
		//   才拿得到持有它的引用对象，`[+0x1F0]` 才是管理器哈希表里真正的键）。
		//   传 TESObjectREFR 进去就是一次都不会命中（v2.2 的 rmOk=0）。
		bool OutlineUnhighlightRef(RE::TESObjectREFR* a_ref, std::uint32_t a_state)
		{
			if (!g_outlineGraphRemoveReady || !g_outlineVisit || !a_ref || a_state >= kOutlineManagerUsed) {
				return false;
			}
			// 管理器槽为空时绝不能调：0x653F60 会把 manager+0x18 当表头去读 → 崩。
			auto* const slot = reinterpret_cast<void**>(g_outlineManagerArray + a_state * sizeof(std::uintptr_t));
			if (!IsReadable(slot, sizeof(void*)) || !*slot) {
				return false;
			}
			const auto root = RefGet3D(a_ref);
			if (!root) {
				return false;  // 3D 还没加载（引用存在但没 3D 时本来就没挂上高亮）
			}
			struct Visitor
			{
				void*  vtbl;
				void** managerSlot;
			} visitor{ reinterpret_cast<void*>(g_outlineRemoveVisorVt), slot };
			g_outlineVisit(&visitor, root.get());
			return true;
		}

		// 确保目标状态的管理器存在。不存在就调用引擎自己的「重建管理器」函数
		// （它会把 12 个管理器按当前 GMST 配色建好，幂等）。
		// ★ 返回 false 时**绝对不能**调 SetOutlineState —— 那会空指针解引用崩游戏。
		bool EnsureManagerFor(std::uint32_t a_state)
		{
			if (OutlineManagerFor(a_state)) {
				return true;
			}
			if (!g_cfg.autoEnsureManagers || !g_outlineEnsure) {
				return false;
			}
			g_outlineEnsure(nullptr);
			return OutlineManagerFor(a_state) != 0;
		}

		// 把引用挂到（或重申到）原生 outline 的 a_state 状态
		bool OutlineRef(RE::TESObjectREFR* a_ref, std::uint32_t a_state)
		{
			if (!g_state.nativeReady || !a_ref) {
				return false;
			}
			// ★ 必须先确认管理器存在：SetOutlineState 内部会直接解引用
			//   managers[state]，为空就是 0xC0000005。
			if (!EnsureManagerFor(a_state)) {
				const auto now = NowMs();
				if (now - g_state.lastOutlineErrMs > kBindWarnIntervalMs) {
					g_state.lastOutlineErrMs = now;
					REX::WARN("native outline: manager[{}] 不存在（引擎还没建，且 autoEnsure 关闭/失败）-> 本轮不挂高亮",
						a_state);
				}
				return false;
			}

			RE::TESObjectREFR* slot = a_ref;
			auto*              p    = g_outlineLookupOrAdd(nullptr, &slot);
			if (!p || !IsReadable(p, sizeof(std::uint32_t))) {
				return false;
			}
			*p = a_state;
			g_outlineSet(&slot, a_state);
			return true;
		}

		// --------------------------------------------------------------------
		// 摘掉原生 outline（v2.2 重写）
		// --------------------------------------------------------------------
		// 三条路，按可靠性从高到低：
		//   ① **引擎自己的 0x653F60**（`OutlineUnhighlightRef`）：id 由引擎自己算，
		//      不依赖我们对「哈希表的键是什么」的猜测 —— 这是 v2.2 的主要改动，
		//      因为 v2.1 用 FormID 去删实测**一次都没命中**（rmOk=0）。
		//   ② 回退：自己按 FormID 调 `Remove(map,&id)` → `Deactivate(id)`。
		//   ③ 无论如何把状态写回 12（让引擎下一轮刷新不再往管理器里塞这个引用）。
		//
		// ★ 状态候选顺序：状态表里记的（最可信，是我们写进去的）→ 配置里的 → 其余全部。
		//   只有前两个都没「动过东西」时才会一路扫完 11 个（每秒几十次的摘除量下，
		//   这个代价是可以接受的；而且走到那一步本身就说明我们的假设错了）。
		// --------------------------------------------------------------------
		bool RemoveOutlineIdFromManager(std::uint32_t a_state, std::uint32_t* a_id)
		{
			if (!g_outlineRemove || a_state >= kOutlineManagerUsed) {
				return false;
			}
			const auto mgr = OutlineManagerFor(a_state);
			if (!mgr) {
				return false;
			}
			auto* const map = reinterpret_cast<void*>(mgr + kOffManagerMap);
			if (!IsReadable(map, kSizeManagerMap)) {
				return false;
			}
			if (!g_outlineRemove(map, a_id)) {
				return false;
			}
			if (g_outlineDeactivate) {
				g_outlineDeactivate(*a_id);
			}
			return true;
		}

		void UnoutlineRef(RE::TESObjectREFR* a_ref)
		{
			if (!g_state.nativeReady || !a_ref) {
				return;
			}
			RE::TESObjectREFR* slot = a_ref;
			auto*              p    = g_outlineLookupOrAdd(nullptr, &slot);
			const bool         pOk  = p && IsReadable(p, sizeof(std::uint32_t));
			const std::uint32_t stateInTable = (pOk && *p < kOutlineManagerUsed)
			                                     ? *p
			                                     : kOutlineStateNone;

			// ---- ① 引擎自己的「3D 图 visitor」摘除（唯一真正有效的一条）----
			// ★ v2.3：不再扫 11 个状态。v2.2 那套「先试状态表 → 再试配置 → 再全扫一遍」
			//   是在补救「怎么都删不掉」——而每一次尝试都是一次真的引擎调用
			//   （虚调用 + 哈希查找 + **整棵 3D 图递归遍历**），单轮最多 11 倍代价。
			//   实测 `timing max=438ms / ops=64` 里有一大半就是它。
			// ★ v4.0：分类分色之后没有「唯一的那个配置状态」了，改成
			//   「状态表里记的」+「配置里用到的每一种状态（去重）」各试一次。
			if (g_outlineGraphRemoveReady) {
				bool done = false;
				if (stateInTable < kOutlineManagerUsed) {
					done = OutlineUnhighlightRef(a_ref, stateInTable);
				}
				if (!done) {
					bool tried[kOutlineManagerUsed]{};
					if (stateInTable < kOutlineManagerUsed) {
						tried[stateInTable] = true;
					}
					for (std::size_t i = 0; i < kCategoryCount && !done; ++i) {
						const auto s = static_cast<std::uint32_t>(g_cfg.stateByCategory[i]);
						if (s < kOutlineManagerUsed && !tried[s]) {
							tried[s] = true;
							done     = OutlineUnhighlightRef(a_ref, s);
						}
					}
				}
				if (done) {
					++g_state.outlineRemoved;
				} else {
					++g_state.outlineUnhighlightMiss;
				}
				if (pOk) {
					*p = kOutlineStateNone;
				}
				return;
			}

			// ---- ② 回退：自己按 FormID 删（v2.1 的老路，实测基本删不到）----
			if (g_state.nativeRemoveReady) {
				const std::uint32_t id = a_ref->GetFormID();
				if (id != 0 && id != 0xFFFFFF) {
					std::uint32_t tmp     = id;
					bool          removed = false;
					if (pOk && *p < kOutlineManagerUsed) {
						removed = RemoveOutlineIdFromManager(*p, &tmp);
					}
					for (std::uint32_t s = 0; !removed && s < kOutlineManagerUsed; ++s) {
						removed = RemoveOutlineIdFromManager(s, &tmp);
					}
					if (removed) {
						++g_state.outlineRemoved;
						if (pOk) {
							*p = kOutlineStateNone;
						}
						return;
					}
				}
			} else if (g_outlineClear) {
				// 退化路径：老版本的行为（引擎自己的 0x17D4F10；对已存在的引用是空操作）
				g_outlineClear(nullptr, &slot);
			}

			// ---- ③ 什么都没删到：至少把状态写回 12 ----
			++g_state.outlineRemoveMiss;
			if (pOk) {
				*p = kOutlineStateNone;
			}
		}

		// 原生模式下：把选中的目标挂上高亮，把掉队的目标摘掉。
		// 只有「新目标」和「距离上次重申超过 ReassertMs 的目标」才会真正调引擎
		// （每轮都全量调会白白吃掉主线程）。
		// ★ v2.1：ReassertMs=0 表示**不再周期性重申**（默认）。此时新目标的期限
		//   直接写成「永远不用重申」，避免 0 被当成「每轮都要重申」。
		void SyncNativeOutline(const std::vector<Candidate*>& a_chosen, std::uint64_t a_nowMs)
		{
			if (!g_state.nativeReady) {
				return;
			}
			static std::unordered_map<RE::TESObjectREFR*, bool> chosenSet;
			chosenSet.clear();
			chosenSet.reserve(a_chosen.size() * 2);
			for (auto* c : a_chosen) {
				chosenSet[c->ref] = true;
			}

			// 单轮的引擎调用预算（0 = 不限）。换场景会积压几百条待办，
			// 一口气做完就是一个可见的长卡顿，所以摊到后面几轮里慢慢做。
			const auto budgetLimit = g_cfg.maxOutlineOpsPerScan > 0
			                           ? static_cast<std::uint32_t>(g_cfg.maxOutlineOpsPerScan)
			                           : 0xFFFFFFFFu;
			std::uint32_t budget = budgetLimit;
			g_state.opsThisScan  = 0;
			g_state.opsDeferred  = 0;

			// ---- 1) 标记 / 取消标记「掉队」----
			// ★ v2.2 的宽限期：目标掉出集合后**先只打一个时间戳**，到点还没回来才真正摘。
			//   走过去时目标会在集合边缘反复进出，没有这一步就是每 200ms 一轮的
			//   Set/Remove 抖动（主线程被这些调用磨出 hitch）；有了它，绝大多数
			//   「进出」都只是改一个时间戳，**零引擎调用**。
			const auto grace = static_cast<std::uint64_t>(g_cfg.unhighlightGraceMs);
			for (auto& [ref, e] : g_state.outlined) {
				if (chosenSet.find(ref) != chosenSet.end()) {
					e.dropAt = 0;  // 又回来了：什么都不用做
				} else if (e.dropAt == 0) {
					e.dropAt = a_nowMs + grace;
				}
			}

			// ---- 2) 到期的摘除（受预算限制）----
			{
				PhaseTimer tUnh{ &g_state.tUnhMs };
				for (auto it = g_state.outlined.begin(); it != g_state.outlined.end();) {
					if (it->second.dropAt == 0 || a_nowMs < it->second.dropAt) {
						++it;
						continue;
					}
					if (budget == 0) {
						++g_state.opsDeferred;  // 预算用完，下一轮继续
						++it;
						continue;
					}
					--budget;
					++g_state.opsThisScan;
					UnoutlineRef(it->second.ref.get());
					it = g_state.outlined.erase(it);
				}
			}

			// ---- 3) 新目标 / 到重申时刻的（同样受预算限制）----
			// ★ v4.0：状态不再是全局一个 —— 每个候选带自己的 state（分类分色，
			//   由 Rescan 里的 ClassifyBase + g_cfg.stateByCategory 算好）。
			const auto reassert = static_cast<std::uint64_t>(g_cfg.reassertMs);
			const auto nextMs   = reassert ? a_nowMs + reassert : UINT64_MAX;
			PhaseTimer tAdd{ &g_state.tAddMs };
			for (auto* c : a_chosen) {
				auto it = g_state.outlined.find(c->ref);
				if (it == g_state.outlined.end()) {
					if (budget == 0) {
						++g_state.opsDeferred;
						continue;
					}
					--budget;
					++g_state.opsThisScan;
					if (OutlineRef(c->ref, c->state)) {
						OutlineEntry e;
						e.ref        = RE::NiPointer<RE::TESObjectREFR>{ c->ref };
						e.reassertMs = nextMs;
						e.state      = c->state;
						// ★ v4.7：记下挂的时候的 3D 根（之后只要它变了就说明 3D 被重建过 ⇒
						//   引擎侧那条登记已经丢了 ⇒ 见「4) 3D 复检」）
						e.last3D = RefGet3D(c->ref).get();
						g_state.outlined[c->ref] = std::move(e);
					}
				} else if (a_nowMs >= it->second.reassertMs) {
					if (budget == 0) {
						++g_state.opsDeferred;
						continue;
					}
					--budget;
					++g_state.opsThisScan;
					// 状态变了（理论上只在 INI 改过之后才会发生）：先把旧的摘掉，
					// 否则同一个引用会同时留在两个管理器里 ⇒ 两层描边。
					if (it->second.state != c->state) {
						UnoutlineRef(c->ref);
					}
					if (OutlineRef(c->ref, c->state)) {
						it->second.reassertMs = nextMs;
						it->second.state      = c->state;
						it->second.last3D     = RefGet3D(c->ref).get();  // ★ v4.7
					} else {
						// 挂不上（管理器又没了）：本轮撤账，下轮重新试
						it = g_state.outlined.erase(it);
					}
				}
			}

			// ================================================================
			// ★★ v4.7：4) 3D 复检 —— 引擎把描边丢了就重挂
			// ================================================================
			// 背景（用户实测「星球表面地图上，原本已经高亮的物体突然不亮了」）：
			//   描边是引擎按「3D 图节点」登记在 HighlightManager 里的（v2.3 实证：
			//   摘除用的 0x653F60 那个 id 取自 **3D 图的叶子节点**，不是 FormID）。
			//   外景走动时引擎会流式卸载 / 重建 3D（LOD ↔ 真模型互换就是最典型的），
			//   重建之后管理器里那条登记就没了 —— 而我们的账本还记着「挂着」，
			//   默认 `ReassertMs=0` 下**永远不会再 Set 一次** ⇒ 那个物体从此不亮，
			//   直到它掉出半径再进来（这正是用户看到的「突然不亮」）。
			//   与 v3.2 修的「用完扫描仪后全灭」是同一类问题：**账本与引擎侧脱节**，
			//   只是那一次是整表被销毁（有管理器数量下跌这条判据），这一次是单条丢失
			//   （管理器数量不变，任何全局判据都看不到）。
			//
			// 做法：每轮按游标复检 `Verify3DPerScan` 个**已挂**目标，
			//   发现它的 3D 根指针与「挂的时候记下的」不一样（重建过 / 第一次加载出来）
			//   就重新 Set 一次（同样走每轮预算）。
			//   ★ 只比指针、**绝不解引用**记下的那个旧指针（3D 可能已经销毁）。
			//   ★ 3D 现在是 null 就不动：看不见的东西本来就没描边，等它加载出来
			//     指针自然会变，那时再挂。
			//   ★ 成本：每轮最多 kVerify3DPerScan 次「取 3D」（虚调用 + 引用计数），
			//     默认 32/轮 = 160/s，与每轮 5~40 条 Set/Remove 相比可以忽略。
			//   诊断：统计行里的 `3D复检: probes=… reassert=…`（后者持续增长 = 这条
			//   自愈在真的干活；用户下次报告里这两个数就是证据）。
			if (g_state.nativeReady && g_cfg.verify3DPerScan > 0 && !a_chosen.empty()) {
				const std::size_t total = a_chosen.size();
				const std::size_t step  = std::min<std::size_t>(total, static_cast<std::size_t>(g_cfg.verify3DPerScan));
				std::uint32_t     probed = 0;
				for (std::size_t k = 0; k < step; ++k) {
					const std::size_t idx = (g_state.verifyCursor + k) % total;
					auto*             c   = a_chosen[idx];
					auto              it  = g_state.outlined.find(c->ref);
					if (it == g_state.outlined.end() || it->second.dropAt != 0) {
						continue;  // 已经不在选中集合里（待摘的不要复活）
					}
					++probed;
					auto root = RefGet3D(c->ref);
					if (!root) {
						continue;  // 3D 还没（重新）加载出来，等下一轮
					}
					const void* cur = root.get();
					if (cur == it->second.last3D) {
						continue;  // 3D 没变过 ⇒ 引擎侧那条登记还在，什么都不用做
					}
					if (budget == 0) {
						++g_state.opsDeferred;
						break;
					}
					--budget;
					++g_state.opsThisScan;
					if (OutlineRef(c->ref, c->state)) {
						it->second.last3D = cur;
						++g_state.outline3DReasserts;
						// 只打前 32 条 + 之后每 100 条一条（外景走动时可能很频繁，
						// 免得把日志刷爆）；趋势看统计行的 `3D复检: reassert=` 就够。
						if (g_state.outline3DReasserts <= 32 || g_state.outline3DReasserts % 100 == 0) {
							REX::INFO("native outline: 3D 重建（ref={:08X} 状态 {}）-> 重挂一次"
									  "（累计 {} 次；引擎侧描边按 3D 节点登记，重建即丢）",
								c->ref->GetFormID(), c->state, g_state.outline3DReasserts);
						}
					}
				}
				g_state.outline3DProbes += probed;
				g_state.verifyCursor = (g_state.verifyCursor + step) % total;
			}
		}

		// 把当前所有已挂高亮的引用全部摘掉。
		// ★ 只给「用户按热键关掉」用（要的就是立刻全灭，卡一下也认）。
		//   换场景 / 读档**不要**走这里 —— 那是几百条调用的突发，会卡；
		//   那条路走 MarkAllForRemoval()，摊到后面几轮慢慢摘。
		void ClearAllNativeOutline()
		{
			if (g_state.outlined.empty()) {
				return;
			}
			const auto n         = g_state.outlined.size();
			const auto okWas     = g_state.outlineRemoved;
			const auto missWas   = g_state.outlineRemoveMiss;
			const auto uhMissWas = g_state.outlineUnhighlightMiss;
			for (auto& [key, entry] : g_state.outlined) {
				UnoutlineRef(entry.ref.get());
			}
			g_state.outlined.clear();
			REX::INFO("native outline cleared (n={} ok={} unhMiss={} removeMiss={} totalOk={})",
				n,
				g_state.outlineRemoved - okWas,
				g_state.outlineUnhighlightMiss - uhMissWas,
				g_state.outlineRemoveMiss - missWas,
				g_state.outlineRemoved);
		}

		// 换场景 / 读档用：**不立刻动手**，只把每个已挂高亮的引用标上「到这个时刻才摘」。
		// 真正的摘除由 SyncNativeOutline 按每轮预算慢慢做完。
		void MarkAllForRemoval(std::uint64_t a_deadlineMs)
		{
			if (g_state.outlined.empty()) {
				return;
			}
			for (auto& [key, entry] : g_state.outlined) {
				entry.dropAt = a_deadlineMs;
			}
			REX::INFO("native outline: {} 个已挂高亮转入「待摘」（deadline=+{}ms，按每轮 {} 条的预算慢慢摘）",
				g_state.outlined.size(),
				a_deadlineMs > NowMs() ? a_deadlineMs - NowMs() : 0,
				g_cfg.maxOutlineOpsPerScan);
		}

		// ====================================================================
		// ★ v3.2：引擎把我们的高亮清掉之后的「自愈」
		// ====================================================================
		// 起因（用户实测）：**打开原版扫描仪再关闭后，MOD 的高亮全灭，且不会自己回来**
		// —— 得按两下 F8（先关再开）才行。根因是引擎在扫描仪生命周期里会拆掉 Monocle
		// HUD（RVA 0x17D4B30：销毁 11 个 HighlightManager + 清空「引用→状态」表 +
		// 摘掉渲染侧的高亮），而我们的 `g_state.outlined` 里还记着那些引用 ⇒
		// `SyncNativeOutline` 认为「已经挂着」，于是**永远不再重挂**。
		//
		// 这与 docs/03 第 12.4 节预判的「低概率整表清空」是同一件事，只是它其实
		// 在正常开关扫描仪时就会发生（当初的结论说「不触发」，这次实测推翻了）。
		//
		// 防御分两路（都**不做任何引擎调用**，只改我们自己的时间戳，
		// 真正的重挂交给 SyncNativeOutline 按 MaxOutlineOpsPerScan 分批做 ⇒ 零突发）：
		//   ① **`MonocleMenu` 由开变关的那一刻** —— 直接对应「放下扫描仪」；
		//   ② **存活管理器数下跌** —— 「整表清空」必伴随 11 个管理器被销毁
		//      （0x17D4B30 会把槽清 0），这条同时兜住「读档 / 回主菜单 / UI 大重建」。
		// ====================================================================

		// 把「已经在表里、但引擎侧可能已经丢了」的目标标成「下轮重申」。
		// 只改两个时间戳：dropAt=0（别把它当掉队摘掉）、reassertMs=0（下一轮就重挂）。
		//
		// ★ v4.0：**只重申「还在选中集合里」的条目**（dropAt == 0）。
		//   已经被 ResetForNewScene / MarkAllForRemoval 标成「待摘」的（dropAt != 0）
		//   属于**上一个世界**的引用 —— 读档后如果把它们又挂回去，就是白做一轮
		//   Set（而且会拖到它们掉出选中集合才被摘掉）。
		void MarkAllForReassert(const char* a_reason)
		{
			if (g_state.outlined.empty()) {
				return;  // 没有已挂的东西，无所谓
			}
			std::size_t touched = 0;
			for (auto& [key, entry] : g_state.outlined) {
				if (entry.dropAt != 0) {
					continue;  // 已标「待摘」= 上个世界的遗留，不要复活它
				}
				entry.reassertMs = 0;
				++touched;
			}
			if (touched == 0) {
				return;
			}
			++g_state.outlineResyncs;
			REX::INFO("native outline: 引擎侧高亮疑似丢失（{}）-> {} 个已挂目标转入「待重申」"
					  "（按每轮 {} 条预算重挂，累计 {} 次）",
				a_reason, touched, g_cfg.maxOutlineOpsPerScan,
				g_state.outlineResyncs);
		}

		void DetectEngineOutlineLoss(std::uint64_t a_nowMs)
		{
			// MonocleMenu 的开/关状态**每帧都跟踪**（即使功能关着），
			// 这样开关功能不会造成一次假的「由开变关」。
			const bool monocleOpen = IsMonocleMenuOpen();
			const bool justClosed  = g_state.monocleOpen && !monocleOpen;
			g_state.monocleOpen    = monocleOpen;

			if (!g_state.nativeReady || !g_state.on) {
				g_state.lastLiveManagers = CountLiveManagers();
				return;
			}

			// ★ v4.0：换场景 / 读档之后的「静置期」内不重挂。
			//   那段时间 `outlined` 里记的是**上一个世界**的引用，ResetForNewScene
			//   已经把它们标成「待摘」；此时再触发「重挂」会把它们又挂回去
			//   （白白多一轮 Set，而且会拖到它们掉出选中集合才被摘掉）。
			if (a_nowMs < g_state.settleUntilMs) {
				g_state.lastLiveManagers = CountLiveManagers();
				return;
			}

			// ① 放下扫描仪的那一刻：引擎很可能刚刚把整张表清过 ⇒ 直接重挂。
			bool fired = false;
			if (justClosed && g_cfg.resyncOnScannerClose) {
				MarkAllForReassert("MonocleMenu 关闭");
				fired = true;
			}

			// ② 管理器数量下跌：`0x17D4B30` 的「销毁 + 清表」一定会让这个数掉下来
			//    （11 → 0）。举着扫描仪期间不判断（那期间引擎自己在管管理器）；
			//    ① 已经命中时不重复（同一次「清表」两个判据会同时成立）。
			const auto live = CountLiveManagers();
			if (!fired && !monocleOpen && g_state.lastLiveManagers != 0 && live < g_state.lastLiveManagers) {
				MarkAllForReassert("HighlightManager 数量下跌");
			}
			g_state.lastLiveManagers = live;
		}

		// ====================================================================
		// 扫描
		// ====================================================================
		void Rescan(std::uint64_t a_nowMs, RE::PlayerCharacter* a_player)
		{
			auto* cell = a_player->parentCell;
			if (!cell) {
				++g_state.skipNoCell;
				return;
			}

			// 本轮要遍历的 cell 表：
			//   [0] = 当前 cell（带「引用数组稳定性」判据，见下）
			//   [1..] = 「近期 cell 环」里形状校验通过的 cell（★ v4.7，见 kRingCellMax）
			// 环的意义有两个：① 跨 cell 边界时**边界对面的引用**也能亮；
			//                  ② 换 cell 时不必把账本整批作废（配合 Tick 里的连续过渡）。
			struct ScanCell
			{
				RE::TESObjectCELL*        cell  = nullptr;
				RE::TESObjectREFR* const* list  = nullptr;
				std::uint32_t             count = 0;
			};
			static std::array<ScanCell, 1 + kRingCellMax> scanCells;
			std::size_t                                    cellCount = 0;

			RawArray    arr{};
			std::size_t usedOff = 0;
			{
				// ★ v2.3：这一段就是实测里那个「什么都没干却花 31ms」的元凶，
				//   单独计时 + 配合 g_vqCalls（真的问了几次内核）一起看。
				PhaseTimer tShape{ &g_state.tShapeMs, &g_state.tShapeMax };

				// cell 指针本身也必须可读：下面的 ReadRawArray 会直接解引用 cell+off，
				// 这一条几乎零成本，专门挡「parentCell 偏移万一也不对」的极端情况。
				if (!IsReadable(cell, 0x90)) {
					++g_state.refsRejected;
					return;
				}

				// ★ 绝不调 cell->IsAttached() / cell->ForEachReference()：
				//   两者都按 commonlibsf 的「声明偏移」访问 TESObjectCELL 成员，而那套偏移
				//   **整体比真实大 8 字节**（见 kCellRefsOffCandidates 上方的完整说明）。
				//   用 ForEachReference 就是 2026-09-16 那次崩溃（读到垃圾 BSTArray）。
				//
				// ★ 也不靠「猜一个偏移」：对候选偏移逐个做**形状校验**
				//   （BSTArray 头自洽 + 抽查元素确实是 REFR/ACHR），谁通过就用谁。
				//   一个都不过 = 读到垃圾 / 数组还没建好 → 这一轮不扫描。
				for (const auto off : kCellRefsOffCandidates) {
					const auto cand = ReadRawArray(cell, off);
					if (ValidateCellRefs(cand, kRefsShapeSamples)) {
						arr     = cand;
						usedOff = off;
						break;
					}
				}
				if (usedOff == 0) {
					++g_state.refsRejected;
					g_state.stableRounds = 0;
					return;
				}
				// 换了个偏移 = 之前读的不是引用数组 → 重新开始数稳定轮数
				if (usedOff != g_state.cellRefsOff) {
					g_state.cellRefsOff  = usedOff;
					g_state.lastRefsSize = arr.size;
					g_state.stableRounds = 0;
					++g_state.skipUnstable;
					return;
				}
				// ★ v4.7：引用数判据分两档 ——
				//   · **突变**（第一次进世界 / 换场景的批量填充）⇒ 还没稳定，等稳定轮数
				//   · **轻微变化**（≤ StreamJumpTolerance）⇒ 加载线程在流式增删引用，
				//     这正是「走动时」最常见的形态，**不再打断扫描**。旧判据是
				//     「任何长度变化 ⇒ stableRounds 归零 + return」，而 stableRounds 要
				//     连续两轮才放行 ⇒ 每次变化跳过约 3 轮（≈600ms）；外景 / 城市里走动时
				//     加载线程一直在增删 ⇒ 扫描被反复打断 = 用户报的「行走时似乎不扫描」
				//     的第二个来源。形状校验每轮都做 + 元素逐个校验，安全性不变。
				const std::uint64_t prevSize = g_state.lastRefsSize;
				const std::uint64_t delta    = arr.size > prevSize ? arr.size - prevSize : prevSize - arr.size;
				const std::uint64_t tol      = static_cast<std::uint64_t>(g_cfg.streamJumpTolerance);
				if (prevSize == 0 || delta > tol) {
					g_state.lastRefsSize = arr.size;
					g_state.stableRounds = 0;
					++g_state.skipUnstable;
					return;
				}
				if (delta != 0) {
					++g_state.streamMoves;  // 流式变化：记一笔，照常扫描
				}
				g_state.lastRefsSize = arr.size;
				// 长度连续稳定若干轮才认为世界已经稳定下来
				if (++g_state.stableRounds < kRefsStableRounds) {
					++g_state.skipWarmup;
					return;
				}

				scanCells[cellCount++] = ScanCell{ cell,
					reinterpret_cast<RE::TESObjectREFR* const*>(arr.data),
					std::min<std::uint32_t>(arr.size, kMaxRefsSanity) };
				// 当前 cell 进环（= 玩家真的在这里；「连续过渡」判据与「边界对面也扫」都靠它）
				RingTouch(cell, a_nowMs, arr.size);
			}

			// ---- ★ v4.7：环里其它 cell（形状 + 自洽性都过才一起扫）----
			{
				for (auto& rc : g_state.ring) {
					if (cellCount >= scanCells.size()) {
						break;
					}
					if (!rc.cell || rc.cell == cell) {
						continue;
					}
					RawArray ringArr{};
					bool     ok = false;
					if (IsReadable(rc.cell, 0x90)) {
						for (const auto off : kCellRefsOffCandidates) {
							const auto cand = ReadRawArray(rc.cell, off);
							if (ValidateRingCellRefs(cand, rc.cell, kRefsShapeSamples)) {
								ringArr = cand;
								ok      = true;
								break;
							}
						}
					}
					if (!ok) {
						// 校验不过 = 这个 cell 已经卸载（或对象被释放）⇒ 累计几次就踢出环
						++rc.miss;
						++g_state.ringSkipped;
						if (rc.miss >= kRingCellMissMax) {
							rc = State::RingCell{};
						}
						continue;
					}
					rc.miss   = 0;
					rc.seenMs = a_nowMs;
					rc.refs   = ringArr.size;
					scanCells[cellCount++] = ScanCell{ rc.cell,
						reinterpret_cast<RE::TESObjectREFR* const*>(ringArr.data),
						std::min<std::uint32_t>(ringArr.size, kMaxRefsSanity) };
				}
				RingTrimByRefs();
			}
			g_state.ringCells = static_cast<std::uint32_t>(cellCount > 0 ? cellCount - 1 : 0);

			const RE::NiPoint3 origin  = a_player->GetPosition();
			const float        radiusU = g_cfg.radiusMeters * g_cfg.unitsPerMeter;
			const float        radius2 = radiusU * radiusU;

			// 「只高亮正前方」：用玩家偏航角做水平夹角判定，把身后的目标排除。
			// 原版那个「中央圆圈」被本 MOD 去掉之后，靠它维持「看得见才亮」的观感。
			// ⚠️ 偏航角→前向量的符号约定是按 Gamebryo/NetImmerse 的常规写法来的，
			//    如果实测发现左右/前后反了，改 kYawForwardSign 即可。
			const bool  useFront = g_cfg.onlyInFront && g_cfg.frontFovDeg < 359.0f;
			const float frontCos = useFront ? std::cos(g_cfg.frontFovDeg * 0.5f * 0.017453292f) : -1.0f;
			const float yaw      = a_player->data.angle.z;
			const float fwdX     = std::sin(yaw) * kYawForwardSign;
			const float fwdY     = std::cos(yaw);

			static std::vector<Candidate>   cands;
			std::vector<Candidate*>         chosen;
			cands.clear();
			cands.reserve(512);
			g_state.candTypes.fill(0);  // ★ v4.1：候选类型直方图只统计「最近一轮」

			// ★ v2.3：遍历 + 排序 + 挑选这一整段（纯内存读，理论上应该在 1ms 量级；
			//   实测却是 30ms 上下，所以必须单独计时把它和形状校验分开看）。
			{
				PhaseTimer tLoop{ &g_state.tLoopMs, &g_state.tLoopMax };

			// ★ v4.7：对「当前 cell + 环内 cell」逐个遍历（见上面 scanCells 的说明）。
			//   每个 cell 只认**它自己**的引用（`ref->parentCell == sc.cell`）：
			//   这条既是最强的自洽判据，也顺带把「读到别的 cell 的引用」挡在外面。
			for (std::size_t ci = 0; ci < cellCount; ++ci) {
			const auto& sc = scanCells[ci];
			auto* const*        list  = sc.list;
			const std::uint32_t count = sc.count;

			// ★ v4.2：库存列表偏移标定（**每会话一次**，只拿容器当样本）。
			//   放在这里是因为它要顺序扫引用找样本；标定完成后这一段不再执行，
			//   热路径里一次 VirtualQuery 都不会有。
			if (!g_state.invCalibDone && g_cfg.skipEmptyLoot) {
				CalibrateInventory(list, count);
			}

			for (std::uint32_t i = 0; i < count; ++i) {
				auto* ref = list[i];
				// 全部是纯内存读，零引擎调用
				if (!ref || !IsPlausiblePointer(reinterpret_cast<std::uint64_t>(ref))) {
					continue;
				}
				if (ref == a_player || ref->IsDeleted() || ref->IsDisabled() || ref->IsPlayerRef()) {
					++g_state.skipDeleted;
					continue;
				}
				if (ref->parentCell != sc.cell) {
					++g_state.skipParentCell;
					continue;
				}
				// ★ 先判距离再判类型：这样「白名单没通过」的统计只在半径内做，
				//   日志里的类型统计才是有意义的（否则满屏都是墙和地形的类型）。
				const float d2 = origin.GetSquaredDistance(ref->data.location);
				if (d2 > radius2) {
					continue;
				}

				const auto* base = ref->data.objectReference.get();
				// ★ v4.2：分类改成「按引用自己」（ClassifyRef）——
				//   ACHR 引用要看死活（活人不亮 = 红线；尸体要亮），
				//   「base = NPC_/LVLN 的普通 REFR」= 尸体道具。
				bool      isCorpse = false;
				const int cat      = ClassifyRef(ref, base, d2, isCorpse);
				if (cat < 0) {
					// 诊断：半径内、类型不在白名单 → 记一笔类型直方图（见统计日志 rejTypes=）
					if (base) {
						++g_state.rejectTypes[static_cast<std::uint8_t>(base->GetFormType()) & 0xFF];
					}
					continue;
				}
				// ★ v4.1：类别开关关掉的（默认只有 kOther = MSTT，即纸箱/桌椅/吧台这类
				//   不能拾取进背包的装饰物；见 Config::categoryEnabled 的说明）。
				//   与「不在白名单」分开计数，日志里 `disabled=N` 能确认开关真的生效。
				if (!g_cfg.categoryEnabled[static_cast<std::size_t>(cat)]) {
					++g_state.disabledSkips;
					continue;
				}
				// ★ v4.2：容器 / 尸体「搜空即熄灭」——库存为空就不进候选集合。
				//   掉出候选之后由 SyncNativeOutline 的宽限期（UnhighlightGraceMs）
				//   在约 1.5 秒内把描边摘掉 ⇒ 观感是「刚搜完就灭」。
				//   判空是纯内存读；读不到 / 形状不对 ⇒ 未知 ⇒ 按「有东西」处理（照常亮）。
				//
				//   ★★ v4.9：展示柜（武器箱 / 武器架 / 头盔架…）是例外 —— 它的内容只在
				//     搜刮界面打开期间以 `kTemporary` 条目投影进库存，关着时读到
				//     size=0（用户实测：「关着不亮、一打开才亮」）。
				//     ⇒ 这类容器**不因读到「空」而熄灭**，否则关着的武器箱永远不亮。
				//     识别 = 静态白名单（SasDisplayCases.h，118 个原版记录）
				//     ∨ 运行期学习集合；完整实证见常量区「v4.9 展示柜」长注释。
				//     开关：INI `SkipDisplayCaseEmpty=0` 可退回旧行为。
				const bool displayCase =
					(!isCorpse && cat == static_cast<int>(Category::kContainer) &&
						g_cfg.skipDisplayCaseEmpty && IsDisplayCaseBase(base));
				if (isCorpse || cat == static_cast<int>(Category::kContainer)) {
					if (g_cfg.skipEmptyLoot && g_invOff != 0) {
						const int loot = RefLootState(ref);
						// ★ v4.8：容器「判空链路快照」探针（首次 + 判决变化时各一条）。
						//   用户报「武器箱关着不亮、一打开就亮」—— 这两条记录就是答案：
						//   见 ContProbe 顶部的长注释（关着时的 size/sum/skipNp/skipEq
						//   与打开后一对比即可定位）。
						if (cat == static_cast<int>(Category::kContainer) && !isCorpse) {
							ContProbe(ref, base, d2, loot, displayCase);
						}
						if (loot == 0) {
							if (!displayCase) {
								++g_state.emptySkips;
								continue;  // 空 ⇒ 不进候选 ⇒ 宽限期后熄灭
							}
							// ★ v4.9：展示柜读到「空」不算空（关闭时内容本来就不在库存里）
							++g_state.displayCaseSkips;
						} else if (loot == 1) {
							++g_state.lootNotEmpty;
							// ★ v4.4：「判到有东西」的尸体 ⇒ 打一份**库存明细**
							//   （只对近距离的、每个 ref 只打一次；上限 LootProbeMax）。
							//   这是「拿空还亮」的取证主力：残留条目会被逐条列出。
							if (isCorpse) {
								LootProbe(ref, d2);
							}
						} else if (loot == -2) {
							// ★ v4.3：细分「有 / 未知 / 库存指针为 null / 未标定」——
							//   用户报「搜空还亮」时，这四个数直接指出卡在哪一步：
							//     empty 不涨 + unknown 一直涨   ⇒ 形状不对（可能标定选错偏移）
							//     empty 不涨 + null 一直涨      ⇒ 库存指针是 null（引擎没建/已销毁）
							//     empty 不涨 + notEmpty 一直涨  ⇒ 库存里真有引擎条目（不可见物品）
							++g_state.lootNullInv;
						} else if (loot == -1) {
							++g_state.lootUnknown;
						} else {
							++g_state.lootBadShape;
						}
					}
				}

				// 水平夹角判定（只高亮正前方时）
				if (useFront) {
					const float dx    = ref->data.location.x - origin.x;
					const float dy    = ref->data.location.y - origin.y;
					const float hlen2 = dx * dx + dy * dy;
					if (hlen2 > 0.01f) {
						const float inv = 1.0f / std::sqrt(hlen2);
						if ((dx * fwdX + dy * fwdY) * inv < frontCos) {
							continue;
						}
					}
				}

				// ★ v4.1：诊断 —— 本轮真正进入候选集合的目标按 base formType 计数
				//   （统计日志里 `candTypes` 那行；「某东西不该亮」时看它的 formType）
				++g_state.candTypes[static_cast<std::uint8_t>(base->GetFormType()) & 0xFF];

				Candidate c;
				c.ref = ref;
				c.d2  = d2;
				// 已经在 outline 表里就是「亮着」（黏性靠它）
				c.lit = g_state.outlined.find(ref) != g_state.outlined.end();
				// ★ v4.0：分类分色 —— 类别 → 该用哪个 outline 状态
				c.state = static_cast<std::uint32_t>(g_cfg.stateByCategory[static_cast<std::size_t>(cat)]);
				c.cat   = static_cast<std::uint8_t>(cat);
				// 已发光的按 0.5 折扣参与排序 = 黏性：站着不动目标不抖，
				// 走动时近的新目标仍然能顶掉远的旧目标。
				c.key = c.lit ? d2 * 0.5f : d2;
				cands.push_back(c);
			}
			}  // ← v4.7：cell 循环结束（当前 cell + 环内 cell）

			g_state.candCount = cands.size();

			std::sort(cands.begin(), cands.end(),
				[](const Candidate& a, const Candidate& b) { return a.key < b.key; });

			// ★ v2.1：去掉「每 2 米一档、每档最多 6 个」的分档限流。
			//   那个限流本来是想「别让目标全挤在脚边」，但它有个非常直观的副作用：
			//   同一层货架上距离几乎一样的东西，超出每档配额的就**永远不亮**
			//   —— 实测截图里货架上就有两个罐子/碗不亮，正是同一档被前面 6 个占满。
			//   原版扫描仪没有这种配额，范围内该亮的都亮，所以这里改成
			//   「按距离（含黏性折扣）从近到远一路取，直到 MaxTargets 上限」。
			chosen.reserve(std::min<std::size_t>(cands.size(), static_cast<std::size_t>(g_cfg.maxTargets)));
			for (auto& c : cands) {
				if (chosen.size() >= static_cast<std::size_t>(g_cfg.maxTargets)) {
					break;
				}
				chosen.push_back(&c);
			}
			g_state.selCount = chosen.size();

			// ★ v4.0：分类命中数（诊断）—— 本轮选中的目标按类别计数
			for (auto* c : chosen) {
				++g_state.categoryCounts[c->cat];
			}
			}  // ← tLoop 计时块结束
			// ------------------------------------------------------------------
			// ★ 视觉层：唯一的一条路 —— 原生 outline
			//   （引擎自己画描边，和原版手持扫描仪同一套渲染；不需要 Papyrus、
			//    不需要 EFSH、也不需要令牌桶）
			// ------------------------------------------------------------------
			{
				PhaseTimer tSync{ &g_state.tSyncMs, &g_state.tSyncMax };
				SyncNativeOutline(chosen, a_nowMs);
			}
		}

		// ====================================================================
		// 每帧主循环
		// ====================================================================
		void Tick()
		{
			// ③ 只看主线程
			if (::GetCurrentThreadId() != g_mainThreadId.load()) {
				return;
			}
			std::unique_lock lock{ g_tickLock, std::try_to_lock };
			if (!lock.owns_lock()) {
				return;
			}

			++g_state.tickCount;
			const std::uint64_t now = NowMs();

			// ★ v4.7：Tick 间隔诊断 —— 「高亮不更新」到底是「Tick 根本没被引擎调用」
			//   还是「调了但被下面的早退挡住」，这两个数一眼就能分开。
			//   实测里出现过 39 秒一个 scan 都没有的窗口（当时两种可能分不出来），
			//   这就是加它的原因：间隔 > kTickGapLogMs 就记一笔并打一行 WARN。
			if (g_state.lastTickMs && now - g_state.lastTickMs > kTickGapLogMs) {
				const auto gap = now - g_state.lastTickMs;
				++g_state.tickGaps;
				g_state.tickGapMsTotal += gap;
				if (gap > g_state.tickGapMsMax) {
					g_state.tickGapMsMax = gap;
				}
				REX::WARN("tick gap {}ms（这段时间引擎没有调用本插件的主循环任务："
						  "游戏暂停 / 载入 / 主线程忙？）loading={} monocle={} on={} cell={:08X}",
					gap,
					g_state.loadingNow ? 1 : 0,
					IsMonocleMenuOpen() ? 1 : 0,
					g_state.on ? 1 : 0,
					g_state.lastCell ? g_state.lastCell->GetFormID() : 0);
			}
			g_state.lastTickMs = now;

			PollHotkey(now);

			// ★ v4.0：热键提示（写 GLOB，桥脚本轮询后弹 HUD）。
			//   放在各种提前 return 之前，保证「关掉功能」那一刻的提示也发得出去。
			FlushNotify();

			// 绑定（只用于热键提示）。**失败不影响高亮** —— 高亮完全在 DLL 里，
			// 所以这里不像旧版那样「绑不上就整轮 return」。
			if (!g_state.bound) {
				BindNotify();
			}

			// ★ v4.0：读档 / 大重建信号 =「载入画面由开变关」（替代旧的 SAS_Epoch GLOB）。
			//   每帧都看一眼（两次 IsMenuOpen，成本可忽略），这样才能在
			//   「扫完这一轮就 return」的路径之外也捕捉到。
			//   与「换 cell」判据重复触发是无害的：ResetForNewScene 只是重新打
			//   「待摘」时间戳，本来就会在下一轮被覆盖。
			{
				const bool loading = IsLoadingScreenUp();
				if (g_state.loadingSeen && !loading) {
					++g_state.loadGameResets;
					g_state.cellRefsOff   = 0;
					g_state.lastRefsSize  = 0;
					g_state.stableRounds  = 0;
					g_state.settleUntilMs = now + kSettleAfterSceneChangeMs;
					InvalidateReadRegions();
					// ★ v4.7：经过载入画面的切换 = 世界重开 ⇒ 「近期 cell 环」也必须作废
					//   （旧 cell 的指针随时可能被释放；而且这样「换 cell」判据才不会
					//    把一个全新的内景误判成「连续过渡」）。
					RingClear();
					ResetForNewScene(now, "载入画面关闭（读档 / 换场景）");
				}
				g_state.loadingSeen = loading;
			}

			// ★ v3.2：检测「引擎把我们的高亮清掉了」的时机并自动重挂
			//   （核心场景 = 玩家用原版手持扫描仪；见 DetectEngineOutlineLoss 的说明）。
			//   放在扫描节流之前：切换要即时被看到，不能等 200ms 的扫描窗口。
			//   ★ v4.0：放在读档处理**之后** —— 读档会把已挂目标标成「待摘」，
			//     顺序反了会被「重挂」覆盖掉（见 DetectEngineOutlineLoss 里的静置期判断）。
			DetectEngineOutlineLoss(now);

			if (!g_state.on) {
				++g_state.skipOff;
				return;
			}

			// 扫描节流
			if (now - g_state.lastScanMs < static_cast<std::uint64_t>(g_cfg.scanIntervalMs)) {
				++g_state.skipThrottle;
				return;
			}
			g_state.lastScanMs = now;

			// ★ v2.2：载入画面期间不做任何引擎调用（哪怕只是「摘掉上一批」）。
			//   载入时主线程在跑加载，我们每条调用都会被拖慢几十毫秒，
			//   几百条叠起来就是实测日志里那个 18 秒的空档。
			g_state.loadingNow = IsLoadingScreenUp();
			if (g_state.loadingNow) {
				++g_state.skipLoading;
				return;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				++g_state.skipNoPlayer;
				return;
			}
			// ★ v4.7：移动距离（诊断）—— 每轮扫描累加玩家位移，统计行里打
			//   `move=X.Xm`。用它把「行走」和「扫描被跳过 / 目标变化」对起来看。
			{
				const RE::NiPoint3 pos = player->GetPosition();
				if (g_state.havePos) {
					const float dx = pos.x - g_state.lastPos.x;
					const float dy = pos.y - g_state.lastPos.y;
					const float dz = pos.z - g_state.lastPos.z;
					g_state.moveMeters += std::sqrt(dx * dx + dy * dy + dz * dz) / g_cfg.unitsPerMeter;
				} else {
					g_state.havePos = true;
				}
				g_state.lastPos = pos;
			}
			auto* cell = player->parentCell;
			if (!cell) {
				g_state.lastCell = nullptr;
				++g_state.skipNoCell;
				return;
			}
			if (cell != g_state.lastCell) {
				const bool first = (g_state.lastCell == nullptr);
				// ★★ v4.7：连续过渡判据 —— 上一个 cell 还在「近期 cell 环」里
				//   （= 它是我们自己最近待过、并且形状校验一直通过的 cell），
				//   说明世界没有被重开：外景里跨过 cell 边界走一步就是这种。
				//   依据与完整推导见常量区 kSettleOnCellCrossMs 上方的长注释。
				const bool continuous = g_cfg.exteriorContinuous && !first && RingContains(g_state.lastCell);
				g_state.lastCell = cell;
				// 换 cell（无论哪种）：重置引用数组稳定性判据。
				//   引擎此刻正在往新 cell 的引用数组里塞东西，parentCell 可能还是
				//   半初始化状态（上一代项目 v18 就是在这里崩的）。
				g_state.cellRefsOff   = 0;
				g_state.lastRefsSize  = 0;
				g_state.stableRounds  = 0;
				++g_state.cellChanges;
				if (continuous) {
					// 世界是连续的 ⇒ **不整批摘、不清账本**，只短静置一下：
					//   谁真的离开半径 / 掉出候选集合，就按 UnhighlightGraceMs 自然淘汰。
					++g_state.cellContinuous;
					g_state.settleUntilMs = now +
						static_cast<std::uint64_t>(g_cfg.settleOnCellCrossMs > 0 ? g_cfg.settleOnCellCrossMs : 0);
					RingTouch(cell, now, 0);  // 先把新 cell 放进环（续上「连续」这条链）
					REX::INFO("cell cross (连续过渡): 账本保留 {} 个已挂目标，静置 {}ms，环={} 个 cell",
						g_state.outlined.size(), g_cfg.settleOnCellCrossMs, RingCount());
				} else {
					g_state.settleUntilMs = now + kSettleAfterSceneChangeMs;
					InvalidateReadRegions();  // v2.3：旧 cell 的「可读区间」不再可信
					RingClear();              // 新世界：环作废（重新从当前 cell 开始攒）
					ResetForNewScene(now, first ? "first cell" : "cell changed（换场景）");
				}
			}
			if (now < g_state.settleUntilMs) {
				++g_state.skipSettle;  // 静置期内不扫描
				return;
			}

			// ★ v4.0.1：进入世界后的第一次扫描 —— 做一次性「配色刷新」
			//   （引擎设置此时肯定已加载完；详见 RefreshOutlineParamsOnce 的说明）
			RefreshOutlineParamsOnce();

			const auto scanT0 = NowMs();
			Rescan(now, player);
			const auto scanDt = NowMs() - scanT0;
			g_state.scanMsTotal += scanDt;
			if (scanDt > g_state.scanMsMax) {
				g_state.scanMsMax = scanDt;
			}
			++g_state.scanMsSamples;
			++g_state.scanCount;

			if (g_cfg.logStats && now - g_state.lastStatsMs > kStatsLogIntervalMs) {
				g_state.lastStatsMs = now;
				// ★ v4.7：新增 `cells=`（本轮扫了几个 cell：第一个是当前 cell，其余是
				//   「近期 cell 环」里的）与 `move=`（窗口内玩家走了多少米 —— 用它把
				//   「行走」和「扫描被跳过」对起来看）。
				REX::INFO("scan#{} cell={:08X} off=0x{:X} refs={} cells={} cand={} sel={} outline={}/{}/{} on={} resync={} monocle={} load={} notify={} move={:.1f}m",
					g_state.scanCount,
					cell->GetFormID(),
					g_state.cellRefsOff,
					g_state.lastRefsSize,
					1 + g_state.ringCells,
					g_state.candCount,
					g_state.selCount,
					g_state.outlined.size(),
					g_state.nativeReady ? 1 : 0,
					CountLiveManagers(),
					g_state.on ? 1 : 0,
					g_state.outlineResyncs,
					g_state.monocleOpen ? 1 : 0,
					g_state.loadGameResets,
					g_state.notifyWrites,
					g_state.moveMeters);

				// ★★ v4.7：诊断组 —— 「高亮不更新 / 没在扫描」的一次日志定位。
				//   · skip: 每种早退各跳过多少轮（throttle 是正常节流，5/s）
				//   · tick: gaps>0 = 引擎有段时间根本没调用主循环任务（暂停 / 载入 /
				//     主线程忙），这是与「被早退挡住」完全不同的两回事
				//   · cell: changes = 换 cell 次数，连续过渡 = 其中走「不整批摘」的
				//   · 环: cell = 最近一轮多扫了几个「近期待过的 cell」；skipped = 校验
				//     不过被跳过（= 那个 cell 卸载了）
				//   · 流式变化 = 引用数轻微增删但照常扫描的次数（外景走动时应该 >0）
				//   · 3D复检: probes/reassert —— reassert 增长 = 引擎侧描边真的丢过、
				//     且被这条自愈重新挂上了（「高亮丢失」的直接证据）
				REX::INFO("  skip (窗口内): off={} throttle={} loading={} noplayer={} nocell={} settle={} shape={} unstable={} warmup={}",
					g_state.skipOff, g_state.skipThrottle, g_state.skipLoading,
					g_state.skipNoPlayer, g_state.skipNoCell, g_state.skipSettle,
					g_state.refsRejected, g_state.skipUnstable, g_state.skipWarmup);
				REX::INFO("  tick/场景 (窗口内): tick间隔>{}ms={} maxGap={}ms 合计={}ms | cell: changes={} 连续过渡={} | 环: cell={} skipped={} | 流式变化={} | 3D复检: probes={} reassert={} | move={:.1f}m",
					kTickGapLogMs, g_state.tickGaps, g_state.tickGapMsMax, g_state.tickGapMsTotal,
					g_state.cellChanges, g_state.cellContinuous,
					g_state.ringCells, g_state.ringSkipped,
					g_state.streamMoves,
					g_state.outline3DProbes, g_state.outline3DReasserts,
					g_state.moveMeters);
				g_state.skipOff        = 0;
				g_state.skipThrottle   = 0;
				g_state.skipLoading    = 0;
				g_state.skipNoPlayer   = 0;
				g_state.skipNoCell     = 0;
				g_state.skipSettle     = 0;
				g_state.refsRejected   = 0;
				g_state.skipUnstable   = 0;
				g_state.skipWarmup     = 0;
				g_state.tickGaps       = 0;
				g_state.tickGapMsMax   = 0;
				g_state.tickGapMsTotal = 0;
				g_state.cellChanges    = 0;
				g_state.cellContinuous = 0;
				g_state.ringSkipped    = 0;
				g_state.streamMoves    = 0;
				g_state.outline3DProbes    = 0;
				g_state.outline3DReasserts = 0;
				g_state.moveMeters     = 0.0f;

				// ★ v4.0：分类命中数 —— 每个类别本轮各挂了多少个（配色不对时靠它定位）
				{
					std::string cats;
					for (std::size_t i = 0; i < kCategoryCount; ++i) {
						if (i) {
							cats += ", ";
						}
						cats += kCategoryName[i];
						cats += "=";
						cats += std::to_string(g_state.categoryCounts[i]);
					}
					g_state.categoryCounts.fill(0);
					// ★ v4.1：disabled = 窗口内因为「类别开关 = 0」被跳过的引用数
					//   （默认只有 kOther/MSTT 关着；它应该持续是一个正数）
					REX::INFO("  category (本轮选中): {} | 类别开关跳过 disabled={}", cats, g_state.disabledSkips);
					g_state.disabledSkips = 0;
				}

				// ★ v4.2：尸体 / 搜空（诊断；计数**每轮扫描都累加**，
				//   所以同一个尸体在半径内待 5 秒的计数会是它的 ~25 倍 —— 看趋势即可）。
				//   ★ v4.3 排查方法（一次日志就能定位）：
				//     · 「尸体该亮却没亮」→ 先看 `ACHR: 见到=N 判活跳过=M`：
				//         M 在涨 ⇒ 那具身体被判成「活人」了，去 `actor probe:` 行里按
				//         FormID / 距离找到它，看它的 `boolBits / formFlags / life`；
				//       M 不动而 `尸体=` 在涨 ⇒ 它进了候选，问题在判空那一路（看下面）。
				//     · 「搜空还亮」→ 看 搜空 那五个数：
				//         `empty=` 不涨 + `null=` 涨      ⇒ 库存指针是 null（引擎没建/已销毁）
				//         `empty=` 不涨 + `unknown=` 涨   ⇒ 形状不对（疑似标定选错偏移）
				//         `empty=` 不涨 + `notEmpty=` 涨  ⇒ 库存里真的还有引擎条目（不可见物品）
				{
					char invOff[16]{};
					if (g_invOff) {
						std::snprintf(invOff, sizeof(invOff), "+0x%zX", g_invOff);
					} else {
						std::snprintf(invOff, sizeof(invOff), "%s", "未标定");
					}
					// ★ v4.4：`kDead位` 一路在实测里恒为 0（引擎不给倒地者置位），所以
					//   新增 `life死=`（lifeState ∈ {1,2,5}）与 `life出血=`（∈ {7,8}）
					//   两路来源计数 —— 「打死的敌人亮没亮」看这两行就知道走通了没有。
					REX::INFO("  corpse (窗口内累加): 尸体={} (kDead位={} StartsDead标志={} life死={} life出血={} "
							  "道具={} 炮塔/机器人/昏迷={}) "
							  "| ACHR: 见到={} 判活跳过={} | StartsUnconscious跳过={} "
							  "| 前置过滤: deleted/disabled={} 非本cell={} "
							  "| 搜空: empty={} notEmpty={} unknown={} null={} shapeBad={} invOff={} "
							  "| 判空跳过: np={} eq={} | 展示柜跳过={}",
						g_state.corpseSeen, g_state.corpseByBit, g_state.corpseByFlag,
						g_state.corpseByLife, g_state.corpseByBleed,
						g_state.corpseProps, g_state.corpseUncSeen,
						g_state.achrSeen, g_state.achrLive,
						g_state.corpseUncSkipped,
						g_state.skipDeleted, g_state.skipParentCell,
						g_state.emptySkips, g_state.lootNotEmpty, g_state.lootUnknown,
						g_state.lootNullInv, g_state.lootBadShape, invOff,
						g_state.lootSkipNonPlayable, g_state.lootSkipEquipped,
						g_state.displayCaseSkips);
					g_state.corpseSeen       = 0;
					g_state.corpseByBit      = 0;
					g_state.corpseByFlag     = 0;
					g_state.corpseByLife     = 0;
					g_state.corpseByBleed    = 0;
					g_state.corpseProps      = 0;
					g_state.corpseUncSeen    = 0;
					g_state.corpseUncSkipped = 0;
					g_state.emptySkips       = 0;
					g_state.lootNotEmpty     = 0;
					g_state.lootUnknown      = 0;
					g_state.lootNullInv      = 0;
					g_state.lootBadShape     = 0;
					g_state.lootSkipNonPlayable = 0;
					g_state.lootSkipEquipped = 0;
					g_state.displayCaseSkips = 0;
					g_state.achrSeen         = 0;
					g_state.achrLive         = 0;
					g_state.skipDeleted      = 0;
					g_state.skipParentCell   = 0;
				}

				// 摘除计数（诊断）：rmOk 应随 sel/outline 变化一起增长，
				// unhMiss 长期增长 = 连引擎自己的摘除函数都没动到管理器里的东西。
				// ★ v2.3：graphRemove=1 才说明「3D 图 visitor」那条路可用（这是唯一真正
				//   有效的摘除路径）。mapCnt = 代表状态的管理器哈希表元素数，它应该
				//   跟着 outline 一起涨落 —— 只涨不落就说明还有摘不掉的残留。
				REX::INFO("  outline remove: rmOk={} unhMiss={} removeMiss={} removeReady={} unhighlightReady={} graphRemove={} mapCnt={}",
					g_state.outlineRemoved, g_state.outlineUnhighlightMiss,
					g_state.outlineRemoveMiss,
					g_state.nativeRemoveReady ? 1 : 0,
					g_outlineUnhighlightReady ? 1 : 0,
					g_outlineGraphRemoveReady ? 1 : 0,
					ManagerMapCount(OutlineManagerFor(PrimaryState())));

				// 性能窗口：每轮扫描耗时（max 才是「卡顿」的感觉来源）与单轮引擎调用数。
				REX::INFO("  timing: scan avg={}ms max={}ms ops={} deferred={} loading={}",
					g_state.scanMsSamples ? g_state.scanMsTotal / g_state.scanMsSamples : 0,
					g_state.scanMsMax,
					g_state.opsThisScan,
					g_state.opsDeferred,
					g_state.loadingNow ? 1 : 0);

				// ★ v2.3：把「32ms 到底花在哪」直接拆开打出来（数都是窗口内平均）。
				//   shape = 读 cell + 引用数组形状校验（vq = 真的问内核几次 VirtualQuery）
				//   loop  = 遍历全部引用 + 排序 + 挑选
				//   sync  = SyncNativeOutline（其中 unh=摘、add=挂）
				{
					const auto avg = [&](std::uint64_t a_sum) -> std::uint64_t {
						return g_state.scanMsSamples ? a_sum / g_state.scanMsSamples : 0;
					};
					REX::INFO("  timing2: shape avg={}ms max={}ms vq={}/scan | loop avg={}ms max={}ms | sync avg={}ms max={}ms (unh avg={}ms add avg={}ms)",
						avg(g_state.tShapeMs), g_state.tShapeMax,
						g_state.scanMsSamples ? (g_vqCalls - g_state.vqCalls) / g_state.scanMsSamples : 0,
						avg(g_state.tLoopMs), g_state.tLoopMax,
						avg(g_state.tSyncMs), g_state.tSyncMax,
						avg(g_state.tUnhMs), avg(g_state.tAddMs));
				}
				g_state.vqCalls       = g_vqCalls;
				g_state.scanMsTotal   = 0;
				g_state.scanMsMax     = 0;
				g_state.scanMsSamples = 0;
				g_state.tShapeMs      = 0;
				g_state.tShapeMax     = 0;
				g_state.tLoopMs       = 0;
				g_state.tLoopMax      = 0;
				g_state.tSyncMs       = 0;
				g_state.tSyncMax      = 0;
				g_state.tUnhMs        = 0;
				g_state.tAddMs        = 0;

				// ★ v3.1：原来这里打的是 Papyrus 侧自报的引导线状态
				//   （guideHb / guideState / guideMarkers），引导线功能已整体移除，
				//   对应的 GLOB 与这段日志也一起删掉。

				// base form 类型直方图（都只取 top 8）：
				//   candTypes —— 本轮**候选集合**（= 该亮的目标）的 formType 分布。
				//     ★ v4.1：用户报「某某东西不该亮」时，看这行就能定位它是哪种 formType。
				//   rejTypes  —— 窗口内「半径内但被过滤掉」的 formType 分布
				//     （不在白名单的 + 因为类别开关跳过的一起算）。
				//     用途：发现「某个东西该亮却没亮」时看它是不是被白名单/开关挡了。
				{
					const auto logTypeHist = [](const char* a_tag,
					                             std::array<std::uint32_t, 256>& a_hist) {
						std::vector<std::pair<std::uint32_t, std::uint8_t>> hist;
						for (std::uint32_t ft = 0; ft < a_hist.size(); ++ft) {
							if (a_hist[ft]) {
								hist.emplace_back(a_hist[ft], static_cast<std::uint8_t>(ft));
							}
						}
						if (!hist.empty()) {
							std::sort(hist.begin(), hist.end(),
								[](const auto& a, const auto& b) { return a.first > b.first; });

							std::string s;
							const auto  topN = std::min<std::size_t>(hist.size(), 8);
							for (std::size_t i = 0; i < topN; ++i) {
								if (i) {
									s += ", ";
								}
								s += "ft=" + std::to_string(static_cast<unsigned>(hist[i].second)) +
								     "x" + std::to_string(hist[i].first);
							}
							REX::INFO("  {}: {}", a_tag, s);
						}
						a_hist.fill(0);
					};
					logTypeHist("candTypes (本轮候选=该亮的目标，formType x 次数)", g_state.candTypes);
					logTypeHist("rejTypes (半径内但不在白名单/被类别开关跳过，formType x 次数)", g_state.rejectTypes);
				}

				// 管理器哈希表的家底（每 30 秒一次就够，用于确认「键到底是什么」）。
				if (now - g_state.lastMapDiagMs > 30000) {
					g_state.lastMapDiagMs = now;
					LogManagerMapDiag("runtime", PrimaryState());
				}
			}
		}
	}

	// ========================================================================
	// 安装
	// ========================================================================
	bool Install()
	{
		auto* task = SFSE::GetTaskInterface();
		if (!task) {
			return false;
		}
		g_mainThreadId.store(::GetCurrentThreadId());
		std::srand(static_cast<unsigned>(::GetTickCount()));

		LoadConfig();
		g_state.on = g_cfg.startEnabled;

		// 原生 outline 的几个引擎函数（RVA 是 1.16.244.0 实测值，会做签名校验）
		ResolveNativeOutline();
		if (!g_state.nativeReady) {
			REX::WARN("原生 outline 不可用（签名不匹配）-> 本次运行不会高亮。"
					  "按 docs/03 第七节重新核对 RVA 后再构建。");
		}

		// ★ v4.0：读档自愈不再需要任何 Papyrus / ESM 通道 —— 直接看载入画面
		//   （见 Tick 里的「载入画面由开变关」），另有「换 cell」与
		//   「HighlightManager 数量下跌」两路判据兜底。

		task->AddPermanentTask(Tick);

		REX::INFO("SAS_AlwaysScan installed (main thread {})", g_mainThreadId.load());
		return true;
	}
}
