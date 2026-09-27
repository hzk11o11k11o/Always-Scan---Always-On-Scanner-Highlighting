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

#include "RE/B/BSFixedString.h"
#include "RE/B/BSTEvent.h"
#include "RE/C/Calendar.h"  // ★ v5.1.7（订正 R7）：游戏时间（读档边界的时间指纹）
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
#include <cstring>
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

		// ================================================================
		// ★★ v4.10：展示柜「拿空即灭」—— 用「容器界面的同一段打开期」把
		//   「真拿空」与「关着读到的空」分开
		// ================================================================
		// 起因（用户实测，v4.9 如实记录的副作用被确认）：*「现在关着的武器箱会高亮
		//   了，但是拿空了却不会熄灭了」* —— v4.9 为了「关着也亮」用了最粗的规则
		//   （展示柜读到空一律不熄灭），于是「拿空后」也永远亮。
		//
		// ★ 难点（数据形态）：展示柜**关闭状态**下 `size=0` 与「刚被拿空」在库存里
		//   长得完全一样（内容只在搜刮界面打开期间投影进来，见上面 v4.9 段）。
		//   ⇒ 必须引入**独立于库存的第二个信号**：**搜刮界面是不是开着**。
		//
		// ★ 判据（游戏自带脚本实证）：容器界面的菜单名 = `"ContainerMenu"`
		//   —— `AudioContainerNoAnimScript.psc`（容器开/关音效）与
		//   `OutpostContainerScript.psc`（「打开容器就进 busy、关掉再结算」）都用
		//   `RegisterForMenuOpenCloseEvent("ContainerMenu")`；DLL 侧与 `MonocleMenu`
		//   同一套做法（`RE::UI::IsMenuOpen` 轮询，每帧一次）。
		//
		//   规则（每个展示柜引用一份状态，见 State::DisplayCaseVerdict）：
		//     · 打开期内读到 `kTemporary` 条目 ⇒ 记下「这一段打开期」的段号；
		//     · **同一段打开期**里读到「空」⇒ 内容被拿光 ⇒ 判空（熄灭）；
		//     · 关闭状态下读到「空」⇒ 内容未知 ⇒ 照常亮（v4.9 的目标，不回归）；
		//     · 一旦判空过 ⇒ 粘性：关掉后保持熄灭（= 用户要的「拿空即灭」）。
		//
		//   ★ 为什么用「UI 段号」而不是「时间窗」：关闭动作会让投影立刻收回
		//     （size→0，v4.9 日志实证），只看时间窗会把「关掉没拿」误判成「拿空」，
		//     而段号在每次「界面开 ↔ 关」翻转时 +1 ⇒ 「上一段打开期」的标记永远
		//     不可能等于「这一段」⇒ 两种情形被干净地分开。
		//   ★ 开销：除每帧一次 IsMenuOpen（与既有的 loading / monocle 检测同量级）
		//     外，热路径仍是纯内存读；任何一步形状不对 ⇒ 按「未知」⇒ 照常亮
		//     （最坏结果 = 退回 v4.9 行为，绝不会崩）。
		//   ★ 回退：INI `DisplayCaseUiEmpty=0` ⇒ 退回 v4.9 行为；菜单名可由
		//     `ContainerMenuName` 覆盖（万一是别的名字，不用换 DLL）。
		//   ★ 已知窗口（如实记录）：玩家在同一个扫描间隔（默认 200ms）内完成
		//     「拿空 + 关闭」⇒ 两头都观测不到「同一段打开期内读到的空」⇒ 漏判
		//     （仍亮）；下次打开该容器（若已被拿空 ⇒ 库存在打开期也是空，但那时
		//     `kt` 不会出现 ⇒ 依旧无法确认）。概率极低，且不会误灭（安全方向）。
		constexpr std::size_t kDisplayCaseVerdictMax = 512;  // per-ref 判决缓存上限（防呆）

		// ================================================================
		// ★★ v4.11：容器界面「开/关」改用**引擎自己的菜单事件**（v4.10 的轮询实测全程 ui=0）
		// ================================================================
		// 起因（用户 v4.10 实测）：*「武器箱拿空后还是不会熄灭」* —— 日志里同一个武器箱
		//   （ref=0033FCCA）的三条 `cont probe` 全是 `ui=0`（连"打开期间能看到投影条目
		//   `tp=1`"的那一条也是 `ui=0`）⇒ v4.10 的「同一段打开期」判据**从未生效**，
		//   统计行 `展示柜拿空=0` 一直不动，武器箱自然永远不熄灭。
		//   ⇒ 菜单名本身是对的（`Starfield.exe` 的菜单名表里有 `ContainerMenu`；
		//     `Starfield - Interface.ba2` 里有 `interface/containermenu.swf`），
		//     **锅在「轮询 `RE::UI::IsMenuOpen`」这条路读不到它**（对 LoadingMenu /
		//     FaderMenu 是有效的，载入检测一路正常）⇒ 换一条**引擎自己发**的路：
		//
		//   ★ 规则：`RE::UI` 本身就是 `BSTEventSource<MenuOpenCloseEvent>`（基类在
		//     `UI + 0x20`，见 commonlibsf `RE/U/UI.h` 的基类列表）——引擎每次
		//     「菜单开 / 关」都会往这个源上发一条事件（Papyrus 的
		//     `RegisterForMenuOpenCloseEvent` 收到的就是同一条）。我们注册一个 sink，
		//     事件里带着**菜单名**与**开/关**两个字段 ⇒ 既拿到准确状态、又拿到名字。
		//
		//   ★ 事件类型：`RE::MenuOpenCloseEvent` 定义在 `RE/E/Events.h`，而那个头
		//     单独 include 会因为缺类型编译不过（v4.0 已经踩过，见 docs/03）。
		//     它只有两个字段（`BSFixedString menuName; bool opening;`，sizeof 0x10），
		//     所以这里**本地定义一个同布局的类型**，再用 `BSTEventSource<T>`
		//     （布局与 T 无关：只有一个 sink 数组 + 两个计数）注册到 UI 的那个源上。
		//     `RegisterSink`/`UnregisterSink` 走的是引擎函数（REL::ID 123821/123822，
		//     已用 versionlib 核对过），引擎侧的 `NotifyVisitor` 只按 vtable 第 1 槽
		//     回调 `ProcessEvent` ⇒ 同布局成立即可。
		//
		//   ★ 兜底（事件通道没生效时不能瞎）：`MenuDump` 用**候选名 + `IsMenuOpen`**
		//     把「此刻开着的菜单」摊开写日志（纯引擎查询、零内存猜测，未注册的名字
		//     只会返回 false）—— 万一菜单名不是 `ContainerMenu`，一次日志就能看出来。
		//   ★ 自愈：看到展示柜投影（= 搜刮界面确实开着）而我们的标志还是 false 时，
		//     自动把「此刻唯一开着的、非白名单」的菜单名学成容器菜单名（会话级），
		//     这样即使原版/第三方改了菜单名，同一局里也能自动恢复。
		constexpr std::size_t kOffUiMenuEventSource  = 0x20;  // RE::UI 的 MenuOpenCloseEvent 源
		// ================================================================
		// ★★ v4.12：vtable 期望值订正 —— 上一轮那条「不注册」WARN 的**真根因**
		// ================================================================
		// v4.11 实测日志（用户 13:20~13:22）：
		//   `menu events: UI+0x20 的 vtable=0x7FF65860E3D8，期望 0x7FF65955ADD0
		//    （BSTEventSource<MenuOpenCloseEvent>，RVA 0x5CCADD0）-> **不注册**`
		// ⇒ 期望值算错了：**0x5CCADD0 根本不是 vtable，而是另一个类型的 TypeDescriptor
		//   （RTTI 类型描述符）** —— 于是 sink 每 4 秒重试一次、一次都没挂上，
		//   容器界面信号全程退回「轮询」（而对 `ContainerMenu` 轮询本来就读不到），
		//   展示柜自然永远不熄灭。
		//
		// 离线复核（本轮新增 `out/probe_menu_evt.py` / `out/probe_menu_evt2.py` /
		//   `out/dis_span.py` / `out/find_menu_notify.py`，四条线互相印证）：
		//   ① RVA 0x5CCADD0 处 `+0x10` 的字符串 =
		//      `.?AU?$BSTSDMTraits@VUI@@U?$BSTSingletonSDMOpStaticBuffer@VUI@@@@@@`
		//      ⇒ 那是「UI 单例 traits」的 RTTI，跟菜单事件毫无关系；
		//   ② exe 里 `BSTEventSource<MenuOpenCloseEvent>` 的 RTTI 名字字符串在
		//      RVA 0x5CCAF78 ⇒ TypeDescriptor = 0x5CCAF68（MSVC：名字在 TD+0x10）；
		//   ③ ★ 定死的证据 = **UI 构造函数里那串 vtable 写入**（RVA 0x253E2BD 区段）：
		//         mov [rcx+0x00], <vtable 0x4D7E3F8>
		//         mov [rcx+0x10], <vtable 0x4D7E408>   ← BSInputEventReceiver（vtable 有 2 槽）
		//         mov [rcx+0x20], <vtable 0x4D7E3D8>   ★ 就是它
		//         mov [rcx+0x48], <vtable 0x4D7E3E8>
		//         mov [rcx+0x70], <vtable 0x4D7E3B8>
		//         mov [rcx+0x98], <vtable 0x4D7E3C8>
		//         mov [rcx+0xC0], <vtable 0x4D7E450>
		//         mov [rcx+0xE8], <vtable 0x4D7E460>
		//         mov [rcx+0x110], <vtable 0x4D7E430>
		//         mov [rcx+0x138], <vtable 0x4D7E440>
		//         mov [rcx+0x160], <vtable 0x4D7E420>
		//     这 11 个偏移与 commonlibsf `RE/U/UI.h` 的基类表**逐条对上**
		//     （0x00 BSTSingletonSDM / 0x10 BSInputEventReceiver /
		//      0x20 **BSTEventSource<MenuOpenCloseEvent>** / 0x48 MenuModeChange /
		//      0x70 MenuPauseChange / 0x98 MenuPauseCounterChange / 0xC0 Tutorial /
		//      0xE8 BSCursorTypeChange / 0x110 BSCursorRotationChange /
		//      0x138 BIUIMenuVisiblePausedBegin / 0x160 BIUIMenuVisiblePausedEnd）
		//     ⇒ **commonlibsf 的 UI 布局是对的**（这次错的是我们算的 RVA，不是头文件）；
		//   ④ 自洽校验：0x4D7E3D8 的首槽（RVA 0x25475BC）正是
		//      `sub rcx, 0x20; jmp …` —— MSVC 多继承里「子对象位于完整对象 +0x20」的
		//      标准析构 thunk；同组的 0x4D7E440 首槽是 `sub rcx, 0x138`，
		//      与 ctor 里写 `[rcx+0x138]` 的那一条严格吻合 ⇒ 整张表自洽；
		//   ⑤ 实测值反推：0x7FF65860E3D8 − 模块基址 0x7FF653890000 = 0x4D7E3D8 ✓。
		//
		// ⇒ 订正为 **0x4D7E3D8**。注册前仍然做 vtable 硬比对（不是它就绝不注册，
		//   退回轮询并打一条「UI 布局指纹」便于下次定位）。
		// ★ 教训（写进 docs/99 踩坑 12）：**RTTI 的 REL::ID 查出来的是 TypeDescriptor
		//   （类型描述符），不是 vtable** —— 要 vtable 必须走
		//   「COL → TypeDescriptor」链，或直接找**构造函数里写这个指针的那条指令**。
		constexpr std::uintptr_t kMenuOpenCloseSourceVtblRva = 0x4D7E3D8;
		constexpr std::size_t kMenuEvtOpenListMax    = 8;     // 事件侧「当前开着的菜单」上限
		constexpr std::size_t kMenuEvtNameMax        = 32;    // 菜单名长度上限（BSFixedString 实际更短）
		constexpr std::size_t kMenuEventLogMaxDefault = 48;   // 普通菜单事件最多写多少条日志
		constexpr std::size_t kMenuLearnMax          = 4;     // 会话级最多学习几个容器菜单名
		constexpr std::uint64_t kMenuLearnWindowMs   = 2500;  // 「最近一次的打开事件」有效窗口
		// ★ v4.12：事件日志按「每个菜单名前几条」记（长会话里也一定能看到「都是哪些菜单在开关」；
		//   上一轮是全局 48 条，玩久了额度就烧光，正是这轮取证最缺的信息）。
		constexpr std::size_t   kMenuEvtNameStatMax   = 16;   // 「按名字统计」的表大小
		constexpr std::uint32_t kMenuEvtPerNameLogMax = 3;    // 每个名字最多记几条
		constexpr std::size_t   kMenuEvtRecentMax     = 4;    // 「最近几条事件」环形（取证行用）
		constexpr std::size_t   kMenuEvtSrcScanMax    = 0x200; // UI+0x20 对不上时往后扫多远找源
		constexpr std::uint64_t kMenuSinkCheckMs      = 60000; // 每 60 秒核对一次「sink 还在不在」
		// ★★★ v5.1.5（订正 R5）：读档事件 sink 的「还在不在」核对周期（同 menu / loot 通道）
		constexpr std::uint64_t kLoadSinkCheckMs      = 60000;
		constexpr std::size_t kDcWatchMax            = 12;    // 逐帧观察的展示柜引用上限
		// ★ 逐帧观察表的淘汰阈值：连续这么多帧读不到该引用就踢出去。
		//   取值偏小（≈0.13 秒 @60fps）—— 因为 `IsReadable` 失败那一路会走
		//   `VirtualQuery`（毫秒级），不能让它每帧对着一具已经销毁的引用反复问内核。
		constexpr std::uint32_t kDcWatchMissMax      = 8;
		constexpr float kDcWatchRadiusMeters         = 20.0f;  // 只逐帧观察这么近的展示柜
		constexpr std::size_t kDisplayCaseTraceMaxDefault = 24;  // 投影出现/消失追踪日志上限
		constexpr std::size_t kMenuDumpMax           = 6;     // 「此刻开着的菜单」快照最多打几次
		constexpr std::uint64_t kMenuEvtBadMax       = 5;     // 事件负载「不像菜单事件」到几条就注销通道

		// ================================================================
		// ★★ v4.13：搜刮界面**根本不是 UI 菜单** ⇒ 改用两条「引擎自己的游戏事件」
		// ================================================================
		// ★ 取证（v4.12 那一局的日志，本轮逐行核对）：
		//   · 投影出现（= 搜刮面板开着）的那一刻**没有任何 ContainerMenu 事件**
		//     （统计行 `menu events: total=18 container=0`；那一局的 18 条事件全被
		//      Fader / Loading / Main / Cursor / Data / Pause / HUD / HUDMessages 占满）；
		//   · `menu dump` 的 `IsMenuOpen` 快照里只有 `[HUDMenu, HUDMessagesMenu]`。
		//   ⇒ 结论：Starfield 的「快速搜刮面板」**不是 UI 菜单**（不产生
		//     `MenuOpenCloseEvent`）—— v4.10 的轮询与 v4.11/v4.12 的事件
		//     **两条路拿不到它**，不是常量算错，是「面板 = 菜单」这个**前提不成立**。
		//
		// ★ 离线取证（本轮新增两个可复用工具，都在 `out/`）：
		//   `out/find_event_sources.py`（RTTI 名字 → TD → COL → vtable → 静态对象）
		//   与 `out/scan_event_sources.py`（反向：扫 .data 里「首字段 = vtable」的对象
		//   → vtable → RTTI 名字；**全镜像 861 个 `BSTEventSource<X>` 里 229 个是静态对象**）。
		//   其中这两个正是我们要的：
		//
		//   ① `BSTEventSource<TESContainerChangedEvent>` @ RVA **0x5977BA8**
		//      （vtable RVA **0x4B98240**）—— 「**物品在容器之间移动**」事件，
		//      就是 Papyrus `OnItemAdded` / `OnItemRemoved` 背后那一个
		//      （游戏自带 `OutpostContainerScript.psc` 就是靠它做前哨容器的联动结算）。
		//      负载 = `{ source, target, baseObject, itemCount, itemRef, uniqueID… }`
		//      （同 commonlibsf `RE/E/Events.h`，sizeof 0x28）。
		//      ⇒ 玩家从箱子里拿走东西**必然**经过它 ⇒ 可以**精确记账**。
		//
		//   ② `BSTEventSource<QuickContainerOpenedEvent>` @ RVA **0x5978A30**
		//      （vtable RVA **0x4B985B0**）—— 「**快速搜刮面板打开**」事件。
		//      ★ 负载 = **一个指向容器引用的指针**（反汇编实证：发事件的函数在
		//        RVA 0x1480390 区段，取到引用后先 `cmp byte ptr [rcx+0x2E], 0x4B`
		//        （容器 formType）判是容器，再 AddRef 放进负载、`Notify` 出去）
		//        ⇒ 它把「面板此刻为**哪个**箱子开着」直接告诉我们（per-ref！）。
		//
		// ★ 判决（分层，实现在 `DisplayCaseReadsEmpty`）：
		//   ① **记账（精确）**：逐帧/每轮读到投影时把内容记成 `base -> count` 快照
		//      （`DcSnapOut`）；收到「从这个 ref 拿走 X×N」事件就减账，
		//      **减到 0 ⇒ 判拿空**（粘性熄灭）。拿空后立刻关掉、甚至同一个扫描间隔
		//      内关掉都不影响 —— 这正是 v4.10 那个「已知窗口」缺的东西。
		//      ★ 实测（2026-09-19 14:25:07，武器箱 ref=0033FCCA）：两条 take
		//        （枪 ×1、弹药 ×8）精确减账 8→0 ⇒ `记账判空=1` ⇒ 熄灭并保持。
		//   ② **兜底**（INI `DisplayCaseQuickOpenEmpty`，★ v4.14 起**默认关**）：
		//      **只有在①从没生效过**（这一段打开期里一次拿走事件都没收到）时才启用：
		//      「快速面板为这个 ref 开着」+「这一段里读到过内容（v4.14 加严）」+
		//      「连续读到空 ≥ 400ms」⇒ 判拿空。
		//      ★★ v4.14 默认关的两个理由（同一局日志 + 推导，两条都实证/可推导）：
		//        a) **误灭面 ①「打开瞬间」**：面板刚打开、投影还没建立的那 0.4~0.7 秒
		//           （展示柜关闭时库存本来就是空的）会被当成「连续读到空」⇒ 误判。
		//           实证：14:25:05.812 快速面板事件 → 14:25:06.222 判「拿空」，
		//           可 14:25:06.463 投影出现、箱子里明明有 2 件东西。
		//           ⇒ v4.14 用 `sawContentInSession`（这一段里见过内容）把 a) 挡掉。
		//        b) **误灭面 ②「打开不拿就关掉」**：关掉后投影消失、库存又读空，
		//           而「面板关闭」**没有任何信号**（拿空与关闭在数据上完全同形 ——
		//           v4.9 就论证过）⇒ 400ms 后必然误判 ⇒ 违反验收点「打开不拿就
		//           关掉 ⇒ 保持亮」。这一类**无法修**（信息不可分）⇒ 默认关掉兜底。
		//      ★ 为什么敢关：真实拿空走记账（①）—— 展示柜触发物品事件已有实测；
		//        「不走物品事件的展示柜」至今没有任何实测证据。项目红线是
		//        「宁可少灭一次，绝不误灭」（v4.9/v4.12 两轮验收都依赖这条）。
		//   ③ 两条都不成立 ⇒ 退回 v4.9/v4.12 行为（照常亮），绝不乱灭。
		//
		// ★ 安全姿势与 v4.11/v4.12 完全相同：注册前**硬核对 vtable**（不是那个
		//   类型就绝不注册）、BSTEventSource 形状校验、负载全部 `SafeReadMem` 读、
		//   回调里只往队列塞一条记录（判决在主线程做）。
		constexpr std::uintptr_t kInvEvtSourceRva     = 0x5977BA8;   // TESContainerChangedEvent 静态源
		constexpr std::uintptr_t kInvEvtSourceVtblRva = 0x4B98240;   // 它的 vtable（注册前硬核对）
		constexpr std::uintptr_t kQuickOpenSourceRva     = 0x5978A30;  // QuickContainerOpenedEvent 静态源
		constexpr std::uintptr_t kQuickOpenSourceVtblRva = 0x4B985B0;  // 它的 vtable
		constexpr std::size_t   kLootEvtQueueMax      = 64;     // 事件→主线程队列上限
		constexpr std::size_t   kLootEvtPerRefLogMax  = 3;      // 每个 ref 最多记几条事件日志
		constexpr std::size_t   kDcSnapMax            = 8;      // 展示柜内容快照的条目上限
		constexpr std::uint32_t kDcSnapStackBudget    = 96;     // 快照最多走多少个 stack（防呆）
		constexpr std::uint64_t kDcEmptyConfirmMs     = 400;    // 「连续读到空」多久算确认（兜底判据用）
		constexpr std::uint64_t kQuickOpenSessionMs   = 30000;  // 「快速面板打开」会话的有效期
		constexpr std::uint64_t kLootEvtSinkCheckMs   = 60000;  // 每 60 秒核对 sink 还在不在

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

		// ★★★ v5.1.6（订正 R6）：读档候选的**复核延后** —— 事件只记「待复核」，
		//   等「载入画面关闭 + 这么久（且没有新的载入）」世界稳定后再跑资源链复核。
		//   为什么：v5.1.5 在「载入画面关闭」那一瞬间就复核，实测在传送 / 换世界空间时
		//   拿到**假证伪**（勘测数据 / 上下文还没就绪）⇒ 把真绿误作废（用户报告的
		//   「传送后已扫描物品变青」）。定义放这里是因为启动日志也要引它。
		constexpr std::uint64_t kFloraScopeEvalDelayMs = 4000;

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

		// ================================================================
		// ★★★ 2026-09-27（订正 R3）：环内 cell 的**分片遍历** —— 治「帧数下降」
		// ================================================================
		// 起因（用户报告 + 当轮日志）：
		//   `timing: scan avg=79ms`（每 200ms 一轮 ⇒ 主线程占用 ~40%）；
		//   拆开后 = `loop avg=55ms`（大头）+ `sync avg=14ms` + shape ~0ms。
		//   而 loop 里只有「遍历引用 + 分类 + 距离」（纯内存读，单条 ~1µs，cache miss 主导）
		//   ⇒ 唯一能解释 55ms 的就是**遍历量**：当前 cell + 环内 cell 合计可达
		//   `kRingRefsCap = 60000` 个引用（外景大 cell 每个几万引用），6 万 × ~0.9µs ≈ 54ms。
		// 治法：
		//   每个环内 cell 的引用数组**均分成 ≤ N 片**（默认 N = 5），每轮只遍历**一片**、
		//   游标轮转 ⇒ 每轮环内遍历量 ≈ 环总量 / N（默认 6 万/5 ≈ 1.2 万 ⇒ ~10ms）；
		//   当前 cell（玩家所在）**不分片**，每轮全扫（视野里的目标必须即时响应）。
		// 观感安全（两条都靠配置钳制，见 LoadConfig）：
		//   ① **分片周期（N × ScanIntervalMs）必须 < UnhighlightGraceMs** ——
		//      否则「本轮没被扫到」的目标会在宽限期到期时被 SyncNativeOutline 摘掉、
		//      下一片再挂回来（闪烁）。默认 5 × 200ms = 1000ms < 1500ms ✓；
		//      有效片数 eff = min(用户值, (grace - interval) / interval)，运行时钳死。
		//   ② 小 cell（引用数 ≤ kRingSliceMinRefs）不分片 —— 单 cell / 小场景行为与旧版完全一致。
		// 回退：INI `RingSliceMaxRounds=0`（= 1 = 每轮全扫，退回旧行为）。
		constexpr std::uint32_t kRingSliceMaxRounds = 5;
		constexpr std::uint32_t kRingSliceMinRefs   = 2000;

		// ================================================================
		// ★★★ 2026-09-27 深夜（订正 R4）：把热路径上的**内核调用**全部拿掉 ——
		//   治用户报告的「**静态场景帧数正常，动态场景（战斗 / 走动时扫到新的高亮
		//   物品）卡顿**」。
		// ================================================================
		// 起因（同一局日志，2026-09-27 08:32~08:48）：
		//   · 08:37 静止：cur+ring = 1538 个引用 ⇒ `loop avg=10ms`、`walk=10ms`；
		//   · 08:47 走动：cur+ring = 1545 个引用 ⇒ `loop avg=63ms`、`walk=63ms`。
		//   遍历量 / 候选数 / 判空次数**几乎一模一样**，墙钟差 6 倍 ⇒ R3 的假设
		//   （「耗时 = 遍历量 × 常数」）**被自己的日志证伪**：差的不是「做了多少事」，
		//   而是「每件事多贵」。
		//   进一步看：同一窗口里 `sync`（引擎调用）两边都是 7ms、`3D复检` 都是
		//   700~800 次 —— 说明**引擎调用没变慢**，变慢的只有 loop 里那一段。
		//   loop 里唯一会「进制内核」的东西是：
		//     · `SafeReadMem` → `ReadProcessMemory`（容器 / 尸体判空的库存链，
		//        一轮几百次）；`IsReadable` → `VirtualQuery`（形状校验 / 探针）。
		//   两者都要拿**进程地址空间锁**；游戏流式加载（走动 / 战斗 = 大量
		//   commit / decommit / remap）时那把锁被抢，单次调用从 ~10µs 涨到几百 µs
		//   （按日志反推 64ms ÷ ~170 次）⇒ 每轮多出几十毫秒 = 可见卡顿。
		// 治法（两条都默认开、都能一行 INI 回退）：
		//   ① `FastReadMem=1`：安全读改成 **SEH 兜底 + 直接读**（正常内存零内核调用）；
		//      可读性校验改成**逐页 1 字节直读探针**（页粒度 = 保护的粒度）。
		//   ② `LootCacheTtlMs=1500`：容器 / 尸体判空**按引用缓存**，失效靠事件
		//      （拿 / 放物品、容器界面开关）⇒ 一轮的库存链调用从 30+ 次降到个位数。
		// 另外把计时精度从 ms 换成 µs（`NowUs`）：原来每次 `NowMs() - t0` 取整，
		//   单次几百 µs 的判空 / 分类全被抹成 0 —— R3 的 `loot=0ms flora=2ms`
		//   就是这么来的（**假象**：真实开销躲在 walk 里看不见）。
		constexpr std::size_t kLootMemoMax = 512;  // 判空缓存的条目上限（超了整体清空）
		// 「遍历内部空档」的阈值（µs）：把「我们的指令慢」与「线程被抢 / 等内存」
		//   分开。每 64 个引用取一次时刻，间隔 ≥ 这个值就记一次 `卡顿`。
		constexpr std::uint64_t kStallThresholdUs = 3000;

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

		// ★★★ v4.29：放下扫描仪后的「恢复提速」窗口 —— 治用户实测的
		//   「**植物和矿石的高亮速度明显低于其他物品**」。
		//   背景（v4.28 那一局日志 + 代码核对）：
		//     ① 每次放下扫描仪，引擎都会拆 Monocle HUD（0x17D4B30：销毁 11 个
		//        HighlightManager + 清表）⇒ `DetectEngineOutlineLoss` 必须把
		//        **全部已挂目标**（那一局 268 个）转入「待重申」；
		//     ② 重挂按 `MaxOutlineOpsPerScan`（默认 64）条/轮慢慢做 ⇒ 268 个要
		//        5 轮 ≈ **1.05 秒**；而「扫完变绿」的状态变化（青 → 绿）也排在
		//        这条队列里按距离消耗同一份预算 ⇒ 用户感知「植物 / 矿石慢一拍」。
		//   做法：resync（放下扫描仪 / 管理器数量下跌）之后的 `kResyncBoostMs`
		//   毫秒内，把每轮预算提到 `kResyncBoostBudget`（宁快勿慢 —— 这段时间
		//   玩家刚放下扫描仪，正盯着屏幕看高亮回来），并且把「状态变化」的条目
		//   排到普通重挂之前（见 SyncNativeOutline 的 2.5 步）。
		//   INI 可回退：`ResyncBoostMs=0` 或 `ResyncBoostBudget=0` = 退回旧行为。
		constexpr std::uint64_t kResyncBoostMs     = 2500;
		constexpr std::uint32_t kResyncBoostBudget = 192;

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
		// 分类只依赖 base form 的类型（`RE::TESForm::GetFormType()`），零成本；
		// ★ v4.17 起 MISC 还要多看一眼「物品记录上的关键词」（是不是资源，见
		//   IsResourceBase()）。仍然只有纯内存读。
		// 下标顺序 = INI 里 StateLoot / StateWeapon / ... 的顺序。
		// ====================================================================
		enum class Category : std::uint8_t
		{
			// 杂项 = 其余可拾取物：MISC（**不含资源**）/ KEYM / INGR / SLGM …
			//   ★ v4.17：用户需求原文「**杂项不变，还是原本的颜色**」⇒ 这一类
			//     继续用引擎 state 2 的蓝（INI `StateLoot=2`，键名保持不动）。
			kLoot = 0,
			// ================================================================
			// ★★ v4.17（需求 1.6）：按「物品栏分类」把原来那一大坨 kLoot 拆成 6 组
			// ----------------------------------------------------------------
			// 用户需求（1.6新需求.md）：
			//   武器、投掷物 ／ 太空服、背包、头盔、服饰 ／ 弹药、救援 ／ 笔记 ／ 资源
			//   各给一种颜色；**杂项不变**。
			// 判据全部**离线取证**（Starfield.esm 1.16.244.0，见 docs/16 与
			// out/esm_invcat_probe.py / out/esm_resource_probe.py）：
			//   · 投掷物（`FragGrenade` / `FragMine` / `ShrapnelGrenade` …）在数据里
			//     就是 **WEAP**（带 `InventoryCategoryWeaponThrowable` 关键词）；
			//   · 太空服 / 背包 / 头盔 / 服饰 全是 **ARMO**
			//     （关键词 `InventoryCategoryArmorSuit/ArmorBackpack/ArmorHelmet/Apparel`）；
			//   · 救援 = **ALCH**（含食品 / 饮料），弹药 = **AMMO**；
			//   · 笔记 = **BOOK**（备注 / 数据板 / 杂志 / 书 —— Starfield.esm 里
			//     **没有** NOTE 记录，所以 kNOTE 一并归到这一类）；
			//   · 资源 = **MISC + `ResourceType*` 关键词**（410 条；而 Digipick /
			//     Credits / 毛绒玩具 / 盆栽这类**真杂物**没有该关键词）⇒ IsResourceBase()。
			// ================================================================
			kLootWeapon,    // 武器、投掷物：WEAP
			kLootApparel,   // 太空服、背包、头盔、服饰：ARMO
			kLootAmmoAid,   // 弹药、救援：AMMO + ALCH
			kLootNote,      // 笔记：BOOK（+ NOTE）
			kLootResource,  // 资源：MISC + ResourceType* 关键词
			kContainer,     // 容器：CONT
			kDevice,        // 可交互设备：ACTI / TERM
			kDoor,          // 门：DOOR
			kFlora,         // 植物：FLOR
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
			"loot", "weapon", "apparel", "ammoaid", "note", "resource",
			"container", "device", "door", "flora", "other", "corpse"
		};

		// ================================================================
		// ★★ v4.17：「资源」判据 —— MISC 记录上的 `ResourceType*` 关键词
		// ----------------------------------------------------------------
		// 离线取证（Starfield.esm 1.16.244.0；脚本 `out/esm_resource_probe.py`，
		// 完整结论见 `docs/16-物品栏分类分色.md`）：
		//   · MISC 记录共 **1319 条**：**410 条**带 `ResourceType*` 关键词 = 资源物品
		//     （`InorgCommonIron` / `InorgExoticNeon` / `OrganicAdhesive` /
		//      `Manufactured*` …），**909 条**不带 = 真杂物（Digipick / Credits /
		//      毛绒玩具 / 雪景球 / 纸巾盒 / 盆栽 / 桌面风扇 …）；
		//   · `ResourceType*` 关键词共 **28 个**，全部在 Starfield.esm（下表）；
		//   · 「只带 `ResourceRarity*`、不带 `ResourceType*`」的 MISC = **0 条**
		//     ⇒ 这条判据不漏（脚本里专门统计过）。
		// 读法（commonlibsf 声明 + 本项目「形状校验优先」的铁律）：
		//   `class TESObjectMISC : …, public BGSKeywordForm  // 0x1E8`
		//   `BGSKeywordForm::keywords（BSTArray<BGSKeyword*>）` 在基类 +0x20
		//   ⇒ **base + 0x208** 处是 BSTArray 头。
		//   ★★★ v4.23 **订正布局**（旧注释在这里写了「data 在前、size/capacity 在后，
		//     与常量区 `kOffArray*` 那一套不同，这里不套用」—— 那句话是错的，而且
		//     正是「资源判据全程静默失效」的根因）：commonlibsf `BSTArray.h` 里
		//       `BSTArrayBase{ _size@+0, _capacity@+4 }`（第 52 行）
		//       `BSTArray : BSTArrayBase, Allocator { …; void* _data; }`（第 136/380 行）
		//     ⇒ 布局 = **`{ _size@+0x00, _capacity@+0x04, _data@+0x08 }`**，与常量区
		//       那一套**完全相同**。旧读法把 `data` 读成 `size|capacity<<32`（小数）
		//       ⇒ 每个 MISC 都判 false ⇒ 资源永远归杂项。完整证据链见 docs/22 §一。
		//   ★ 校验链：1 ≤ size ≤ 64 ∧ size ≤ capacity ≤ 4096 ∧ data 可读 ∧
		//     每个元素是指向 **KYWD** 的指针。任何一环不过 ⇒ 返回 false（= 按杂项处理）。
		//     失败方向永远是安全的那一边：宁可少一个颜色，绝不把垃圾内存当资源。
		//   ★ 偏移自适应：候选表逐个试，第一个「形状合格且非空」的偏移被采纳并缓存
		//     （`resource keyword: 关键词数组标定 = …` 一行 INFO 可核对）；全不合格
		//     时打 `misc kw probe:`（≤6 条）把原始读数摆出来。
		//   ★ 启动自检（`ResourceKeywordSelfTest`）：用「已知答案」的原版记录验一次
		//     （资源正样本必须命中 / Digipick、Credits 必须不命中），只在拿到明确
		//     反证时才整体退回「全部 MISC = 杂项」。
		// ================================================================
		// ★★★ v4.23：`TESObjectMISC` 的 `BGSKeywordForm::keywords` 偏移**不写死一个** ——
		//   首选 0x208（commonlibsf：`TESObjectMISC` 的 `BGSKeywordForm` 基类 @+0x1E8
		//   + `BGSKeywordForm::keywords` @+0x20），其余是「偏移万一又整体漂移」时的
		//   自适应备份（形状校验全过才采纳；见 IsResourceBaseRaw / ScanKeywordsAt）。
		constexpr std::size_t   kMiscKwOffCandidates[] = { 0x208, 0x200, 0x1F8, 0x210, 0x218 };
		constexpr std::uint32_t kMiscKeywordMax   = 64;     // 防呆上限（资源物品实测最多 4 个）
		constexpr std::uint32_t kMiscKeywordCapMax = 4096;  // capacity 合理性上限
		constexpr std::uint32_t kMiscKwProbeMax   = 6;      // `misc kw probe:` 每会话最多几条
		constexpr std::uint32_t kResKeywordDigipick = 0x0000000A; // Digipick（自检：杂项）
		constexpr std::uint32_t kResKeywordCredits = 0x0000000F;  // Credits（自检：杂项）
		// ★★ v4.22：自检的**正样本池**（资源 MISC 物品；任一「拿得到且命中」即算通过）。
		//   为什么不是一个样本：**实测 0x5556E（InorgCommonIron）整局 `LookupByID` 都是
		//   null**（用户那一局 121 次尝试全是 `iron=0`，而 Digipick / Credits 一次就拿到；
		//   离线 ESM 里 0x5556E 确实是 `InorgCommonIron` + `ResourceTypeSolid`）。
		//   旧代码「单样本 + 拿不到就不生效」⇒ 「资源」被永久冻在「杂项」上，正是用户
		//   实测反馈的「资源和杂物无法区分（都是原版扫描仪的蓝色）」（见 docs/21）。
		//   FormID 全部离线核对过（`out/esm_resnode_probe.py`，都带 ResourceType* 关键词）：
		constexpr std::uint32_t kResSelfTestSamples[] = {
			0x0005556E,  // InorgCommonIron
			0x00055568,  // InorgCommonLead
			0x00055572,  // InorgCommonNickel
			0x0005556B,  // InorgUncommonTungsten
			0x0005556D,  // InorgRareTitanium
			0x00055573,  // InorgRarePlatinum
			0x00055570,  // InorgUncommonAlkanes（Gas）
		};
		// 28 个 ResourceType* 关键词的 FormID（全部在 Starfield.esm = 无需 load order 前缀）
		constexpr std::uint32_t kResourceKeywords[] = {
			0x0003F001,  // ResourceTypeCraftingGeneric
			0x0016B569,  // ResourceTypeCraftingInorganicCommon
			0x0004248E,  // ResourceTypeCraftingInorganicExotic
			0x0004248D,  // ResourceTypeCraftingInorganicRare
			0x0004248C,  // ResourceTypeCraftingInorganicUncommon
			0x0004248F,  // ResourceTypeCraftingInorganicUnique
			0x000424A5,  // ResourceTypeCraftingMfgCommon
			0x000424A6,  // ResourceTypeCraftingMfgExotic
			0x000424A7,  // ResourceTypeCraftingMfgRare
			0x000424A8,  // ResourceTypeCraftingMfgUncommon
			0x000424A9,  // ResourceTypeCraftingMfgUnique
			0x00042490,  // ResourceTypeCraftingOrganicCommon
			0x000424A1,  // ResourceTypeCraftingOrganicExotic
			0x000424A2,  // ResourceTypeCraftingOrganicRare
			0x000424A3,  // ResourceTypeCraftingOrganicUncommon
			0x000424A4,  // ResourceTypeCraftingOrganicUnique
			0x0006FDB3,  // ResourceTypeFauna
			0x0006FDB2,  // ResourceTypeFlora
			0x00299548,  // ResourceTypeGas
			0x0029954A,  // ResourceTypeLiquid
			0x0023CEB6,  // ResourceTypeManufactured
			0x00021377,  // ResourceTypeManufacturedCommon
			0x00021387,  // ResourceTypeManufacturedExotic
			0x0002137B,  // ResourceTypeManufacturedRare
			0x00021378,  // ResourceTypeManufacturedUncommon
			0x0002139B,  // ResourceTypeManufacturedUnique
			0x002C5A95,  // ResourceTypeOrganic
			0x00299549,  // ResourceTypeSolid
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

			// --- ★★★ v4.23：举着原版扫描仪时，**星球扫描目标类别让位原版** ---
			// 1 = 只要 `MonocleMenu` 开着（= 玩家举着手持扫描仪），**「植物」类别**
			//     （FLOR = 星球上的植物 / 矿脉 / 气泉 / 液池，MOD 用 state 7 挂它们）
			//     就不参与本轮的挂 / 摘 / 重申 —— 既不写新状态，也不摘掉已挂的。
			//   ★ 为什么需要（用户实测反馈，见 docs/22）：
			//     原版扫描仪在星球上给「可扫描目标」上色走的是引擎自己的逐引用求值
			//     （RVA 0x159ED90 → 写 state 7/8/9/10，写完立刻被渲染消费）。
			//     MOD 每 200ms 一轮重申自己的类别状态，就会把引擎刚写上的
			//     「扫描前 / 扫描后 / 正在扫描」盖掉 —— 用户看到的就是
			//     「扫描前和扫描后颜色无法区分了」。
			//   ★ 为什么只让「植物」这一类让位、而不是整段停摆：
			//     ① 只有 state 7/8（以及被引擎临时写上的 9/10）是原版扫描目标槽位，
			//        MOD 其它类别（武器 / 服饰 / 弹药 / 笔记 / 资源 / 容器 / 尸体 /
			//        设备 / 门）用的是别处，重申它们**不会**影响原版；
			//     ② 引擎在扫描仪 HUD 建立时会销毁管理器 + 清空状态表
			//        （RVA 0x17D4B30，见 v3.2），整段停摆就等于举着扫描仪时
			//        **「屏幕中央圆圈之外也高亮」这个卖点也没了** ⇒ 只让该让的让。
			//   ⇒ 让位期间：FLOR 不挂新、不重申、也不摘（`SyncNativeOutline` 里对
			//     这类条目**不排摘除**，避免把引擎自己挂的那条 highlight 也摘掉）；
			//     放下扫描仪那一刻，v3.2 的 `MarkAllForReassert` 会把它们整批铺回来。
			//   0 = 退回 v4.22 行为（举着扫描仪也按 state 7 重申）；改完重进游戏生效。
			bool          yieldTargetsWhileScanning = true;

			// ================================================================
			// ★★★ v4.25：星球目标「已扫描」⇒ 也用原版那个绿（放下扫描仪之后）
			// ================================================================
			// 起因（用户实测反馈）：
			//   「矿石、气体、液体、植物、动物现在扫描后，在扫描仪里的颜色是原版颜色了，
			//     但是收起扫描以后，在 MOD 的高亮颜色里还是扫描前的原版蓝色，
			//     这个也要跟着一起变成原版绿色」
			// v4.24 修好的是「举着扫描仪时」那一半（state 4/5 归还引擎）；这一项修的是
			// 「放下扫描仪之后」那一半 —— MOD 自己重挂时也得区分扫没扫过。
			//
			// 1 = 用**引擎自己的判据**（RVA 0x1597A50「这个资源已扫描（进了勘测数据）？」，
			//     走 FLOR → produceItem → MISC → BGSResource 那条链，见常量区证据链）：
			//       已扫描 ⇒ 挂 StateFloraScanned（默认 5 = 原版「已扫描」绿 #27C684）；
			//       未扫描 ⇒ 挂 StateFlora（7 = 原版青色脉冲）；
			//     ★ 放下扫描仪那一刻 + 换场景 / 读档时缓存作废 ⇒ 刚扫完的东西
			//       立刻（下一个扫描节拍，≤200ms）变绿。
			// 0 = 退回 v4.24 行为（整个「植物」类别只用 StateFlora 一个状态）。
			bool          floraScannedByResource = true;

			// ★★★ v4.28：**主判据** —— 直接问引擎「这个引用扫没扫过」
			//   （`ScannableComponent::GetOutlineState(ref)`：**1 = 未扫描 / 2 = 已扫描**；
			//    引擎自己的原生函数 `IsScanned` 就是它 `== 2`，证据链见常量区）。
			// 为什么还要这一条：`0x1597A50` 那条资源链**只对「产出物品是 LVLI」的
			//   FLOR 成立**（引擎 `0x159ED90` 里就有 `cmp byte [rax+0x2E],0x3F / jne`），
			//   而**植物**的产出物品是 **MISC** ⇒ 拿它的资源数组去判「已扫描」是错的：
			//   植物产出的那个资源（如有机纤维）只要进过勘测数据，整片植物在**没扫描前**
			//   就会被涂成「已扫描」的绿色 —— 用户 v4.27 实测反馈的正是这个
			//   （矿石 / 气泉 / 液池的产出是 LVLI ⇒ 它们一直是对的）。
			//   GetOutlineState 按**引用**回答（引擎内部按 species / resource 解析
			//   canonical id），对植物 / 矿脉 / 气泉 / 液池一视同仁，是原版 HUD 的口径。
			// 1 = 启用（默认）；0 = 只用资源链（= v4.27 口径，植物会偏绿）。改完重进游戏。
			bool          floraScannedByEngineState = true;

			// ================================================================
			// ★★★ v4.33：「已扫描植物低概率变青」的二次加固（用户实测复现 + 日志实证）
			// ================================================================
			// 本会话实测日志（2026-09-26 22:39:49 ~ 22:43:11，约 3.5 分钟）：
			//   换场景 / 读档之后，**窗口内所有星球目标都被判「未扫描」**（整片青），
			//   直到用户**举一下扫描仪**（引擎重写 state 4/5）才恢复绿色。
			// 三个可修的点（全部与「单向学习表」有关，证据链见 docs/31）：
			//   ① `reset (载入画面关闭…)` **每次都清空学习表** —— 而「已扫描」是
			//      base（物种 / 资源）级的单向事实，读档 / 快速旅行后没有清的理由；
			//   ② 学习表只活在内存 ⇒ **每次重开游戏都要重新「学」（= 再开一遍扫描仪）**；
			//   ③ v4.31 的「沿用旧结论」条件 `!chain.shapeOk` 对**植物**永远不成立
			//      （植物 produceIsMisc ⇒ shapeOk 恰好 = true）⇒ 植物从未被沿用保护。
			//
			// 1 = 学习表**落盘**（`SAS_AlwaysScan.flora-learn.txt`，与 esm / INI 同级）：
			//     每学到一条「已扫描」立刻追加一行，启动时读回 ⇒ 跨会话保留。
			//     ⚠️ 该表按 **base** 记录、**不区分存档** —— 换存档玩时，另一个存档里
			//        扫过的物种也会显示绿色（只是颜色观感，不影响任何玩法判定）；
			//        想严格按存档 ⇒ 设 0（只在本会话内有效）。
			bool          floraLearnPersist = true;

			// 1 = 载入画面关闭 / 换场景时**清空**内存学习表（= v4.31 / v4.32 行为）；
			// 0 = 不清（★ v4.33 默认）—— 「已扫描」不会退回，base 级记忆跨场景有效。
			//     ★ 这是本轮「换场景后整片变青」的直接修复（用户日志实证）。
			bool          floraLearnClearOnLoad = false;

			// 「未确认已扫描」的判据缓存 TTL（毫秒，默认 **5000**）。
			//   背景：引擎 `GetOutlineState(ref)` 对**同一个引用**会一会儿答 2、一会儿
			//   答 1（组件 / 登记未就绪），旧版把结果一律缓存 30 秒 ⇒ 引擎刚变回
			//   「已扫描」也要等 30 秒才换色。现在：判成「已扫描」的保持 30 秒 TTL，
			//   **判成未扫描的**按这个键重问 ⇒ 引擎状态一旦恢复立刻（≤5 秒）变绿。
			//   0 = 退回 30 秒（= v4.32 行为）。
			int           floraUnscannedTtlMs = 5000;

			// ================================================================
			// ★★★ v5.1.5（订正 R5）：**「已扫描」记忆按存档隔离**（热键 = 引擎读档事件）
			// ================================================================
			// 用户报告（原文）：
			//   「未扫描星球资源直接显示绿色问题还存在，特别是重新读档后（未扫描时的存档）
			//    问题非常严重」
			//
			// 根因：按引用的「已扫描」记忆（含落盘文件 `SAS_AlwaysScan.flora-learn.txt`）
			//   **不区分存档**，而「已扫描」这件事**是存档里的勘测数据**决定的：
			//   · 在「后玩的存档」里学到的一批绿（引擎亲手画过 4/5 / 状态 == 2 / 链命中）
			//     会**跨存档**保留 ⇒ 读回「更早的存档」（勘测数据更少）时，
			//     那些引用在**这个存档里根本没扫过**，却仍然被判「已扫描」= 假绿；
			//   · 更要命的是「按物种（base）扩散表」（v5.1.2）：一个假绿的引用会把它的
			//     species 记进表 ⇒ **同 species / 同资源的所有实例**一起变绿 ——
			//     用户看到的「问题非常严重」就是这个放大器。
			//
			// 修法：把**引擎自己的读档事件**（`TESLoadGameEvent`）当**存档边界**
			//   （换场景 / 快速旅行 / 进门都不算 —— 那些不会让勘测数据倒退）：
			//     · 每次读档：`floraSaveEpoch + 1`，并把**按物种扩散表**清空
			//       （它只该由「本存档内当场见证」的结论重建）；
			//     · 记忆条目自带 `epoch`（见证于第几次读档之后）——
			//       **不在本存档作用域内**的条目不再产生「已扫描」（见 FloraEntryInScope）；
			//     · 每次读档顺手做一次**资源链复核**：对记忆里出现过的 base 逐个问引擎
			//       `0x1597A50`（这个资源在不在**本存档**的勘测数据里）——
			//       链走通且答「没有」⇒ 该 base 的全部条目当场丢掉（矿脉 / 气泉 / 液池；
			//       植物产出 MISC ⇒ 链不适用、不丢，交给「本存档内见证」规则）。
			//
			// 取值：
			//   1 = ★ 默认（本次修复）：**本会话第一次读档**仍信任落盘记忆
			//       （= 重开游戏继续玩同一个存档时，扫过的目标不用再开一遍扫描仪 ——
			//         docs/33 验收 ⑤ 的体验保留），**之后**每次读档只认「该存档内见证过」
			//       的绿（+ 资源链当场复核）。
			//   2 = 最严格：落盘记忆一律只当提示（连第一次读档也不认），全部要求
			//       「本存档内见证」—— 假绿投诉仍存在时的对照开关。
			//   0 = 旧行为（v5.1.4：记忆永远有效、物种表不清）—— 只为对照 / 回退。
			int           floraMemoryScope = 1;

			// ★★★ v5.1.5（订正 R5）→ ★★★ v5.1.8（订正 R8）：**按物种（base）扩散是否按星球收紧**。
			//   R5 的初衷：v5.1.2 的扩散表只按 base 记（全局），怕「在星球 A 学到的物种
			//   让星球 B 上没扫过的同种资源也变绿」⇒ 加了「必须同一颗 worldspace」的硬拒绝。
			//   R8 的依据（用户 2026-09-27 10:50 那一局日志实证）：在星球 A 本会话亲手扫过的
			//   物种，星图快速旅行到 B 之后**引擎自己**（一举扫描仪）就把同 species 的实例
			//   画成绿 4/5 ⇒ **引擎的物种知识本来就是跨星球生效的**；按星球硬拒绝只会把
			//   「本存档里明确已扫描」的目标涂成青色（= 用户第三次报的「传送后变青」）。
			//   ⇒ 默认 **0 = 引擎口径（全局物种表，作用域仍由「存档」管）**：
			//     · 物种表是**会话级**、不落盘；读档时按**游戏时间锚点**剪枝（R7）；
			//     · 落盘记忆只在**作用域内**才播种（R8，见 SeedFloraBaseFromRefMemory）；
			//     ⇒ 跨星球扩散不会再外溢到别的存档（R5 真正要防的是这个）。
			//   `1` = 保留 R5 的严格口径（只在同一颗星球内扩散）—— 只为对照 / 回退。
			bool          floraSpeciesPlanetScope = false;

			// ================================================================
			// ★★★ v5.1.6（订正 R6）：**「读档边界」改为证据驱动**（治「传送后变青」）
			// ================================================================
			// 用户报告（原文）：
			//   「传送切换地图后，出现了已扫描物品变成扫描前青色的问题，
			//    开启扫描仪再关闭又变回绿色」
			//
			// 根因（用户日志实证，2026-09-27 09:55 那一局）：
			//   · 引擎的 `TESLoadGameEvent` **不只是读档会发** —— 部分「传送 / 切换地图」
			//     也会发（那一局的「读档 #2」就是用户的传送：09:52 的几次快速旅行
			//     都没发事件，只有 09:55:51 的换地图发了）；
			//   · v5.1.5 把「事件 ⇒ 无条件推进作用域 + 清物种表 + 丢条目」绑死 ⇒
			//     传送时把**真绿**也作废了（那次丢掉 21 条 / 证伪 8 个 base，
			//     而用户随后一挙扫描仪，引擎又把同一批目标画成绿 = 它们在本存档里
			//     确实是已扫描的 ⇒ 那次「证伪」是**假证伪**）；
			//   · 假证伪的来源：复核发生在「载入画面关闭」的那一瞬间 —— 换世界空间时
			//     勘测数据 / 上下文可能还没就绪，而且复核用的是**当前星球**的上下文
			//     （记忆里别的星球的 base 在它上面查必然「没扫描」）。
			//
			// 修法（三块，都在 ProcessFloraSaveLoad / MaybeProcessFloraSaveLoad）：
			//   ① **复核延后到世界稳定**：事件只记「待复核」，等「载入画面关闭 +
			//      `kFloraScopeEvalDelayMs`（4 秒）且没有新的载入」再跑 —— 那一刻
			//      勘测数据一定已就绪；
			//   ② **跨世界空间跳过证伪**：这个 base 的见证 worldspace（物种表里有）
			//      明确只有别的世界空间 ⇒ 在当前上下文复核它没有意义 ⇒ 跳过
			//      （不当作「证伪」）；
			//   ③ **没有证伪就不推进**：资源链一个 base 都证伪不了 ⇒ 视为「传送 /
			//      读同一存档」（勘测数据没倒退）⇒ **记忆保持有效**（作用域不推进、
			//      物种表不清、条目不丢），而不是像 v5.1.5 那样无条件作废。
			//
			//   1 = ★ 证据驱动（默认，本次修复）；0 = v5.1.5 旧行为（事件即边界，
			//   无条件推进 / 清物种表 / 丢条目）—— 只为对照 / 回退。
			//   ★ `FloraMemoryScope=2`（最严格）不受本开关影响：仍按「每次事件都推进」。
			bool          floraLoadBoundaryEvidence = true;

			// ★★★ v5.1.7（订正 R7）：**读档边界改用「游戏时间指纹」**（默认开，1）。
			// ----------------------------------------------------------------
			// 为什么换口径（完整证据链见 docs/40）：v5.1.6 把「资源链复核有证伪」当作
			//   「读回了更早的存档」的证据，但**载入刚结束时资源链的否定答案是错的** ——
			//   用户 2026-09-27 10:15 那一局实证：
			//     · 10:15:38（载入画面关闭 + 4s）复核 15 个 base ⇒ 9 个「证伪」⇒
			//       删掉 132 条记忆（用户看到的就是「已扫描物品变青」）；
			//     · 10:15:48 用户一举扫描仪，引擎自己把**同一批** base 画成绿（state 4/5）；
			//     · 10:16:56 资源链对同一批里的 base=0x270033 答「已扫描」（命中）——
			//       **同一个函数、同一个资源，80 秒后答案相反**。
			//   ⇒ 「没扫描」这个否定答案在载入刚结束的那段时间**没有区分度**
			//     （链路上下文没就绪时也答「没扫描」），不能拿它做不可逆的作废。
			//
			// 新口径 = **游戏时间**（`Calendar::gameDaysPassed`；随存档一起被保存 / 读回）：
			//   · 每条记忆自带**学习时刻**（落盘行第 3 个字段；旧格式行 = 0 = 未知）；
			//   · 读档时把「本存档的游戏时间」记成**锚点**：
			//       学习时刻 ≤ 锚点 + eps ⇒ 这个扫描发生在本存档的过去 ⇒ 记忆有效；
			//       学习时刻 >  锚点 + eps ⇒ 属于更新的时间线 ⇒ **不在作用域**（不产生绿）；
			//   · ★ **作废是可逆的、且不删条目**：再读一个更新的存档，条目就自动回来
			//     （v5.1.5/v5.1.6 是「删掉 + 举扫描仪重学」）；
			//   · 传送 / 继续同一存档 ⇒ 锚点几乎不动 ⇒ 什么都不作废（本次修复的核心，
			//     不再依赖「资源链自己证明自己」这种脆弱证据）；
			//   · 读回更早的存档 ⇒ 只有「未来」的条目失效，其余照常有效 —— 比 R5 的
			//     「一律作废、植物要靠举扫描仪重学」精准得多。
			//   `FloraMemoryScope=2`（最严格）仍然走旧口径（对照开关）。
			//   0 = 回退到 v5.1.6 的链证据路径（万一 Calendar 读不到也会自动回退）。
			bool          floraSaveFingerprint = true;

			// ================================================================
			// ★★★ v5.2：植物「已扫描」= **直读引擎的扫描进度表**（用户要求：抛弃自建记忆）
			// ----------------------------------------------------------------
			// 用户指令（原文，2026-09-27）：「不能直接读取游戏自己的物件状态来判断吗？
			//   …… 我们还是走植物也直接读游戏本身物件状态的方式，抛弃自己记忆的方法」。
			// 做法：复刻引擎给「已扫描植物」写 state 4/5 时用的那条**纯查询**（见常量区
			//   `kRvaFloraKnowledgeId` 那段的反汇编证据链）——即 `PlayerKnowledge` 的
			//   物种槽 `percent`（0..100；== 100 = 已扫描）。查询是**只读**的：
			//   · key1 = `[0x81 组件 + 0x28]`（可扫描组件的物种/资源 ID，直读内存）；
			//   · key2 = `0x1307180(ref)`（"ref → 知识 ID"的纯查询函数）；
			//   · 表  = `[[0x23FF640() + 0x8B0] + 0x268]`（TLS 单例 + 知识库主表）；
			//   · `0x24105D0` / `0x23467B0` = 引擎自己的两级哈希查找（FNV-1a，纯只读）；
			//   · 命中后读 `[元素+0x20]` 的 byte = 该物种的扫描进度。
			// ⇒ 对**任何引用**（含植物、含运行时临时引用的外景目标）**任何时刻**可问，
			//   不再依赖「引擎在可见窗口里画过 / 我们记下来」。判定：`percent == 100`
			//   ⇒ 已扫描（绿）；`percent < 100` ⇒ **不短路**（继续走后面的判据，宁可青）。
			//   结果按 **base（物种）**缓存 —— 「扫一个实例 ⇒ 同 species 全绿」由
			//   **引擎数据本身**保证（进度就是物种级的），与 v5.1.2 的植物扩散同语义，
			//   但数据源是引擎而不是我们的记忆；读档时清缓存（进度是存档级数据）。
			// 签名 / 形状校验全过才启用；任何一步不符 ⇒ 自动回退（新判据不生效）。
			// 0 = 关（回退到 v5.1.8 的记忆判据链）。
			bool          floraEngineProgress = true;

			// ★★★ v5.2：**自建记忆层总开关**（用户要求：抛弃记忆、直读引擎）。
			//   0 = ★ 默认（v5.2）：以下全部**不参与判定**，也不再学习 / 落盘 ——
			//       · ⓪ 按引用记忆（`floraRefKnow` / `SAS_AlwaysScan.flora-learn.txt`）；
			//       · ⓪.2 按物种扩散表（`floraBaseKnow`）、R8 的「落盘记忆播种」；
			//       · 「没有权威答案就沿用旧结论」的粘滞保护。
			//       「已扫描」只由**引擎自己的数据**回答：扫描进度直读（上面那条）
			//       + 引擎状态表只读探针（4/5）+ `GetOutlineState` + 资源链（LVLI）。
			//       为什么能不要记忆了：v5.1 那一整族 bug（全绿 / 变青 / 跨星球外溢 /
			//       跨存档外溢）的根源都是「我们的记忆 ≠ 引擎的事实」；进度直读把
			//       「引擎的事实」本身变成了查询结果（进度是存档级数据、引擎自己管）。
			//   1 = 保留 v5.1.8 的记忆行为（只为对照 / 回退；用户实测旧口径有问题时
			//       不要再打开，改「读日志 → 修直读路径」）。
			bool          floraUseMemory = false;

			// 「已扫描」的星球目标用哪个 outline 状态（0..11）。默认 **5**：
			//   引擎把「已经在勘测数据里的星球目标」写进 state 4（远）/ 5（近），
			//   两者原生色都是**绿色 #27C684**（`outline colors` 日志里的
			//   state=4 Bounty / state=5 Social），所以选 5（原版近处用的那个）不会
			//   引入任何新颜色；想让它退成「和未扫描一样」就把这里设成与
			//   StateFlora 相同的值（7）。
			//   ⚠️ 5 也是「弹药、救援」用的槽位（v4.24 起那个槽位的颜色同样交给引擎
			//      = 原版绿）⇒ 两者同色，不冲突；若你把 StateFloraScanned 改成 4，
			//      则与「设备」同槽位（同样是原版绿）。
			int           stateFloraScanned = 5;

			// ★★★ v4.25（诊断）：`flora scan:` 探针最多打几条（默认 8；0 = 关掉）
			//   每条 = 一次**真实的判据查询**（带缓存，所以一个 base 最多每 2 秒一条），
			//   内容：`base=… produceItem=…(LVLI) misc=…(MISC) irES=…(IRES) -> 已扫描=1/0`。
			//   用途：① 确认判据真的在跑（`已扫描=1` 出现 = 引擎说这个资源扫过了）；
			//         ② 出问题时（永远 0 / 形状校验 WARN）这一行带上链上三个指针，
			//            可以直接和离线反汇编对照 —— 偏移是不是又变了。
			int           floraScanProbeMax = 8;

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
			// ★ v4.2：尸体默认也是 **9 橙**（和容器同色，两者都是「搜刮目标」）；
			//   想区分开就改 INI 的 `StateCorpse`（可用的其它色见 INI 里那张实测配色表）。
			//
			// ★★ v4.17：物品栏分类分色（需求 1.6）—— 5 个「可拾取」子类各一个新状态，
			//   其它类别原样不动（**杂项继续 2 蓝**）。状态分配的依据（全部离线实证，
			//   见 docs/16 §配色 与 out/esm_invcat_probe.py）：
			//     · **引擎自己会写**的状态（反汇编 0x159ED90 = 原版扫描仪的逐引用求值：
			//       0/1、2/3、4/5、7/8、9、10 —— 全部 11 个槽位里**只有 6 号它从不写**）
			//       ⇒ 尽量别动它们；
			//     · ★★★ v4.24 订正：v4.17~v4.23 那句「4/5/6/10 全镜像没有任何代码写」
			//       **是错的**（当时只看了函数尾部 0x159F604~0x159F654 那一段）——
			//       4/5 由 `add edx,4`（0x159F4AF / 0x159F562）产生，10 由
			//       `mov edx,0xa`（0x159F601）产生。**4/5 = 已扫描的星球目标（原生绿）**
			//       ⇒ v4.24 起颜色归还引擎（证据链见 docs/23）。真正的「MOD 专用」只剩 6。
			//   ★★★ v4.19：**颜色不再依赖状态的原生值** —— 用户实测反馈「不同类别
			//     看起来是同一个颜色」（原生 state 0/1/2/3 全是蓝色系：青 / 淡蓝白 /
			//     蓝 / 蓝，根本分不开），要求「区分度要高、别用相近色」。现在按类别
			//     **逐一覆盖颜色**（见下面的 colorOverride），状态只当「颜色槽」用：
			//     同一状态 = 同一色 ⇒ 共享状态的类别必须同色（容器+尸体 = 9、
			//     设备+弹药救援 = 4/5 —— ★ v4.24：这两个槽位不再覆盖颜色）。
			//   ★★★ v4.22（用户实测：「星球上的矿石、气体、液体、植物、动物现在扫描前和
			//     扫描后颜色无法区分了，这两个状态保持游戏原版颜色即可」）：
			//     **state 7 / 8（TargetScannable / TargetScanned）必须留给引擎** ——
			//     原版扫描仪在星球上扫「矿石 / 气体 / 液体 / 植物 / 动物」用的就是这一对
			//     槽位（离线 dump 那对槽位的原值：基色 0x00000000、脉冲 #72E8FF/#115B69，
			//     7 与 8 完全一致 ⇒ 原版的「扫描前 / 扫描后」区别**只可能**来自引擎自己
			//     往这两个槽位里写的东西）。我们一旦覆盖 7 的 RGB，就等于把这一对槽位
			//     改成同一种颜色 ⇒ 「扫描前后分不出」。
			//     ⇒ 「资源」**不再借用 state 7**，改用 **3（Tracked）**并覆盖成紫：
			//       3 的原生色与 2 同为蓝 #1F8EE2，是引擎写的最少用的一路
			//       （0/1 通用、2/3 完全扫描/追踪、7/8 目标、9 目标完全扫描），
			//       借它当「资源色」对原版观感的代价最小（`StateResource` 可再改）。
			//   ★ kOther（MSTT，默认关）改回 **2（与杂项同 state 同色）** ——
			//     MSTT 是桌椅 / 纸箱这类装饰物，「跟杂项一个蓝」本来就是设计意图
			//     （同 state 同色，不会触发撞色 WARN）。
			//
			// ★★★ v4.30：**state 0 / 1 归还引擎**（用户实测：「扫描中的 NPC 全部
			//   变成了这种只有带有赏金的人物才会出现的颜色」）。真根因：引擎的
			//   逐引用求值函数（`0x159ED90`）对**通用引用（含活人 NPC）**写
			//   **state 0（远）/ 1（近）**（0/1 分支：`bl == 0 ∧ 原状态 = 12`，
			//   `setne dl`；反汇编与截图取色证据见 docs/28），而 v4.19~v4.29 把
			//   这两个槽位覆盖成「武器 红 / 服饰 品红」⇒ 举着扫描仪时所有行人
			//   跟着变色（与用户说的「赏金人物色」一致）。
			//   ⇒ 修法与 v4.22 归还 7/8、v4.24 归还 4/5 完全同构：**0/1 一个字节
			//     都不写**；原本住在那里的类别搬家：
			//       · 「武器」 0 → **9**、「服饰」 1 → **10**（继续用 MOD 自定色：
			//         红 #FF2E2E / 品红 #FF3BD4 —— 9/10 的引擎目标很少见，风险
			//         与「容器 / 门」原先住 9/10 时相同）；
			//       · 「容器 / 尸体」 9 → **1**、「门」 10 → **0**（改成**原版色**：
			//         1 = 亮青 #72E8FF 脉冲、0 = 青 #3EADF2 脉冲，都不覆盖 ——
			//         搜刮目标仍然醒目，门的观感仍近白）。
			//
			// ★★★ v4.32：**分组配色定稿**（用户需求 `颜色分类.md`）—— 两条并组，
			//   不引入任何新槽位 / 新覆盖（这就是它「很安全」的全部原因）：
			//     · 「太空服 / 背包 / 头盔 / 服饰」 **1 → 10**（并进「武器 / 投掷物」的
			//       红组 ⇒ 整套装备一个色）；
			//     · 「笔记」 **0 → 3**（并进「资源」的紫组）。
			//   其余不动：容器 / 尸体 = 9（橙）、门 = 6（白）、弹药 / 救援 = 5
			//   （原版绿）、杂项 = 2（原版蓝）、设备 = 4（原版绿）、植物 = 7（原版青）。
			//   ⇒ 覆盖槽位仍是 5 个（2/3/6/9/10）、覆盖色集合一字未变（红 / 紫 / 白 /
			//     橙 / 蓝），state 0/1/4/5/7/8 依旧一个字节都不写 ⇒ v4.30 的 NPC
			//     修复与星球目标「青 ↔ 绿」全部不回归。v4.31 里「服饰 / 笔记让位」
			//     的妥协就此取消（换组不换色，代价为零）。
			std::array<int, kCategoryCount> stateByCategory{
				2,  // kLoot        杂项 —— 蓝（**原生不变**，用户需求）
				10, // kLootWeapon  武器、投掷物 —— ★★★ v4.31：9 → **10** + **覆盖为红**。
					//      10 是 v4.21~v4.29 的老「门」槽位（引擎目标罕见），
					//      用来纪念「武器红」（见 colorOverride 的 v4.31 段）
				10, // kLootApparel 太空服/背包/头盔/服饰 —— ★★★ v4.32：1 → **10**
					//      （并进「武器」的**红组** —— 用户「颜色分类」：整套装备一个
					//      色；与武器共槽同色，见 v4.32 段与 colorOverride）
				// ★★★ v4.24：state 5 / 4 **归还引擎**（引擎用它们画「已扫描的星球目标」= 绿色；
					//   证据见 colorOverride 上方的长注释）。这两个类别继续用 4/5，但颜色 =
					//   原生绿 #27C684（弹药救援仍是「绿」这一组；想自定义见 INI 的 ColorAmmoAid）。
				5,  // kLootAmmoAid 弹药、救援 —— 原版绿 #27C684（不覆盖，state 5）
				3,  // kLootNote    笔记 —— ★★★ v4.32：0 → **3**（并进「资源」的
					//      **紫组** —— 与资源共槽同色 #B36BFF，见 colorOverride）
				3,  // kLootResource 资源 —— 紫 #B36BFF（★ v4.22：7 让给原版扫描目标）
				9,  // kContainer   容器 —— ★★★ v4.31：1 → **9** + **覆盖为橙** ——
					//      9 的原生色本来就是橙（TargetFullyScanned #FFAA00，v4.19 起
					//      一直覆盖成 #FF9500：加了 noFill 的「轮廓橙」）⇒ 用户要的
					//      「恢复容器橙」回来了（见 v4.31 段）
				4,  // kDevice      设备 —— 原版绿 #27C684（不覆盖，state 4；★ v4.24 归还引擎）
				6,  // kDoor        门 —— ★★★ v4.31：0 → **6** + **覆盖为白**。
					//      6 是**引擎从不写**的唯一槽位（v4.24 订正后的结论）⇒
					//      用它 = 对原版零影响，「门白」原样恢复（见 v4.31 段）
				7,  // kFlora       植物 / 矿脉 / 气泉 / 液池 —— ★★★ v4.23：**改回 7**
				//     （= 原版 `TargetScannable`，颜色**不覆盖** ⇒ 原生青色脉冲轮廓）
				//     理由见上面 v4.23 段与 categoryEnabled 里的长注释。
				2,  // kOther       MSTT（默认关）—— 与杂项同 state（同蓝、不覆盖）
				9   // kCorpse      尸体 —— ★★★ v4.31：1 → **9**（= 容器；橙、与容器同色）
			};

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
			//   `EnableLoot / EnableWeapon / EnableApparel / EnableAmmoAid /
			//    EnableNote / EnableResource / EnableContainer / EnableDevice /
			//    EnableDoor / EnableFlora / EnableOther`，改完重进游戏生效。
			//   ★ v4.17：`EnableLoot` 现在的含义 = **杂项**（原来那一坨里剩下的）；
			//     新增的 5 个物品子类各有自己的开关（默认全开）。
			//
			//   ★★★ v4.22：**「植物」默认关**（用户实测需求 1 原文：
			//     「星球上的矿石、气体、液体、植物、动物现在扫描前和扫描后颜色无法区分了，
			//      这两个状态保持游戏原版颜色即可」）。
			//     根因（离线全量取证，`out/esm_resnode_probe.py`，Starfield.esm 382 万条）：
			//       **FLOR 记录里既有植物、也有星球上的矿脉 / 气泉 / 液池** ——
			//         · 植物：`FloraBloodStoneTall` 等（关键词 `FloraTypeOrganic`，169 条）
			//         · 矿石 / 气体 / 液体：`MineralDeposit*`（关键词 `FloraTypeInorganic`
			//           + `FloraTypeSolid / FloraTypeGas / FloraTypeLiquid`，125 条）
			//       ⇒ 它们**全是原版扫描仪的「可扫描目标」**，原版靠 state 7/8 这对槽位
			//         区分「扫描前 / 扫描后」。MOD 一旦常亮涂成亮绿（state 5），
			//         这一对槽位的区别就被永久盖掉 —— 用户在星球上就再也分不出扫没扫过。
			//     ⇒ v4.22 把整个「植物」类别关掉（**留给原版引擎自己上色**）；
			//       想恢复 MOD 的常亮绿，INI 里写 `EnableFlora=1`（代价 = 又看不到
			//       扫描前后的区别，二者不可兼得）。
			//
			//   ★★★ v4.23 订正（用户实测反馈 1：「星球上的矿石、气体、液体、植物、
			//     动物**现在颜色完全不显示了**」）：v4.22 的「交还原版」在本 MOD 的
			//     语境里等于「什么都没有」—— **原版只在举着扫描仪时上色**，而本 MOD
			//     的存在意义正是「不举扫描仪也有高亮」。所以不是「开着就看不到扫描
			//     前后的区别」这一个二选一，而是**三个约束可以同时满足**：
			//       ① 不举扫描仪 ⇒ 本 MOD 用 **state 7** 挂上（原版 `TargetScannable`
			//          槽位 + **不覆盖颜色** ⇒ 青色脉冲轮廓），星球上一直看得见；
			//       ② 举着扫描仪 ⇒ `YieldTargetsWhileScanning=1` **这一类让位原版**
			//          （不挂新 / 不重申 / 不摘），引擎自己的 7/8/9/10 与
			//          「扫描前 / 扫描后 / 正在扫描」的区别原样呈现；
			//          其余类别照常重申 ⇒ 「圆圈之外也高亮」不受影响；
			//       ③ 放下扫描仪 ⇒ v3.2 的 `DetectEngineOutlineLoss`（MonocleMenu
			//          由开变关）整批重申 ⇒ 回到 ①。
			//     ⇒ 「植物」类别**默认开**、state = 7、`ColorFlora` 保持 unset。
			//       想退回 v4.22 的「完全不碰 FLOR」⇒ `EnableFlora=0`。
			//       想回到 v4.17~v4.21 的常亮亮绿 ⇒ `EnableFlora=1` + `StateFlora=5`
			//       （+ `ColorFlora=00FF66`，若 INI 模板里那行已被注释掉则无需）。
			std::array<bool, kCategoryCount> categoryEnabled{
				true, // kLoot        杂项
				true, // kLootWeapon
				true, // kLootApparel
				true, // kLootAmmoAid
				true, // kLootNote
				true, // kLootResource
				true, // kContainer
				true, // kDevice
				true, // kDoor
				// ★★★ v4.23：**「植物 / 矿脉」重新默认开**（v4.22 曾默认关，用户实测
				//   「星球上的矿石、气体、液体、植物、动物现在颜色完全不显示了」）。
				//   v4.22 的思路是「整类交还原版」，但原版只在**举着扫描仪**时上色 ——
				//   而本 MOD 的立身之本就是「不举扫描仪也有高亮」⇒ 交还原版就等于
				//   在星球上什么都看不到。正解是**两者兼得**：
				//     · 本类 state = **7**（原版 `TargetScannable` 槽位）、
				//       `ColorFlora = kColorUnset` ⇒ **用原版颜色**（青色脉冲轮廓，
				//       与「杂项蓝 / 资源紫 / 弹药绿」区分得很开），不是 MOD 自造色；
				//     · 举着扫描仪时这一类让位原版（`YieldTargetsWhileScanning=1`）⇒
				//       「扫描前 / 扫描后 / 正在扫描」的原版状态与颜色**一个都不被盖**。
				//   ⇒ 不用扫描仪 = 青色常亮；用扫描仪 = 完全原版（含扫描前后的区别）。
				true,  // kFlora
				false, // kOther（MSTT —— 默认关，理由见上）
				true   // kCorpse
			};

			// ★ v4.0.1：可选的「自定义类别颜色」（INI 里写 ColorLoot=RRGGBB 之类）。
			//   kColorUnset = 不覆盖，完全用引擎那个状态的原生配色。
			//   设了就把 RGB 写进引擎的每状态**配色块**（+0x00/+0x20 脉冲、+0x80 基色），
			//   再让引擎建/刷管理器 + 整批重挂（详见 WriteColorOverrides / ApplyColorOverrides）。
			//   ★★ v4.19：「**默认值就是最终配色**」—— 除 kOther（MSTT，默认关）外
			//     全部类别都带默认色，所以**不写 INI 也有一套高区分度的颜色**；
			//     INI 里的 `ColorXxx` 只是「想改才写」。INI 缺这些键**不再影响效果**。
			// ★★★ v4.19：默认配色改为**高区分度调色板**（用户要求「不同类别颜色区分度
			//   要高，不要弄太相近的颜色，肉眼很难分辨」）。
			//   配色原则（每条都写进 docs/18）：
			//     · 六个「物品组」占据六个相隔 ≥44° 的色相：
			//         武器 红 #FF2E2E(0°) / 服饰 品红 #FF3BD4(316°) / 弹药救援 绿 #27C684(150°，原生)
			//         / 笔记 黄 #FFD700(51°) / 资源 紫 #B36BFF(268°) / 杂项 蓝 #1F8EE2(207°，**原生不动**)
			//     · 世界类目标（容器/尸体 橙 #FF9500、设备 绿 #27C684、门 白 #FFFFFF）
			//       也都跟上面六个错开；
			//     · **共享 state 的两组颜色必须一致**（容器+尸体 = 9、设备+弹药救援 = 4/5），
			//       否则会互相覆盖（WriteColorOverrides 里有撞色 WARN）。
			//   ★ 结论：能覆盖的**都尽量覆盖**（不再依赖「原生状态色」—— 原生 0/1/2/3 全是
			//     蓝色系，正是用户说的「看起来一样」）。
			//
			// ★★★ v4.24：**state 4 / 5 归还引擎** —— 设备 / 弹药救援不再覆盖颜色。
			//   用户实测反馈（原文）：
			//     「矿石、气体、液体、植物、动物现在颜色扫描前是原版颜色，但是举起扫描仪
			//       扫描的颜色不是原版，而且扫描后，没有变成原版扫描后的绿色」
			//   硬证据（反汇编 0x159ED90，引擎扫描仪的**逐引用求值函数**）：
			//     · 对**星球的矿石 / 气体 / 液体 / 植物**（base = **FLOR**，0x2E；FLOR 里既有
			//       植物也有 `MineralDeposit*` 矿脉 / 气泉 / 液池 —— 见 docs/21 §1.1），
			//       引擎会顺着 `FLOR+0x260`（= `TESProduceForm::produceItem`，一个 **LVLI**）
			//       → 第一个条目（**MISC**）→ `MISC+0x238`（= `BGSCraftingResourceOwner`
			//       的 `unk10`，24 字节三元组数组）→ 取里面的 **BGSResource（IRES，0x9F）**，
			//       再调 `0x1597A50(irES)` = **「这个资源是不是已经扫描过（进了勘测数据）」**；
			//     · 只要命中 ⇒ `add eax,4` / `add edx,4`（0x159F562 / 0x159F4AF）⇒
			//       **state 4（远）/ 5（近）** —— 而这两个槽位的原生色就是**绿色 #27C684**
			//       （`outline colors[install]`：state=4 Bounty / state=5 Social = 39,198,132）。
			//     ⇒ 用户说的「原版扫描后的绿色」= **state 4 / 5**。
			//   v4.19~v4.23 把 4 覆盖成「设备 青 #00E5FF」、5 覆盖成「弹药救援 亮绿 #00FF66」
			//   ⇒ 举着扫描仪时，**已经扫描过的**星球目标显示成青色（远的）/ 亮绿（近的），
			//     永远看不到原版那个绿色 —— 正是用户这一轮报的两个症状。
			//   ⇒ 修法：4 / 5 的颜色**一个字节都不写**（与 v4.22 归还 7/8 同一个道理）；
			//     「设备」「弹药救援」**继续用这两个槽位**（类别逻辑不变），颜色变成
			//     引擎原生绿 #27C684（弹药救援本来就是绿组，观感变化最小）。
			//   ★ 教训（写进 docs/23）：11 个槽位里**只有 state 6 是引擎全镜像不写的**
			//     （0/1、2/3、4/5、7/8、9、10 全会写），所以「借一个状态当自己的颜色」
			//     永远是在赌「引擎不会在我看得见的地方写它」—— 借之前先看这条注释。
			//
			// ★★★ v4.30：**state 0 / 1 归还引擎**（同一条教训的第二个案例）。用户实测：
			//   「扫描中的 NPC 全部变成了这种只有带有赏金的人物才会出现的颜色」。
			//   根因（反汇编 `0x159ED90` 的 0/1 分支 + 截图逐像素取色，见 docs/28）：
			//   引擎对**通用引用（含活人 NPC）**写 **state 0（远）/ 1（近）**
			//   （`setne dl` ⇒ 0/1；条件 `bl == 0 ∧ 原状态 = 12`）—— 而 v4.19~v4.29
			//   把 0/1 覆盖成「武器 红 / 服饰 品红」⇒ 举着扫描仪时**所有行人**
			//   变成红 / 品红（= 用户说的「赏金人物色」；远处红、近处品红）。
			//   ⇒ 修法：**0/1 一个字节都不写**；武器 / 服饰改挂 **9 / 10**
			//     （继续用 MOD 自定色：红 / 品红）；容器 / 尸体 / 门搬到 **1 / 0**
			//     并使用**原版色**（亮青脉冲 / 青脉冲，见下）。
			//   ⇒ 覆盖列表从 7 个降到 **5 个（2/3/6/9/10）** —— 「举着扫描仪时
			//     引擎画的东西」只剩这五个低风险槽位还可能被改色。
			std::array<std::uint32_t, kCategoryCount> colorOverride{
				0x001F8EE2u,   // kLoot        杂项 —— 蓝 #1F8EE2（= 原生值，用户要求「不变」）
				0x00FF2E2E,    // kLootWeapon  武器、投掷物 —— 红（★ v4.31 起挂在 state 10）
				// ★★★ v4.32：**覆盖为红 #FF2E2E**（与「武器」同色 —— 用户需求
				//   `颜色分类.md` 把整套装备（武器 / 投掷物 / 太空服 / 背包 / 头盔 /
				//   服饰）并进**红组**，state 10 与武器共用；v4.31 的「原版淡蓝白」
				//   不再使用。同 state 同色 ⇒ 不会触发撞色 WARN）。
				0x00FF2E2E,    // kLootApparel 太空服/背包/头盔/服饰 —— 红（★ v4.32 与武器同组）
				// ★★★ v4.24：**不覆盖** —— state 5 是引擎给「已扫描的星球目标（近）」
				//   画绿色的槽位（见上面长注释的硬证据）；写它 = 用户在星球上永远
				//   看不到原版扫描后的绿色。原生色 #27C684 = 绿，与「弹药救援 =
				//   绿」这个分组意图一致，观感变化最小。
				kColorUnset,   // kLootAmmoAid 弹药、救援 —— 原版绿 #27C684（state 5 归还引擎）
				// ★★★ v4.32：**覆盖为紫 #B36BFF**（与「资源」同色 —— 用户需求
				//   `颜色分类.md` 把「笔记 + 资源」并进**紫组**，state 3 与资源
				//   共用；v4.31 的「原版青」不再使用）。
				0x00B36BFF,    // kLootNote    笔记 —— 紫（★ v4.32 与资源同组）
				0x00B36BFF,    // kLootResource 资源 —— 紫（★ v4.22 落在 state 3，不再占 7）
				// ★★★ v4.31：**覆盖为橙 #FF9500** —— 用户要求「恢复容器 / 尸体的橙色」。
				//   state 9 的原生色本来就是橙（#FFAA00 = TargetFullyScanned），这里
				//   沿用 v4.19~v4.29 的橙值（+ noFill ⇒ 轮廓橙）；9 的引擎目标罕见
				//   （城市实况约 3 个元素），代价与「借 9/10 当色槽」同源。
				0x00FF9500,    // kContainer   容器 —— 橙 #FF9500（★ v4.31 恢复）
				// ★★★ v4.24：**不覆盖** —— state 4 是引擎给「已扫描的星球目标（远）」
				//   画绿色的槽位（同上）。设备（终端 / 开关等）现在显示原版绿
				//   #27C684；想恢复青色 ⇒ `ColorDevice=00E5FF`（代价：原版扫描后的
				//   绿色（远目标）会被盖掉，INI 里已注明）。
				kColorUnset,   // kDevice      设备 —— 原版绿 #27C684（state 4 归还引擎）
				// ★★★ v4.31：**覆盖为白 #FFFFFF** —— 用户要求「恢复门白」。
				//   ★ 为什么这次放在 6：**state 6 是引擎全镜像唯一不写的槽位**
				//     （v4.24 订正后的结论）⇒ 覆盖它**对原版零影响**（只有 MOD
				//     挂的门会读它），比 v4.21~v4.29 的「门 = 10」更安全。
				0x00FFFFFFu,   // kDoor        门 —— 白 #FFFFFF（★ v4.31 恢复，挂在 6）
				// ★★★ v4.23：植物 / 矿脉 / 气泉 / 液池 **不覆盖颜色** —— 直接用原版
				//   state 7 的原生配色（脉冲 High `#72E8FF` / Low `#115B69`、基色 alpha=0
				//   = 不填充），也就是原版扫描仪扫「可扫描目标」时那个青色脉冲轮廓。
				//   ★ 必须 unset：只要一覆盖，engine 自己在 state 7/8 上的区分（如果
				//     有）就被抹平 —— 这正是 v4.17~v4.21 那几轮「扫描前后分不出」的来源。
				kColorUnset,   // kFlora       植物 / 矿脉 —— 原版色（state 7 不覆盖）
				kColorUnset,   // kOther       MSTT（默认关）—— 不覆盖（原生蓝）
				0x00FF9500     // kCorpse      尸体 —— 橙 #FF9500（★ v4.31 恢复，= 容器）
			};

			// ★ v4.20：每个类别的「覆盖不透明度」（0~255；**0 = 特殊值 = 保留引擎原值**）。
			//   ★★ v4.21 订正（用户实测「1.7.2 没起作用」）：**alpha 不被渲染消费** ——
			//     用户截图量化（`out/analyze_shot.py`）：被白色覆盖的门区域 41 600 px 的
			//     R/G/B **p5~p95 只差 0~1 个灰阶（stdev=1.1）** ⇒ 画面是**不透明纯色填充**，
			//     102(40%) 与 255(100%) 在画面上**逐像素相同**。
			//   ⇒ 本键现在只作用于**脉冲色（轮廓）**，不再用于「让材质透出来」；
			//     「不填充」改由 `Config::noFill`（基色 alpha=0）负责。
			//   ⇒ 默认全 0（保留引擎原值）—— 与 v4.19 的轮廓观感逐字节一致。
			std::array<std::uint8_t, kCategoryCount> colorAlpha{
				0,    // kLoot        杂项
				0,    // kLootWeapon  武器、投掷物
				0,    // kLootApparel 太空服/背包/头盔/服饰
				0,    // kLootAmmoAid 弹药、救援
				0,    // kLootNote    笔记
				0,    // kLootResource 资源
				0,    // kContainer   容器
				0,    // kDevice      设备
				0,    // kDoor        门
				0,    // kFlora       植物
				0,    // kOther       MSTT（默认关）
				0     // kCorpse      尸体
			};

			// ★★ v4.21：**「不填充」总开关**（本轮定性的落地）。
			// ----------------------------------------------------------------
			// 硬证据（2026-09-25 用户截图，工具 `out/analyze_shot.py`）：
			//   · 门（白色覆盖）41 600 px：R/G/B 的 p5~p95 只差 0~1 个灰阶
			//     （stdev=1.1）⇒ 渲染 = **不透明纯色填充**，alpha 完全没被消费；
			//   · 手枪（红色覆盖）同理（p5~p95 只有 14 个灰阶的抖动）。
			// ⇒ 「覆盖太深、盖住材质」不是 alpha 不够小，而是**引擎在"填充"**。
			//
			// ★ 引擎自己的用法：原版扫描仪扫物品用的 state 7/8
			//   （TargetScannable / TargetScanned）的**描边基色 = 0x00000000（alpha=0）**
			//   —— 即**不填充**，只有脉冲色（轮廓）在画 ⇒ 原版扫描时物品材质看得见。
			//   而 v4.19 为了让颜色「画得出来」把 state 0/1 的基色 alpha 从 0 补成
			//   0xFF ⇒ 亲手打开了「填充」这盏灯（v4.19 起「覆盖太深」的真正来源）。
			//
			// ⇒ v4.21：**基色（+0x80）一律写 `RGB + alpha=0`（不填充）**，
			//   只保留脉冲色（+0x00/+0x20 = 彩色轮廓）⇒ 物品材质透出、颜色仍可辨。
			// ⇒ INI `NoFill=0` 一键退回 v4.19 的「补 0xFF（实心填充）」行为。
			bool noFill = true;

			// ================================================================
			// ★★★ v5.0：**完全自建颜色通道**总开关（默认 1 = 开）
			// ----------------------------------------------------------------
			// 1 = 走自建通道（docs/32）：13 条自建 HighlightManager 各自一份 32 字节
			//     参数；**不写引擎状态表、不覆盖引擎配色块** ⇒ 原版扫描仪 / NPC /
			//     星球目标的颜色 100% 原版；类别配色互不干扰（不再并组 / 让位）。
			// 0 = 完全回到 v4.33 的旧路径（state 覆盖；回退用，一行 INI 切换）。
			// ★ 自建通道不可用时（引擎版本变化导致签名不符 / 模板标定失败）会
			//   **自动回退旧路径**并打 WARN —— 不需要手动改这个键。
			// ================================================================
			bool channelMode = true;
			// 「植物已扫描」通道的颜色（kColorUnset = 内置原版绿 #27C684）。
			// 只影响自建通道模式（旧路径下「已扫描植物」用 StateFloraScanned）。
			std::uint32_t colorFloraScanned = kColorUnset;

			// ★ v4.19：诊断探针 —— 把「渲染侧实际收到的每状态参数块」打进日志
			//   （含**基色**= 真正画出来的颜色）。每个会话最多 3 次、只读、带指针校验。
			//   排「颜色没生效」时非常有用；不想要噪音就写 `RendererProbe=0`。
			bool          rendererProbe   = true;

			// ★★★ v4.24：诊断探针 —— 举着扫描仪约 1.5 秒后，把**11 个 HighlightManager
			//   各自的元素数**打一行（`manager occupancy[举着扫描仪]: 0=.. 1=.. … 10=..`）。
			//   用途：直接看**引擎自己在往哪些 state 写**（本轮就是靠「state 4/5 会涨」
			//   这条实况来验证「已扫描的星球目标 = 绿色」的结论；也用来复盘
			//   「扫描前 / 扫描后 / 正在扫描」到底落在哪几个槽位）。
			//   每个会话最多 6 次、发生在「举起扫描仪」之后 1.5s（那一刻引擎已经写完
			//   至少一轮），全部只读 + 指针校验。不想要噪音就写 `ManagerOccupancyProbe=0`。
			bool          managerOccupancyProbe = true;

			// ★★ v4.17：「资源」判据 = 读 MISC 记录上的 `ResourceType*` 关键词
			//   （离线实证：1319 条 MISC 里 410 条带它 = 资源物品；909 条不带 =
			//     Digipick / Credits / 玩具 / 盆栽 这类真杂物。见 IsResourceBase()）。
			//   设 0 = 完全不读关键词（全部 MISC 都算杂项，即 1.5 的行为），
			//   排查「资源颜色不对」时用；不用换 DLL。
			bool          resourceByKeyword = true;

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
			// ★★ v4.10：展示柜「拿空即灭」（见常量区「v4.10 展示柜拿空即灭」）
			// ================================================================
			// 1 = 用「容器界面的同一段打开期」区分两种「空」：
			//     打开期内读到的空 = 内容被拿光 ⇒ 熄灭（含拿空后关闭的粘性）；
			//     关着 / 从没打开过读到的空 = 内容未知 ⇒ 照常亮。
			//   0 = 退回 v4.9 行为（展示柜读到空一律不熄灭）。
			bool          displayCaseUiEmpty = true;
			// 容器（搜刮）界面的菜单名 —— 默认 `"ContainerMenu"`（游戏自带脚本实证：
			//   `AudioContainerNoAnimScript` / `OutpostContainerScript` 都用它）。
			//   万一实测发现是别的名字（改了菜单名 / 第三方 UI），改这里即可，不用换 DLL。
			char          containerMenuName[64]{ "ContainerMenu" };

			// ================================================================
			// ★★ v4.11：容器界面信号改走「引擎菜单事件」（见常量区「v4.11」长注释）
			// ================================================================
			// 1 = 注册 `RE::UI` 的 MenuOpenCloseEvent sink，用它驱动「容器界面开着吗」
			//     （v4.10 的轮询在实测里全程读不到 ⇒ 必须叠上这条路）；
			//   0 = 只用轮询（回到 v4.10 的行为，仅排查用）。
			bool          containerMenuEvents = true;
			// 1 = 允许「自愈」：看到展示柜投影却还没有界面标志时，把此刻唯一开着的
			//     非白名单菜单名学成容器菜单名（会话级；原版 / 第三方改了菜单名也能恢复）。
			//   0 = 不学习（只写诊断日志）。
			bool          containerMenuLearn = true;
			// 「此刻开着的菜单」快照日志上限（诊断用；只在需要时打，默认 6 条足够定位）。
			int           menuDumpMax = static_cast<int>(kMenuDumpMax);
			// 菜单事件写日志的上限（普通事件；容器菜单相关的事件不限，因为它们很少）。
			int           menuEventLogMax = static_cast<int>(kMenuEventLogMaxDefault);
			// 展示柜「投影出现 / 消失」追踪日志上限（诊断；默认 24 条）。
			int           displayCaseTraceMax = static_cast<int>(kDisplayCaseTraceMaxDefault);
			// 1 = 展示柜**逐帧**观察（把「拿空 ⇒ 立刻关掉」的窗口从 200ms 缩到 1 帧）；
			//   0 = 只在 200ms 扫描节拍上判（v4.10 的行为，仅排查用）。
			bool          displayCaseFrameWatch = true;

			// ================================================================
			// ★★ v4.13：改用**引擎自己的游戏事件**（见常量区「v4.13」长注释）
			// ================================================================
			// 为什么换：实测证明搜刮面板**不是 UI 菜单**（投影出现那一刻没有任何
			//   ContainerMenu 事件、IsMenuOpen 快照里只有 HUD/HUDMessages），
			//   所以 v4.10 的轮询与 v4.11/v4.12 的菜单事件**两条路都拿不到它**。
			// 1 = 注册两个引擎事件 sink：
			//     · TESContainerChangedEvent  —— 物品进出容器（**精确记账**的输入）
			//     · QuickContainerOpenedEvent —— 快速搜刮面板打开（per-ref）
			//   0 = 不注册（退回 v4.12 行为，仅排查用）。
			bool          containerLootEvents = true;
			// 1 = 允许「兜底判据」：快速面板确认为这个 ref 开着、这一段打开期里读到过
			//     内容、且**一次拿走事件都没收到**、且连续读到空 ≥ 400ms ⇒ 判「拿空」。
			//   0 = 只信记账（默认，v4.14 起）。
			//   ★★ v4.14 默认改成 0（关）——实测 + 推导发现兜底有**两个误灭面**：
			//      ① 面板刚打开、投影还没建立的那 0.4~0.7 秒（关闭状态库存本来就空）
			//         ⇒ 「连续读到空」直接成立 ⇒ 误判（2026-09-19 14:25:06 的日志实证，
			//         那一刻箱子里明明有 2 件东西；本版已用 `sawContentInSession` 加严）；
			//      ② 「打开看一眼、不拿、关掉」——关掉后投影消失、库存又读空，
			//         而「面板关闭」这件事**没有任何信号**（拿空与关闭在数据上同形，
			//         v4.9 就论证过）⇒ 400ms 后必然误判（本版无法修，只能默认关）。
			//   ★ 取舍：记账（TESContainerChangedEvent）已实测能覆盖真实拿空
			//      （2026-09-19 那局 2 条 take 精确减账到 0）；「不走物品事件的展示柜」
			//      目前没有任何实测证据 ⇒ 「宁少灭、不误灭」（项目红线）。
			//      真要开回来：INI 设 1（会重新带上 ② 的误灭风险）。
			bool          displayCaseQuickOpenEmpty = false;
			// 物品事件 / 快速面板事件的日志上限（诊断；默认 32 条）。
			int           lootEventLogMax = 32;

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
			// ★★★ 2026-09-27（订正 R3，性能）：环内 cell 的**分片遍历**（治「帧数下降」）。
			//   背景：环（kRingRefsCap = 60000）里各 cell 的引用原来是**每轮全量遍历**的
			//   （纯内存读），6 万 × ~0.9µs ≈ 54ms/轮 ⇒ 用户当轮日志 `loop avg=55ms`。
			//   分片 = 每个环内 cell 的数组均分成 ≤ N 片、每轮只遍历一片（游标轮转）
			//   ⇒ 每轮环内遍历量 ≈ 环总量 / N（默认 6 万/5 ≈ 1.2 万，~10ms 量级）。
			//   0 / 1 = 关（每轮全扫，退回旧行为，仅用于对照）；默认见 kRingSliceMaxRounds。
			//   ★ 安全性：读取时按「N × ScanIntervalMs < UnhighlightGraceMs」钳出
			//     `ringSliceRoundsEff`（防「本轮没扫到 ⇒ 宽限期摘掉 ⇒ 下片挂回」的闪烁），
			//     Rescan 只认这个有效值 —— 见 LoadConfig 与启动日志那一行。
			int           ringSliceMaxRounds = static_cast<int>(kRingSliceMaxRounds);
			std::uint32_t ringSliceRoundsEff = kRingSliceMaxRounds;  // 钳制后的有效片数（≥1）

			// ★★★ 订正 R4（性能）：两把削减「每轮扫描里最贵那一块」的开关。
			//   背景（用户 2026-09-27：「静态场景帧数正常，动态场景（战斗 / 走动时扫到
			//   新的高亮物品）卡顿」）：同一局日志里 loop 的耗时**与遍历量无关**
			//   （08:37 静止：1538 引用 → 7ms；08:47 走动：1545 引用 → 63ms），
			//   也与判空次数无关（两边都在 30 次/轮 量级）⇒ 差的是**单次操作的墙钟成本**。
			//   这条路径上唯一会「进制内核」的操作就是 SafeReadMem（ReadProcessMemory）
			//   与 IsReadable（VirtualQuery）—— 两者都要拿**进程地址空间锁**；
			//   游戏流式加载（走动 / 战斗 = 大量 commit / decommit）时那把锁被抢，
			//   单次调用可以从 ~10µs 涨到 ~300µs（按日志反推），每轮几百次 ⇒ 几十毫秒。
			//   ⇒ ① `fastReadMem`：改成 SEH 兜底 + 直接读（正常内存零内核调用）；
			//      ② `lootCacheTtlMs`：容器 / 尸体判空结果按引用缓存（见下）。
			bool          fastReadMem    = true;
			// 判空缓存 TTL（毫秒）。**权威的失效信号是事件**（拿 / 放物品的
			// TESContainerChangedEvent、容器界面开关）—— 它们一发生就整体作废缓存；
			// TTL 只是兜住「没有事件的变化」（比如别的角色 / 脚本动了容器内容）。
			// 0 = 关（每轮实时判空，退回 v5.1-R3 行为）。
			int           lootCacheTtlMs = 1500;
			// ★★★ v4.29：放下扫描仪后的「恢复提速」窗口（治「植物 / 矿石比其它物品慢一拍」）。
			//   resyncBoostMs = 提速窗口时长（0 = 关掉，退回旧行为）；
			//   resyncBoostBudget = 窗口内每轮重挂预算（0 = 关掉，用 MaxOutlineOpsPerScan）。
			//   完整背景见常量区 kResyncBoostMs 的说明。
			std::uint64_t resyncBoostMs     = kResyncBoostMs;
			std::uint32_t resyncBoostBudget = kResyncBoostBudget;
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
			// ★★★ v4.25：这条高亮属于哪个类别（Category）。
			//   为什么需要它：v4.23 的「举着扫描仪时让位原版」是按**状态**判断的
			//   （`state == StateFlora`），而 v4.25 起「植物」类别会在
			//   StateFlora（未扫描，青）与 StateFloraScanned（已扫描，绿）之间切换 ——
			//   按状态判断就会漏掉「已扫描」那一半（它用的 state 5 正是引擎此刻在用的
			//   槽位，漏保护 = 放下扫描仪后 MOD 会把引擎刚挂的那条摘掉）。
			//   ⇒ 改成按类别判断，与状态取值彻底解耦。
			std::uint8_t                     cat        = 0xFF;
			// ★★★ v5.0：自建颜色通道号（ChannelMode=1 时用；见 docs/32）。
			//   0..kCategoryCount-1 = 类别通道；kCategoryCount = 「植物已扫描」通道。
			//   通道模式下 `state` 不再参与挂载/比较/摘除（那三项都看 channel），
			//   只保留给「让位 / 日志」等旧语义。
			std::uint32_t                    channel    = 0xFFFFFFFFu;
		};

		// ================================================================
		// ★★★ v5.1.5（订正 R5）：「按物种（base）扩散表」的**作用域** —— 见证时所在的
		//   **世界空间（星球 / 内景）**（定义在 State 之前，因为 State 里有它的容器）。
		// ----------------------------------------------------------------
		// 为什么必须有这一层：v5.1.2 的扩散表只按 base 记（**全局**），而用户实测的假绿
		//   正是**跨星球**那种 ——「未扫描星球资源直接显示绿色问题还存在」（v5.1 之前的
		//   base 级引用记忆也踩过同一个坑，docs/33 的结论）。
		//   ⇒ 扩散限制在**同一颗星球**：
		//     · 见证时记下当时的 worldspace（`TESObjectREFR::GetParentWorldSpace()`，
		//       指针只在会话内有意义 —— 表本来就不落盘）；
		//     · 命中时要求当前引用的 worldspace ∈ 列表；
		//     · `count == 0`（见证时读不到）/ `a_ws == 0`（当前引用读不到）⇒ **不收紧**
		//       （宁可保留旧行为，也不因为读不到就误判成假绿）。
		// ================================================================
		struct FloraBaseScope
		{
			std::array<std::uintptr_t, 4> worldspaces{};  // 见证时的 worldspace（最多 4 个）
			std::uint32_t                 count = 0;
			// ★★★ v5.1.7（订正 R7）：**最早**一次见证的游戏时间（天；0 = 未知）。
			//   读档时用它剪枝：「见证时刻 > 本存档的游戏时间」⇒ 这个物种的绿属于更新的
			//   时间线 ⇒ 不在本存档作用域（见 TryProcessFloraSaveLoadByTime）。
			float                         days  = 0.0f;

			[[nodiscard]] bool Seen(std::uintptr_t a_ws) const
			{
				for (std::uint32_t i = 0; i < count && i < worldspaces.size(); ++i) {
					if (worldspaces[i] == a_ws) {
						return true;
					}
				}
				return false;
			}
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
			// ★★★ v4.29：resync 后的「恢复提速」截止时刻 —— 在这个窗口内
			//   SyncNativeOutline 用 `ResyncBoostBudget`（默认 192）条/轮重挂，
			//   免得 268 个目标按 64 条/轮要走 5 轮 ≈ 1 秒（用户感知：
			//   「植物和矿石的高亮速度明显低于其他物品」）。见常量区 kResyncBoostMs。
			std::uint64_t resyncBoostUntilMs = 0;

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
			// ★★ v4.17：「资源」关键词判据的启动自检结果
			//   0 = 还没定论（自检继续尝试；**判据此刻已经生效** —— 见 IsResourceBase）
			//   1 = 通过（有正样本命中）  2 = 否决（负样本命中 / 正样本全不命中）
			std::uint8_t  resKeywordTest  = 0;
			// ★ v4.18：自检的「未就绪」诊断 —— v4.17 是静默重试（日志里什么都看不到，
			//   实测 `resource=0` 无从定位）。现在每 5 秒最多一条 WARN、最多 6 条。
			std::uint32_t resKeywordTries    = 0;
			std::uint32_t resKeywordWarns    = 0;
			std::uint64_t resKeywordWarnAtMs = 0;
			// ★ v4.22：自检节流（一圈会遍历 1~8 个 cell ⇒ 每个都调一次太亏）+ 首个命中诊断。
			std::uint64_t resKeywordNextTryMs  = 0;
			std::uint32_t resKeywordFirstHit   = 0;  // 世界里首个被判成「资源」的 base FormID
			// ★★★ v4.23：关键词数组的**偏移标定**（0 = 还没定）—— 见 IsResourceBaseRaw。
			//   一旦从某个候选偏移读出「形状合格且非空」的数组就采纳它（会话级缓存），
			//   之后热路径只读这一个偏移；日志里 `关键词数组标定 = base+0x…` 一行可核对。
			std::size_t   resKwOff             = 0;
			// ★ v4.23：`misc kw probe:` 探针额度（每会话 ≤ kMiscKwProbeMax 条）——
			//   5 个候选偏移都不像关键词数组时打出原始读数（偏移/布局又不对的唯一证据）。
			std::uint32_t miscKwProbes         = 0;

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
			// ★ v4.10：容器（搜刮）界面状态 —— 每帧更新（见 Tick）。
			//   containerUiSerial：每次「开 ↔ 关」翻转 +1（= 段号）。展示柜的
			//   「拿空」判据靠它把「同一段打开期」与「上一段（已关闭）」严格分开
			//   （完整推导见常量区「v4.10 展示柜拿空即灭」）。
			bool          containerUiOpen   = false;  // 当前 ContainerMenu 是否打开
			std::uint64_t containerUiSerial = 0;      // 段号（每次开/关翻转 +1）
			std::uint64_t containerUiOpens  = 0;      // 打开次数（诊断）
			//   DisplayCaseVerdict：每个展示柜引用一份判决缓存（见常量区 v4.10）：
			//     lastKnown    —— 0=未知（照常亮） 1=有东西 2=已确认拿空（粘性熄灭）
			//     lastTpSerial —— 最后一次见到 `kTemporary` 投影条目时的 UI 段号
			//   displayCaseEmptied：窗口内「展示柜拿空即灭」的轮数（诊断：
			//     非 0 且增长 = 新规则在生效）。
			//
			//   ★★ v4.13：这里的字段被「游戏事件通道」复用/扩展（见常量区「v4.13」）：
			//     · 内容快照 snap[]（逐帧/每轮读到投影时刷新）= **记账的基准**
			//       —— 收到「从这个 ref 拿走 base×N」就减账，减到 0 ⇒ 判拿空；
			//     · quickOpenMs：「快速搜刮面板为这个 ref 打开」的时刻（0 = 没有）；
			//     · sawTakeEvt：这一段打开期里收到过拿走事件没有 —— 有 ⇒ 只信记账
			//       （兜底判据自动让位，避免「拿了部分就立刻关掉」被误判）；
			//     · emptySinceMs：「连续读到空」的起点（兜底判据要求 ≥ 400ms）。
			struct DcSnap
			{
				std::uint32_t base[kDcSnapMax]{};
				std::uint32_t cnt[kDcSnapMax]{};
				std::uint8_t  count     = 0;
				bool          valid     = false;  // 这一份是不是「完整读过一遍」
				bool          uncertain = false;  // 有过认不出的东西 / 减成负数 ⇒ 不许下「拿空」结论
			};
			struct DisplayCaseVerdict
			{
				std::uint8_t  lastKnown    = 0;
				std::uint64_t lastTpSerial = 0;
				// ★ v4.13
				DcSnap        snap{};
				std::uint64_t quickOpenMs      = 0;
				bool          sawTakeEvt       = false;
				std::uint64_t emptySinceMs     = 0;
				std::uint32_t evtLogs          = 0;   // 该 ref 已写的事件日志条数（限流）
				// ★ v4.14：**这一段打开期里读到过内容**（打开时清零、读到「有东西」时置位）
				//   —— 兜底判据必须要有它：否则「面板刚打开、投影还没建立」的那一瞬
				//   （关闭状态库存本来就空）会被当成「连续读到空」⇒ 误判拿空。
				//   实测证据见常量区「v4.13」尾部的 v4.14 段（2026-09-19 那一局的
				//   `display case emptied (兜底…)` 就是它误判出来的）。
				bool          sawContentInSession = false;
			};
			std::unordered_map<const RE::TESObjectREFR*, DisplayCaseVerdict> displayCaseVerdict;
			std::uint64_t displayCaseEmptied = 0;
			// ★ v4.13：事件通道的开关 / 注册状态（sink 由 EnginesLootEventSinks 挂）
			bool          lootEvtActive       = false;  // 是否解释物品事件
			bool          quickOpenActive     = false;  // 是否解释快速面板事件
			bool          invEvtRegistered    = false;
			bool          quickOpenRegistered = false;
			std::uint64_t lootEvtRetryAtMs    = 0;
			std::uint64_t lootEvtSinkCheckMs  = 0;
			std::uint32_t lootEvtSinkFailures = 0;
			// ★ v4.13：诊断计数（「事件通道到底有没有在工作」一眼可辨）
			std::uint64_t lootEvtMatches       = 0;  // 事件命中逐帧观察表的条数
			std::uint64_t lootEvtTakeApplied   = 0;  // 真正减过账的次数
			std::uint64_t lootEvtPutApplied    = 0;  // 真正加过账的次数
			std::uint64_t lootEvtEmptyDecided  = 0;  // 记账判出「拿空」的次数（核心指标）
			std::uint64_t lootEvtUnmatched     = 0;  // 没命中观察表的事件数（诊断）
			std::uint64_t quickOpenMatched     = 0;  // 快速面板事件命中观察表的次数
			std::uint64_t quickOpenUnmatched   = 0;  // 快速面板事件没命中观察表的次数
			std::uint64_t quickOpenFallbackDecided = 0;  // 兜底判据判出「拿空」的次数
			std::uint64_t quickOpenLastMs      = 0;  // 最近一次「快速面板打开」（任何 ref）
			std::uint32_t quickOpenLastFid     = 0;  // 它的 FormID（诊断）
			std::uint32_t lootEvtLogs          = 0;  // 事件日志已写条数（上限 cfg.lootEventLogMax）

			// ================================================================
			// ★★ v4.11：容器界面「事件通道」+ 展示柜逐帧观察（见常量区「v4.11」）
			// ----------------------------------------------------------------
			// 事件通道（`menuEvt*`）由**引擎的菜单事件回调**写 —— 回调线程不保证
			// 与主线程相同 ⇒ 名字集合用锁保护，计数用原子；主线程只读。
			// ================================================================
			std::mutex    menuEvtLock;
			int           menuEvtOpenCount = 0;                      // 事件侧「此刻开着」的菜单数
			char          menuEvtOpenNames[kMenuEvtOpenListMax][kMenuEvtNameMax]{};
			char          menuEvtLastOpen[kMenuEvtNameMax]{};       // 最近一次「打开」的菜单名
			std::uint64_t menuEvtLastOpenMs   = 0;                   // 它的时间戳（自动学习用）
			std::uint64_t menuEvtContainerLog = 0;                   // 已写日志的容器相关事件数
			std::atomic<std::uint64_t> menuEvtTotal{ 0 };            // 收到的全部菜单事件数
			std::atomic<std::uint64_t> menuEvtContainer{ 0 };        // 其中「容器菜单」相关的条数
			std::atomic<std::uint64_t> menuEvtBad{ 0 };              // 负载不像菜单事件的条数（安全阀）
			std::atomic<bool>          menuEvtOpen{ false };         // 事件侧：容器菜单此刻开着吗
			bool          menuSinkRegistered = false;                // sink 注册成功了没有
			// ★ v4.12：事件负载解释开关。坏负载到阈值时**只关掉解释**（sink 仍然挂着，
			//   `ProcessEvent` 直接返回）—— 不去调引擎的 `UnregisterSink`（它的 ID 映射
			//   与 `RegisterSink` 不是同一形状，且在回调里注销有重入风险，见实现处注释）。
			bool          menuEvtActive      = false;
			std::uint32_t menuSinkFailures   = 0;                    // 注册失败次数（诊断）
			std::uint64_t menuSinkRetryAtMs  = 0;                    // 失败后下次重试的时间
			std::uint64_t menuSinkCheckMs    = 0;                    // 上次核对「sink 还在不在」的时间
			std::uint32_t menuFingerprints   = 0;                    // UI 布局指纹已打几次（上限 2）
			bool          menuEvtCrossChecked = false;               // 首条事件的交叉校验做过没有
			std::uint64_t menuEvtLogged      = 0;                    // 已写日志的普通事件数
			std::uint32_t menuDumps          = 0;                    // 「此刻开着的菜单」快照已打几次
			// ★ v4.12：按名字统计「哪些菜单在开关」（每个名字最多记 kMenuEvtPerNameLogMax 条）
			struct MenuEvtNameStat
			{
				char          name[kMenuEvtNameMax]{};
				std::uint32_t count = 0;  // 这个名字一共来了几条事件
			};
			std::array<MenuEvtNameStat, kMenuEvtNameStatMax> menuEvtNames{};
			std::size_t   menuEvtNameCount = 0;
			// ★ v4.12：最近几条事件（名字 + 开/关 + 时间戳），取证行里带上
			struct MenuEvtRecent
			{
				char          name[kMenuEvtNameMax]{};
				bool          opening = false;
				std::uint64_t ms      = 0;
			};
			std::array<MenuEvtRecent, kMenuEvtRecentMax> menuEvtRecent{};
			std::size_t   menuEvtRecentCount = 0;
			// 会话级学习的「容器菜单」候选名（自动学习，见常量区「v4.11」自愈那条）
			std::array<std::string, kMenuLearnMax> containerMenuLearned{};
			std::size_t   containerMenuLearnedCount = 0;
			std::uint64_t containerMenuLearnedEvents = 0;
			// 展示柜**逐帧**观察表：只放「半径内、近距离的展示柜」，
			//   每帧读一次它的库存 ⇒ 把「拿空 ⇒ 立刻关掉」的观测窗口从 200ms 缩到 1 帧。
			struct DcWatch
			{
				const RE::TESObjectREFR* ref      = nullptr;
				std::uint32_t           miss      = 0;  // 连续形状不对的帧数（到上限就踢）
				int                     lastLoot  = -2;  // 上一帧的 loot（-2 = 还没有基线）
				bool                    lastTp    = false;  // 上一帧见到投影条目没有
			};
			std::array<DcWatch, kDcWatchMax> dcWatch{};
			std::size_t   dcWatchCount        = 0;
			std::uint64_t dcWatchFrames       = 0;  // 逐帧观察跑了多少帧（诊断）
			std::uint64_t displayCaseEmptiedFrame = 0;  // 其中「逐帧判出拿空」的次数（诊断）
			std::uint32_t dcTraceCount        = 0;  // 投影出现/消失追踪日志已写条数
			std::uint64_t dcUiProbeLastMs     = 0;  // 「菜单快照 + 自愈」上次跑的时间（限流 1 秒）
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
			//   ★★★ 订正 R4：**单位从 ms 改成 µs**（见下面的长注释）。
			std::uint64_t scanMsTotal     = 0;  // 窗口内 Sum(每轮 Rescan 耗时，µs)
			std::uint64_t scanMsMax       = 0;  // 窗口内最大单轮耗时（这才是卡顿的感觉来源，µs）
			std::uint32_t scanMsSamples   = 0;  // 窗口内 Rescan 调用次数（**含**早退的那些）
			// ★★★ 订正 R4：真正跑完遍历的轮数 —— 所有分段平均都必须除以它。
			//   原来一律除以 scanMsSamples（含「形状没过 / 还在等稳定 / 静置期」等早退轮），
			//   早退轮的分段耗时是 0 ⇒ 把平均值**稀释**了（诊断时会低估真实开销）。
			std::uint32_t scanFullSamples = 0;
			std::uint32_t scanRefsLast    = 0;  // 最近一轮实际遍历的引用数（诊断）
			std::uint64_t opsThisScan     = 0;  // 最近一轮实际发出的引擎调用数
			std::uint64_t opsDeferred     = 0;  // 因为预算不够而推迟到下一轮的调用数
			bool          loadingNow      = false;

			// --- ★ v2.3：把 Rescan 拆成几段分别计时（窗口内求和 / 求最大）---
			//   没有这一行就只能看到「scan avg=32ms」而不知道 32ms 花在哪。
			//   · shape = 读 cell + 引用数组形状校验（VirtualQuery 集中在这一段，看 vq 次数）
			//   · loop  = 遍历全部引用挑目标（纯内存读）
			//   · sync  = 挂/摘（SyncNativeOutline，引擎调用集中在这一段）
			//   · unh / add = 其中 UnoutlineRef / OutlineRef 各占多少
			std::uint64_t tShapeUs  = 0;
			std::uint64_t tShapeMaxUs = 0;
			std::uint64_t tLoopUs   = 0;
			std::uint64_t tLoopMaxUs = 0;
			std::uint64_t tSyncUs   = 0;
			std::uint64_t tSyncMaxUs = 0;
			std::uint64_t tUnhUs    = 0;
			std::uint64_t tAddUs    = 0;
			std::uint64_t vqCalls   = 0;

			// --- ★★★ 2026-09-27（订正 R3）：把 loop / sync 再拆一层 —— 定位「帧数仍有下降」---
			//   用户报告「帧数还是有下降现象」；当轮日志的形态是：
			//     timing: scan avg=79ms（每 200ms 一轮 ⇒ 主线程占用 ~40%）
			//     timing2: loop avg=55ms（大头）| sync avg=14ms | shape ~0ms
			//   而 loop 里只有「遍历引用 + 分类 + 距离」（纯内存读）—— 唯一能解释 55ms 的
			//   就是**遍历量**（`kRingRefsCap = 60000`：当前 cell + 环内 cell 合计可达 6 万引用，
			//   按 ~0.9µs/引用 正好是 50ms 量级）。所以这一层拆出来回答两个问题：
			//     ① refsWalkCur / refsWalkRing —— 每轮到底遍历了多少引用（分开统计）；
			//     ② tWalk / tLoot / tFlora / t3D —— 时间分别花在哪一段。
			//      · tWalk    = loop 内「遍历引用 + 分类 + 距离」整段时间（**含**下面几个子项）
			//      · tLoot    = 其中 RefLootState（容器 / 尸体判空）
			//      · tFlora   = 其中 FloraTargetScanned / ProbeFloraEngineState（植物 / 矿脉判据）
			//      · t3D      = sync 内 3D 复检（RefGet3D × Verify3DPerScan）
			//   ★★★ 订正 R4：单位 = **µs**，并新增 tClassify / tProbe / 卡顿检测 ——
			//     因为 R3 的 ms 计时**按次取整**（`NowMs() - t0` 每次 < 1ms 就记 0），
			//     而判空 / 植物 / 分类都是「一次几百 µs × 几十次」的形态 ⇒ 原来的
			//     `loot=0ms flora=2ms` 是**假象**（真实开销全被抹平到 walk 里）。
			//     R4 起全部用 QueryPerformanceCounter 的 µs 计时。
			std::uint64_t tWalkUs      = 0;
			std::uint64_t tLootUs      = 0;
			std::uint64_t tFloraUs     = 0;
			std::uint64_t tClassifyUs  = 0;  // ClassifyRef（含 ACHR 判决探针 / 分类链）
			std::uint64_t tProbeUs     = 0;  // ContProbe / DcWatchAdd / LootProbe（诊断探针）
			std::uint64_t t3DUs        = 0;
			std::uint64_t refsWalkCur  = 0;  // 窗口内遍历的「当前 cell」引用数（求和）
			std::uint64_t refsWalkRing = 0;  // 窗口内遍历的「环内 cell」引用数（求和）
			std::uint64_t ringSliceDeferred = 0;  // 窗口内因分片而推到下轮的引用数（诊断）

			// --- ★★★ 订正 R4：卡顿检测（遍历内部的「空档」）---
			//   每 64 个引用取一次时刻；相邻两次之间的间隔 ≥ kStallThresholdUs 记一次卡顿。
			//   用途：把「遍历本身慢」与「主线程被抢 / 内存卡住」分开 ——
			//   若 `卡顿合计` 占了 walk 的大头，就不是我们的指令慢，而是调度 / 内存。
			std::uint64_t stallCheckUs = 0;
			std::uint32_t stallCount   = 0;
			std::uint64_t stallUs      = 0;
			std::uint64_t stallMaxUs   = 0;
			// 调用计数（窗口内求和；每次统计日志清零）—— 「耗时」必须配「分母」
			std::uint32_t cntLoot      = 0;  // RefLootState 真实执行次数（不含缓存命中）
			std::uint32_t cntLootMemo  = 0;  // 判空结果来自缓存（省掉）
			std::uint32_t cntFlora     = 0;  // FloraTargetScanned 次数
			std::uint32_t cntClassify  = 0;  // ClassifyRef 次数
			std::uint32_t cntProbe     = 0;  // 诊断探针次数
			// 直读撞异常 / 走内核读的**上一次快照**（与全局计数相减得到窗口内增量）
			std::uint64_t sehFaults    = 0;
			std::uint64_t kernelReads  = 0;
			// 最慢一轮的快照（µs；供 timing3 行打印 —— 卡顿是「最慢那几轮」造成的）
			std::uint64_t worstScanUs   = 0;
			std::uint64_t worstScanRefsSum = 0;  // 完成轮遍历量的和（算「每轮平均引用数」用）
			std::uint32_t worstRefs     = 0;
			std::uint64_t worstWalkUs   = 0;
			std::uint64_t worstLootUs   = 0;
			std::uint64_t worstFloraUs  = 0;
			std::uint64_t worstClassifyUs = 0;
			std::uint64_t worstStallUs  = 0;
			std::uint32_t worstStalls   = 0;
			std::uint64_t worstCpuUs    = 0;  // 该轮的**线程 CPU 时间**（与墙钟对比 ⇒ 被抢了多少）

			// --- ★★★ 订正 R4：容器 / 尸体「判空结果」按引用缓存 ---
			//   见 Config::lootCacheTtlMs 的长注释（来源 = 事件失效 + TTL 兜底）。
			struct LootMemoRec
			{
				std::uint64_t atMs   = 0;
				std::uint32_t serial = 0;  // 与 lootMemoSerial 不一致 = 已被事件作废
				int           loot   = -3;
			};
			std::unordered_map<const RE::TESObjectREFR*, LootMemoRec> lootMemo;
			std::uint32_t lootMemoSerial = 0;

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
				// ★★★ 订正 R3：分片游标 —— 环内 cell 每轮只遍历「一片」（见
				//   Config::ringSliceMaxRounds 与 Rescan 里的分片逻辑）。
				//   0 = 下一片从数组头开始；推进到 >= refs 时归 0。
				std::uint32_t      sliceCursor = 0;
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
			// ★★★ v4.23：举着原版扫描仪 ⇒ 「星球扫描目标」类别让位（YieldTargets-
			//   WhileScanning，见 Config）。这个计数**只在让位窗口里、且本轮确实
			//   跳过了该类的候选**时才增长 —— 用它确认「让位」真的只在扫描时发生。
			std::uint64_t skipYield     = 0;

			// --- ★★★ v4.25：星球目标「已扫描」判据（见常量区 kRvaIsResourceScanned）---
			//   缓存：**引用指针 → 「这个引用已扫描」**（带 TTL；放下扫描仪 / 换场景
			//   时整体作废 —— 见 InvalidateFloraScannedCache 的调用点）。
			//   ★ v4.28：键从「base FormID」改成**引用** —— 主判据
			//     （`GetOutlineState(ref)`）本来就是按引用回答的；结构里带上 base FormID，
			//     指针被回收去装别的 base 时当**缓存未命中**（重新问一次），不会串色。
			struct FloraScanRec
			{
				bool          scanned  = false;
				std::uint64_t atMs     = 0;
				std::uint32_t baseFid  = 0;  // 该引用挂的 base（校验指针复用）
				std::uint8_t  byEngine = 0;  // 1 = 引擎状态判据 / 2 = 单向学习表（★ v4.31）
			};
			std::unordered_map<const RE::TESObjectREFR*, FloraScanRec> floraScannedCache;
			std::uint64_t floraScanQueries    = 0;  // 真正问过引擎几次（累计，诊断）
			std::uint64_t floraScanHits       = 0;  // 其中「已扫描」（累计，诊断）
			std::uint64_t floraScanShapeFails = 0;  // 链的形状校验没过几次（累计，诊断）
			// ★★★ v5.2：引擎扫描进度直读（抛弃记忆后的植物主判据）——
			//   按 **base（物种）** 缓存：进度是**物种级**数据（引擎口径 —— 扫一个实例
			//   ⇒ 该物种 percent 到 100），同 species 的实例共享一条记录 ⇒ 天然实现
			//   「扫一个实例 ⇒ 同 species 全绿」，数据源是引擎而不是我们的记忆；
			//   读档 / 换场景时随判据缓存一起清（进度是**存档级**数据）。
			struct FloraProgressRec
			{
				std::uint8_t  progress = 0;      // 引擎给的进度（0..100）
				std::uint64_t atMs     = 0;      // 查询时刻（TTL 用）
				bool          ok       = false;  // 链路走通（false = 查询失败，TTL 较短）
			};
			std::unordered_map<std::uint32_t, FloraProgressRec> floraProgressByBase;
			std::uint64_t floraProgQueries   = 0;  // 真正调用引擎链几次（诊断）
			std::uint64_t floraProgFull      = 0;  // 其中查到 100（= 已扫描）
			std::uint64_t floraProgPartial   = 0;  // 其中查到 <100（未满）
			std::uint64_t floraProgFails     = 0;  // 查询失败（回退到旧判据）
			std::uint64_t floraProgCacheHits = 0;  // 缓存命中（省掉的引擎链调用）
			std::uint64_t floraProgConflict  = 0;  // 与「引擎状态表 4/5」相矛盾的次数（上限内打 WARN）
			// ★★★ 订正 R10：key 类型 word 是**现读**的 ⇒ 这两条是「现读到底读到了什么」
			//   的直接证据（`keyZero=` 一直涨且 `满=` 不涨 = 类型 word 还没被引擎初始化 /
			//   我们读错了地址）。
			std::uint64_t floraProgKeyZero   = 0;  // 现读到 0 的次数（0 也可能是合法类型）
			// ★★★ 订正 R11：key1 走兜底（0x910690）成功拿到的次数 —— 实测植物 ref 没有
			//   0x81 组件 ⇒ 这个数**在涨**才说明兜底在干活（它不涨而失败不涨 = 没走到）。
			std::uint64_t floraProgK1Fallback = 0;
			//   ★★★ 订正 R12（诊断）：probe 不再只打「前 8 条」——额外保证
			//     **每个 stage 的第一条**与**第一条成功**都能看到（见 QueryFloraScanProgressCached）。
			//     R11 两局日志的实测教训：前 8 条 probe 恰好全是同一个 stage（=8），
			//     而 `满=35` 的成功样例一条都没留下。
			std::uint32_t floraProgProbes    = 0;  // 已打的 `flora progress probe:` 条数（前 N 条额度）
			std::uint32_t floraProgStageSeen = 0;  // stage 位图：某个 stage 是否已经打过一条
			bool          floraProgSawOk     = false;  // 是否已经打过「查询成功」的一行
			bool          floraProgConflictWarned = false;  // 「直读=未满 vs 引擎表=绿」冲突的首条 WARN
			std::uint32_t floraScanProbes     = 0;  // 本会话已打的 `flora scan:` 最终行数
			//   ★★★ 订正 R12：**判绿行**用独立额度 —— R11 两局日志里 8 条额度全被
			//     会话开头的「判青最终行」吃光 ⇒ 判绿行（含 R11 的核心验收点
			//     `判据 = 引擎扫描进度直读`）结构上不可能出现在日志里（即使判据在干活）。
			std::uint32_t floraScanGreenProbes = 0;  // 判绿行（直读/记忆/扩散/捡漏/GetOutlineState）额度
			std::uint32_t floraScanDumps      = 0;  // ★ v4.26：已打的「指针窗口」取证条数
			std::uint64_t floraScanShapeWarnAtMs = 0;
			// ★★★ v4.28：`GetOutlineState(ref)` 主判据的计数（诊断 —— 一眼看它是不是在干活）
			std::uint64_t floraEngineStateQueries   = 0;  // 问过几次
			std::uint64_t floraEngineStateScanned   = 0;  // 其中 = 2（已扫描）
			std::uint64_t floraEngineStateUnscanned = 0;  // 其中 = 1（未扫描）
			// ★★★ v5.1.1：其中 = 0 / 非 1 非 2（引擎「不知道」）—— 实测占 ~20%，
			//   这一档以前在日志里看不见，而它正是「扫过了却说没扫」的那一批。
			std::uint64_t floraEngineStateUnknown   = 0;
			// ★★★ v4.31：「低概率变青」修复的两条诊断（都应该在涨 = 修复在干活）
			std::uint64_t floraLearnedHits   = 0;  // 判据由「单向学习表」直接命中（没重问引擎）
			std::uint64_t floraStickyKeeps   = 0;  // 重问拿不到权威答案 ⇒ 沿用旧结论（保住绿）
			// 窗口内：本轮选中的星球目标里，分别有多少个走「已扫描（绿）」/「未扫描（青）」
			std::uint64_t floraScannedSel   = 0;
			std::uint64_t floraUnscannedSel = 0;
			// ★★★ v4.26 / v5.1（本次修复）：记忆的**粒度**从 base 改成**引用**。
			//   v4.26~v5.0 记的是 base（物种 / 资源）：「引擎亲手画过 4/5」或
			//   「资源进了勘测数据」都写成「这个 **base** 已扫描」⇒ 一旦某个植物 /
			//   矿脉实例被扫过，**同 base 的所有实例、跨星球、跨存档**全都变绿 ——
			//   用户实测报告：「矿石、植物扫描前后颜色都是绿色」（docs/33）。
			//   引擎自己的 `GetOutlineState(ref)` 本来就是**按引用**回答的
			//   （v4.28 实证：ScannableComponent + canonical id + 玩家知识库），
			//   所以「已扫描」也必须按引用记；base 只当**校验**用。
			//
			//   条目结构：引用 FormID → { base FormID（校验）, 是否绿 }。
			//   · 写入（`RememberFloraRef`）只认三个**权威**来源：引擎状态 == 2 /
			//     引擎亲手画过 4-5 / 资源链命中；
			//   · 「绿」是**单向**的（不会被后来的「未扫描」翻案 —— 见 v4.31~v4.33
			//     修的「低概率变青」）；「青」可以被后来的「绿」覆盖；
			//   · 命中后这个引用**不再重问引擎**（这就是防「变青」的机制）。
			struct FloraRefKnow
			{
				std::uint32_t baseFid = 0;      // 校验：记下时的 base（FormID 被回收 / 跨存档 ⇒ 失效）
				bool          green   = false;  // true = 已扫描（绿）
				// ★★★ v5.1.5（订正 R5）：这条结论**见证于第几次读档之后**。
				//   0 = 本会话启动时从落盘文件读回（只在本会话的第一次读档前/后有效）；
				//   N = 第 N 次读档之后当场见证（只在那一次读档之后有效）。
				//   「已扫描」是**存档里的勘测数据**决定的 ⇒ 读档 = 存档边界，
				//   越过边界的老结论一律不再产生绿（见 FloraEntryInScope）。
				std::uint32_t epoch   = 0;
				// ★★★ v5.1.7（订正 R7）：这条结论**见证时的游戏时间**（天；0 = 未知 / 旧格式）。
				//   读档时用它判定作用域：`days ≤ 本存档的游戏时间` ⇒ 这个扫描发生在
				//   本存档的过去 ⇒ 记忆有效（见 FloraEntryInScope / docs/40）。
				float         days    = 0.0f;
			};
			std::unordered_map<std::uint32_t, FloraRefKnow> floraRefKnow;  // 引用 FormID -> 结论
			std::uint64_t floraRefHits      = 0;  // 被「按引用记忆」直接命中几次（诊断）
			std::uint64_t floraRefGreenNew  = 0;  // 本会话新增「绿」条目（= 真正学到的）
			std::uint64_t floraRefCyanNew   = 0;  // 本会话新增「青」条目
			// ★★★ v5.1.2（订正 R2）：**按物种（base）扩散的会话级表** —— 治用户实测的
			//   「有些植物扫描后还是青，打开扫描仪再关闭才变绿」。
			//   根因：引擎知识库是 **species（资源）级**的 —— 只要玩家学过这个物种，
			//   引擎就给同 species 的**所有**实例画绿（原版行为；v5.1 日志实证：举一下
			//   扫描仪，同 base 的一串引用被逐个画成 4/5）。但 v5.1 的记忆是**引用级**的
			//   —— 同 species 里「引擎没画过」（不在扫描仪求值范围 / 视角外 / 数量上限）
			//   的那些实例就永远学不到 ⇒ 停在青色；再举一次扫描仪它们被画到 ⇒ 才补上
			//   （= 用户说的「打开扫描仪再关闭又变成绿色」）。
			//   修法：任一引用被**权威**确认「已扫描」⇒ 把它的 base 记进这张表；
			//   判定时 base 命中 ⇒ 直接绿（同 species / 同资源全绿，与原版一致）。
			//   ★ 为什么不落盘：base 级**跨存档**复用正是 v5.1 修掉的「矿石、植物扫描
			//     前后颜色都是绿色」（docs/33 —— 落盘的 base 来自别的会话 / 别的存档）。
			//     重启游戏后靠「引用级落盘记忆」在遍历到旧引用时**动态激活**（见
			//     FloraTargetScanned ⓪）—— 影响面与「引用级记忆」本身完全相同，
			//     不会额外外溢到别的存档。
			//   ★★★ v5.1.5（订正 R5）：值从「空」升级成 **FloraBaseScope**（见证时所在的
			//     worldspace 列表）—— 同 species 只在**同一颗星球**扩散（用户报的假绿有一半
			//     是跨星球来的）。表本身仍然不落盘（会话级，读档时清空）。
			std::unordered_map<std::uint32_t, FloraBaseScope> floraBaseKnow;  // base FormID -> 作用域
			//   `cell 指针 → worldspace 指针` 缓存（WorldspaceOfRef；换场景 / 读档时清空）
			std::unordered_map<std::uintptr_t, std::uintptr_t> floraCellWsCache;
			std::uint64_t floraBasePlanetDenied = 0;  // 因「不在同一颗星球」被拒绝的扩散次数（★ v5.1.8 起只在 FloraSpeciesPlanetScope=1 时才会涨）
			std::uint64_t floraBaseCrossPlanet = 0;   // ★ v5.1.8：跨星球**放行**的扩散次数（默认口径下发生 = 修复在干活）
			std::uint64_t floraBaseSeeded     = 0;    // ★ v5.1.8：用落盘记忆播种出来的物种表条目数
			std::uint64_t floraBaseHits = 0;  // 被「按物种扩散」直接命中几次（诊断）
			std::uint64_t floraBaseNew  = 0;  // 本会话记下几个 base（诊断）
			// ★★★ v5.1：只读探针（引擎状态表）的计数 —— 这一路**零副作用**
			//   （v4.33 那条调的是 LookupOrAdd，会插入条目 + 加引用计数 ⇒ 长会话变卡）
			std::uint64_t floraTableProbes   = 0;  // 读了几次
			std::uint64_t floraTableGreen    = 0;  // 读到 4/5（引擎亲手画的绿）
			std::uint64_t floraTableCyan     = 0;  // 读到 7/8（引擎亲手画的青）
			std::uint64_t floraTableNoEntry  = 0;  // 引擎从没给这个引用写过状态（最常见）
			// ★★★ v4.33：加固项的计数（诊断；语义见 Config 里 v4.33 / v5.1 段与 docs/31/33）
			std::uint64_t floraStatusTableHits = 0;  // 判据未命中时「读引擎状态表捡到 4/5」的次数
			std::uint64_t floraPersistLoaded   = 0;  // 启动时从落盘文件读回的**引用**数
			std::uint64_t floraPersistIgnored  = 0;  // 落盘文件里被忽略的**旧格式（base 级）**行数
			std::uint64_t floraPersistWrites   = 0;  // 追加写盘的条数
			bool          floraPersistReady    = false;
			std::string   floraPersistPath;          // 懒设置（首次用到时算一次）

			// ★★★ v5.1.5（订正 R5）：**读档 = 存档边界**（「已扫描」记忆按存档隔离）
			//   `saveLoadPending`：读档事件 sink 只置这个原子标志（事件不保证在主线程），
			//     真正的处理在主线程（载入画面关闭时 / Tick 兜底，见 ProcessFloraSaveLoad）。
			//   `floraSaveEpoch`：每处理一次读档 +1；记忆条目自带 epoch（见 FloraRefKnow）。
			std::atomic_bool   saveLoadPending{ false };
			std::atomic_uint64_t saveLoadPendingAtMs{ 0 };  // 事件时刻（Tick 兜底的延迟守卫）
			std::atomic_uint64_t loadEvtTotal{ 0 };      // 收到几次读档事件（诊断）
			std::uint32_t    floraSaveEpoch        = 0;  // 已处理几次读档
			std::uint64_t    floraSaveLoads        = 0;
			std::uint64_t    floraScopeSkipped     = 0;  // 因「不在本存档作用域」而被跳过的记忆命中
			std::uint64_t    floraScopeDemoted     = 0;  // 被引擎当场画的「青」翻案掉的过期绿
			std::uint64_t    floraScopeRescoped    = 0;  // 过期条目在本存档内被重新见证（回到作用域）
			std::uint64_t    floraScopeDropped     = 0;  // 读档复核时被资源链证伪而丢掉的**条目**数
			std::uint64_t    floraScopeDroppedBase = 0;  // 同上，被证伪的 **base** 数
			std::uint64_t    floraScopeChainDisproved = 0;  // 复核里被链**证伪**（本存档没扫描）的 base 数
			std::uint64_t    floraScopeChainSkip   = 0;  // 复核里「链不适用 / 超预算」而没结论的 base 数
			std::uint64_t    floraScopeClearedBase = 0;  // 读档时清掉的物种表条目数
			// ★★★ v5.1.6（订正 R6）：**证据驱动**的两个计数 + 「待复核」调度
			//   `floraScopeKept`：事件到了但链**一个 base 都证伪不了** ⇒ 判定为
			//     「传送 / 读同一存档」、记忆保持有效 的次数（= 没有误作废的次数）。
			//   `floraScopeConfirmedBase`：复核里被链**确认**（本存档里已扫描）的 base 数
			//     （与 floraScopeChainDisproved 相对的一张分母）。
			//   `floraScopeEvalAtMs`：!= 0 ⇒ 「到此时刻（且世界稳定）再做读档复核」。
			//     事件不再立刻处理（见 NoteFloraSaveLoad —— 载入刚结束的那一瞬间
			//     复核会拿到假证伪）。
			std::uint64_t    floraScopeKept        = 0;
			std::uint64_t    floraScopeConfirmedBase = 0;
			std::uint64_t    floraScopeEvalAtMs    = 0;
			const char*      floraScopeEvalWhen    = nullptr;  // 本次待复核的来源（日志用）
			// ★★★ v5.1.7（订正 R7）：**存档时间指纹**（读档边界的新口径，见 Config / docs/40）
			//   `floraSaveDaysFloor`：本存档的「游戏时间锚点」（天）—— 最近一次读档处理时
			//     读到的 `Calendar::gameDaysPassed`。<0 = 还没有可信锚点（一律不收紧）。
			//   `floraLegacyStampDays`：旧格式（无时间字段）条目的锚 —— 第一次可信读档时定，
			//     等价于 R5 的「落盘记忆在本会话第一次读档时仍然有效」。
			//   注：新口径下 `floraSaveEpoch` 只当**本时间线标记**（读一次档 +1；之后
			//     当场见证的条目不再受锚点限制，见 FloraEntryInScope）。
			float            floraSaveDaysFloor   = -1.0f;
			float            floraLegacyStampDays = -1.0f;
			std::uint64_t    floraFpLoads         = 0;      // 用指纹处理过几次读档（诊断）
			std::uint64_t    floraFpVoided        = 0;      // 累计「不在本存档作用域」的条目数（非破坏）
			std::uint64_t    floraFpClearedBase   = 0;      // 累计被剪枝的物种表条目数
			std::uint64_t    floraFpFailures      = 0;      // 游戏时间读不到的次数（回退链证据路径）
			std::uint64_t    floraPersistNoTime   = 0;      // 落盘行里没有时间字段的条数（旧格式，诊断）
			float            floraDaysCache       = 0.0f;   // `CurrentGameDays` 的 1 秒缓存
			std::uint64_t    floraDaysCacheAtMs   = 0;
			char             floraFpSaveName[40]{};         // 诊断：最近一次读档的存档名（读不到 = 空）
			std::uint32_t    floraFpSaveNo        = 0;      // 诊断：currentSaveGameNumber
			bool             loadSinkRegistered    = false;
			std::uint64_t    loadSinkRetryAtMs     = 0;
			std::uint64_t    loadSinkCheckMs       = 0;
			std::uint64_t    loadSinkFailures      = 0;
			std::uint8_t*    loadSinkSource        = nullptr;  // 已注册的读档事件源（用于「还在不在」核对）

			// --- ★ v4.7：Tick 间隔诊断（区分「Tick 没被调」和「被早退挡住」）---
			std::uint64_t lastTickMs     = 0;
			std::uint64_t tickGaps       = 0;  // 间隔 > kTickGapLogMs 的次数
			std::uint64_t tickGapMsMax   = 0;
			std::uint64_t tickGapMsTotal = 0;

			// --- ★ v4.7：3D 复检（引擎侧描边丢了就重挂）---
			std::size_t   verifyCursor       = 0;  // 轮转游标（覆盖所有已挂目标）
			std::uint64_t outline3DProbes    = 0;  // 复检了多少次
			std::uint64_t outline3DReasserts = 0;  // 其中发现 3D 变了、重挂了多少次

			// --- ★ v4.19：渲染侧参数探针跑了多少次（每会话限流 3 次，见 LogRendererParams）---
			std::uint32_t rendererProbeRuns  = 0;

			// --- ★★★ v4.24：管理器占用探针（「举起扫描仪」1.5 秒后打一行，每会话 ≤6 次）---
			bool          manDumpPending = false;
			std::uint64_t manDumpAtMs    = 0;
			std::uint32_t manDumpRuns    = 0;

			// --- ★ v4.7：移动距离（诊断：把「行走」和「跳过」对起来看）---
			RE::NiPoint3 lastPos{};
			bool         havePos    = false;
			float        moveMeters = 0.0f;
		};

		Config g_cfg;
		// ================================================================
		// ★★ v4.15（崩溃 A 的修复，docs/15 §3）：**故意泄漏 `g_state`，永不析构**
		// ================================================================
		// `g_state.outlined` 里存的是 `RE::NiPointer<RE::TESObjectREFR>`，它的析构会调
		// **引擎函数** `TESForm::DecRefCount()`（commonlibsf 里那个成员带一个函数内
		// `static REL::Relocation{ ID::TESForm::DecRefCount }`）。
		//
		// 进程退出时的析构顺序是致命的：
		//   · `g_state` 是**命名空间静态对象** ⇒ 本模块加载时就构造，析构登记在
		//     CRT 退出表的**最前面** ⇒ 最后才析构；
		//   · commonlibsf 的 `REL::IDDB` 是 `TSingleton` 的**函数内静态**（第一次
		//     `REL::ID` 查表时才构造）⇒ 登记在**后面** ⇒ **先**析构，
		//     `~FMemoryMap` 把地址库文件的映射 `UnmapViewOfFile` 掉；
		//   · 之后轮到 `g_state` 析构 ⇒ `outlined` 的节点析构 ⇒ `DecRefCount()` 的
		//     那个 `static REL::Relocation` **首次**初始化 ⇒ 查 `m_v5[38742]`
		//     ⇒ 读**已 unmap 的映射** ⇒ C0000005（10 份转储全在 `SAS+0x4098A`）。
		// 故障栈实证（转储 24876）：
		//   IDDB::offset(IDDB.cpp:457) ← REL::ID::address(REL/ID.h:27)
		//   ← TESForm::DecRefCount(RE/T/TESForm.h:158) ← `~_Hash_vec`(xhash:256)
		//   ← `~list`(list:1061) ← dllmain_crt_process_detach(ucrt/vcstartup)
		//   ← DllMain(DLL_PROCESS_DETACH) ← kernel32!BaseThreadInitThunk
		// ⇒ 既不析构，进程退出时就不会有任何「引擎调用」；mod 里这是标准做法
		//   （退出时进程都要没了，泄漏这点内存无关紧要）。
		//   ★ 底线：**绝不在退出路径上碰游戏对象** —— 连 `DecRefCount` 也不碰
		//   （那时游戏自己的静态析构也可能已经把对象拆了）。
		State& g_state = *new State{};
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

		// ★★★ 订正 R4：**微秒级**时钟（QueryPerformanceCounter，约 20ns/次）。
		//   为什么必须换成 µs：原来所有分段计时都是 `NowMs() - t0`，单次操作 < 1ms
		//   就被取整成 0 —— 而热路径里的判空 / 分类 / 植物判据恰好全是「单次几百 µs、
		//   一轮几十次」的形态 ⇒ 日志里 `loot=0ms flora=2ms`，真实开销被抹平到
		//   walk 那一段里看不见了（R3 的结论就是这么被误导的）。
		std::uint64_t NowUs()
		{
			static const double kTicksPerUs = [] {
				LARGE_INTEGER f{};
				::QueryPerformanceFrequency(&f);
				return f.QuadPart > 0 ? static_cast<double>(f.QuadPart) / 1e6 : 1.0;
			}();
			LARGE_INTEGER c{};
			::QueryPerformanceCounter(&c);
			return static_cast<std::uint64_t>(static_cast<double>(c.QuadPart) / kTicksPerUs);
		}

		// 当前线程**真正占用 CPU** 的时间（µs）—— 与墙钟对比：
		//   墙钟远大于 CPU = 这段时间里线程被抢占 / 在等内存 / 在内核里（页错误）。
		//   ★ 这是区分「我们的指令慢」与「被别人抢了」的唯一硬判据。
		std::uint64_t ThreadCpuUs()
		{
			FILETIME creation{}, exit{}, kernel{}, user{};
			if (!::GetThreadTimes(::GetCurrentThread(), &creation, &exit, &kernel, &user)) {
				return 0;
			}
			const auto toUs = [](const FILETIME& a_ft) -> std::uint64_t {
				ULARGE_INTEGER u{};
				u.LowPart  = a_ft.dwLowDateTime;
				u.HighPart = a_ft.dwHighDateTime;
				return u.QuadPart / 10;  // 100ns → µs
			};
			return toUs(kernel) + toUs(user);
		}

		// ★★★ 订正 R4：**不进制内核**的安全读（SEH 兜底 + 直接 memcpy）。
		//   动机见 Config::fastReadMem 的长注释：ReadProcessMemory 要拿进程地址空间锁，
		//   游戏流式加载时单次调用可慢 10~30 倍，而本项目一轮扫描要读几百次。
		//   语义完全等价（读不出来 ⇒ false）：
		//     · 正常内存：一条 memcpy（~ns 级，零内核调用）；
		//     · 不可读地址：触发硬件异常 → `__except` 吃掉 → 返回 false（罕见）。
		//   分两次拷贝（先到栈上暂存再给调用方）是为了「失败时**不污染**目标缓冲区」——
		//   有调用方不看返回值就按旧值用（例如 `SafeReadMem(raw + x, &v, 4)` 之后
		//   拿 v 当 0 用），半途失败的 memcpy 可能留下垃圾。
		//   ★ `noinline` 是必需的：含 `__try` 的函数一旦被内联进「有析构对象」的调用点，
		//     MSVC 会报 C2712（cannot use __try in functions that require object unwinding）。
		constexpr std::size_t kSehStageBytes = 256;
		__declspec(noinline) bool SehReadMem(const void* a_src, void* a_dst, std::size_t a_len)
		{
			if (a_len == 0) {
				return true;
			}
			std::uint8_t stage[kSehStageBytes];
			std::size_t  done = 0;
			while (done < a_len) {
				const std::size_t chunk =
					std::min<std::size_t>(a_len - done, kSehStageBytes);
				__try {
					std::memcpy(stage, static_cast<const std::uint8_t*>(a_src) + done, chunk);
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					return false;
				}
				std::memcpy(static_cast<std::uint8_t*>(a_dst) + done, stage, chunk);
				done += chunk;
			}
			return true;
		}

		// 诊断计数（窗口内）：直读撞异常 / 走内核兜底
		std::uint64_t g_sehFaults  = 0;
		std::uint64_t g_kernelReads = 0;

		// ★ v2.3：把 Rescan 拆段计时用的小工具（作用域结束自动累加）。
		//   用它以后，即使代码里中途 return，耗时也照样被记上。
		//   ★★★ 订正 R4：计时改成 **µs**（原来 ms 会把「单次几百 µs」抹成 0）。
		struct PhaseTimer
		{
			std::uint64_t* sum;
			std::uint64_t* max;
			std::uint64_t  t0;

			explicit PhaseTimer(std::uint64_t* a_sum, std::uint64_t* a_max = nullptr) :
				sum(a_sum), max(a_max), t0(NowUs())
			{}

			~PhaseTimer()
			{
				const auto dt = NowUs() - t0;
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
		// ★★★ 订正 R4：槽位 8 → **64**、TTL 3s → **10s**。
		//   理由：8 个槽在「当前 cell + 环内 cell + 库存对象 + 表单对象」这种
		//   十几个区间的场景里*几乎每轮都在互相淘汰* ⇒ 每轮都要重新问内核，
		//   而 VirtualQuery 正是本轮要消灭的那类调用（见 Config::fastReadMem）。
		constexpr std::size_t kReadRegionCount = 64;
		constexpr std::uint64_t kReadRegionTtlMs = 10000;
		constexpr std::uintptr_t kPageSize  = 0x1000;
		constexpr std::uintptr_t kPageMask  = kPageSize - 1;
		ReadRegion            g_readRegions[kReadRegionCount];
		std::size_t           g_readRegionNext = 0;
		std::uint64_t         g_vqCalls        = 0;  // 诊断：窗口内真的问了内核多少次

		void InvalidateReadRegions()
		{
			for (auto& r : g_readRegions) {
				r = ReadRegion{};
			}
		}

		// 把「[start,end) 已验证可读」记进缓存（页对齐窗口，理由是保护是**页粒度**的：
		// 页内任意字节可读 ⇒ 整页可读，所以按页扩边是安全的）。
		void NoteReadRegion(std::uintptr_t a_start, std::uintptr_t a_end, std::uint64_t a_nowMs)
		{
			const auto b = a_start & ~kPageMask;
			const auto e = (a_end + kPageMask) & ~kPageMask;
			auto&      slot = g_readRegions[g_readRegionNext];
			g_readRegionNext = (g_readRegionNext + 1) % kReadRegionCount;
			slot.base        = b;
			slot.end         = e;
			slot.expiresMs   = a_nowMs + kReadRegionTtlMs;
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

			// ★★★ 订正 R4：缓存未命中时**先试不进制内核的那条路** ——
			//   逐页 1 字节直读（页头能读 ⇒ 整页能读；页头不可读 ⇒ 直接判不可读）。
			//   原本这里直接走 VirtualQuery（要拿地址空间锁），实测动态场景里
			//   单次可达几百 µs；现在正常内存零内核调用。
			if (g_cfg.fastReadMem) {
				bool ok = true;
				for (auto p = start; p < end; p = (p & ~kPageMask) + kPageSize) {
					std::uint8_t probe = 0;
					if (!SehReadMem(reinterpret_cast<const void*>(p), &probe, 1)) {
						ok = false;
						break;
					}
				}
				if (ok) {
					NoteReadRegion(start, end, now);
					return true;
				}
				++g_sehFaults;  // 诊断：直读真的撞到不可读页（正常应极少）
				return false;   // 页不可读：VirtualQuery 也只会答「不可读」，不必再花一次内核调用
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
			NoteReadRegion(base, rend, now);
			return true;
		}

		// ================================================================
		// ★ v4.3：强判据 —— 这个地址真的指向一个「引擎表单（TESForm）」吗？
		// ================================================================
		// 用途：标定「库存列表偏移」时校验「读到的条目里 `object` 字段是不是真的物品」
		//   —— 标定选错偏移时，读到的多半是垃圾指针，这里会立刻失败。
		//   判据全是 TESForm 基类的固定成员（本项目长期实测读法正确，且有 static_assert 钉住）：
		//     formID @+0x28 非 0；formType @+0x2E ∈ (0, kTotal)（commonlibsf FormTypes.h：0x00=kNONE、
		//     0xD7=kTotal ⇒ 合法类型都在这个区间）。
		// ★★★ v4.27 订正：上界原来是 **0x60**（"引擎的表单类型都在这个区间"）—— **错了**：
		//   commonlibsf `RE/F/FormTypes.h` 里 `kIRES = 0x9F`（= BGSResource，资源记录）、
		//   `kBIOM = 0xA0`、`kCHAL = 0xD6`，一直到 `kTotal = 0xD7`。
		//   而 v4.26 的「已扫描」链**恰恰要求元素是 IRES**（引擎 0x159F548 `cmp byte [rcx+0x2e],0x9f`）
		//   ⇒ 于是 `ReadResourceArray()` 的强形状校验**永远过不了**（2026-09-26 17:22 那局实测：
		//   `链失败 = 查询`、`偏移=0x0/0x0`、每个 base 都是 `资源数组[+0x0]=0个 irES=[无]` —— 证据②全灭）。
		//   库存链（v4.3 起）没被这条坑到，只是因为物品类型（MISC 0x28 / WEAP 0x30 / BOOK 0x27…）恰好都 < 0x60。
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
			if (ft == 0 || ft >= static_cast<std::uint8_t>(RE::FormType::kTotal)) {
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

		// ----------------------------------------------------------------
		// ★ v4.10 / v4.11：「容器（搜刮）界面」此刻是不是开着
		// ----------------------------------------------------------------
		// 用于展示柜的「拿空即灭」判据（见常量区「v4.10 展示柜拿空即灭」）。
		// 菜单名默认 `"ContainerMenu"` —— 游戏自带脚本实证（`AudioContainerNoAnimScript`
		// 拿它做容器开合音效、`OutpostContainerScript` 拿它做「打开就进 busy、关掉再
		// 结算」）；可由 INI `ContainerMenuName` 覆盖（万一是别的名字不用换 DLL）。
		// 与 IsMonocleMenuOpen 一样：菜单没注册时返回 false，不会崩。
		//   ★ v4.11：这条路（轮询）在实测里对 `ContainerMenu` **读不到**（全程 ui=0，
		//     见常量区「v4.11」）⇒ 状态改成「轮询 ∨ 事件」取或，事件那条路见下面。
		bool PollContainerMenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}
			static const RE::BSFixedString kName{ g_cfg.containerMenuName };
			return ui->IsMenuOpen(kName);
		}

		// 泛化版：任意菜单名此刻开着没有（诊断 / 交叉校验用；同样「没注册的名字返回 false」）。
		bool IsMenuNameOpenNow(const char* a_name)
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui || !a_name || !*a_name) {
				return false;
			}
			const RE::BSFixedString name{ a_name };
			return ui->IsMenuOpen(name);
		}

		// ================================================================
		// ★★ v4.11：容器界面 —— 引擎菜单事件通道（完整推导见常量区「v4.11」）
		// ================================================================
		// 事件类型与 commonlibsf `RE::MenuOpenCloseEvent` **同布局**（那个头文件单独
		// include 编译不过，见 v4.0 的经验）；`BSTEventSource<T>` 的布局与 T 无关，
		// 引擎侧的 `NotifyVisitor` 只按 vtable 第 1 槽回调 ⇒ 同布局即可安全互操作。
		struct SasMenuOpenCloseEvent
		{
			RE::BSFixedString menuName;  // 00
			bool              opening;   // 08
		};
		static_assert(sizeof(SasMenuOpenCloseEvent) == 0x10, "must match RE::MenuOpenCloseEvent");

		// 候选菜单名（诊断 / 自动学习用）—— 全部来自**离线取证**：
		//   ① `Starfield.exe` 里那张菜单名表（2026-09-19 dump，见 docs/12 §12）；
		//   ② 游戏自带 `.psc` 里 `RegisterForMenuOpenCloseEvent("…")` 用到的名字
		//      （`Data\Scripts\Source` 全量 grep，一次就全拿到）；
		//   ③ 界面归档里的 swf（`interface/containermenu.swf` 等）。
		//   `IsMenuOpen` 对**没注册**的名字只会返回 false（项目长期实测），
		//   所以这张表随便列，绝不会崩。
		const char* const kMenuNameCandidates[] = {
			"ContainerMenu", "ShipHUDQuickContainer", "BarterMenu", "InventoryMenu", "DataMenu",
			"CraftingMenu", "ArmorCraftingMenu", "WeaponsCraftingMenu", "FoodCraftingMenu",
			"DrugsCraftingMenu", "IndustrialCraftingMenu", "ResearchMenu", "WorkshopMenu",
			"WorkshopQuickMenu", "Workshop_BlueprintMenu", "SpaceshipEditorMenu", "ShipCrewMenu",
			"SkillsMenu", "StatusMenu", "PowersMenu", "MapMenu", "GalaxyStarMapMenu",
			"DataSlateMenu", "BookMenu", "CreditsMenu", "MarketplaceMenu", "MissionMenu",
			"BSMissionMenu", "DialogueMenu", "ShipDialogueMenu", "MessageBoxMenu", "LockpickingMenu",
			"PickpocketMenu", "SecurityMenu", "SitWaitMenu", "SleepWaitMenu", "GenesisTerminalMenu",
			"CustomItemMenu", "AlmanacMenu", "LooksMenu", "ChargenMenu", "SpaceshipInfoMenu",
			"PauseMenu", "MainMenu", "TitleSequenceMenu", "EndGameCreditsMenu", "LoadingMenu",
			"LoadingMenuNewLocation", "FaderMenu", "StreamingInstallMenu", "PlayBinkMenu",
			"ConsoleNativeUIMenu", "TestMenu", "BoundaryMenu", "FanfareMenu", "HUDMenu",
			"HUDMessagesMenu", "HUDActionPointData", "MonocleMenu", "CursorMenu", "ForceClose",
			"NoMenu", "TextInputMenu",
		};
		constexpr std::size_t kMenuNameCandidatesCount =
			sizeof(kMenuNameCandidates) / sizeof(kMenuNameCandidates[0]);

		// 这些菜单**永远开着 / 与容器无关**，不能被自动学习成「容器界面」
		//   （否则玩家一开背包就会被当成「容器界面开着」，可能误判「拿空」）。
		bool IsIgnoredMenuName(const char* a_name)
		{
			static const char* const kIgnored[] = {
				"HUDMenu", "HUDMessagesMenu", "HUDActionPointData", "CursorMenu", "FaderMenu",
				"LoadingMenu", "LoadingMenuNewLocation", "MainMenu", "TitleSequenceMenu",
				"PlayBinkMenu", "TestMenu", "ConsoleNativeUIMenu", "NoMenu", "ForceClose",
				"StreamingInstallMenu", "PauseMenu", "MessageBoxMenu", "MonocleMenu",
				"InventoryMenu", "DataMenu", "MapMenu", "GalaxyStarMapMenu", "SkillsMenu",
				"StatusMenu", "BarterMenu", "ShipCrewMenu", "PowersMenu", "ChargedMenu",
			};
			for (const auto* n : kIgnored) {
				if (::_stricmp(a_name, n) == 0) {
					return true;
				}
			}
			return false;
		}

		// 「这个名字算不算容器（搜刮）界面」：配置名 ∨ 内置 `ContainerMenu` ∨ 已学习名
		//   （大小写不敏感 —— 引擎内部是大小写不敏感的比较）。
		//   ★ 调用时必须已持有 `menuEvtLock`（学习集合由它保护）。
		bool ContainerMenuNameMatchesLocked(const char* a_name)
		{
			if (!a_name || !*a_name) {
				return false;
			}
			if (::_stricmp(a_name, g_cfg.containerMenuName) == 0) {
				return true;
			}
			if (::_stricmp(a_name, "ContainerMenu") == 0) {
				return true;
			}
			for (std::size_t i = 0; i < g_state.containerMenuLearnedCount; ++i) {
				if (::_stricmp(a_name, g_state.containerMenuLearned[i].c_str()) == 0) {
					return true;
				}
			}
			return false;
		}

		bool ContainerMenuNameMatches(const char* a_name)
		{
			std::scoped_lock lock{ g_state.menuEvtLock };
			return ContainerMenuNameMatchesLocked(a_name);
		}

		// 事件侧「此刻开着的菜单里有没有容器界面」——**在锁内重算**（所以学到新名字后
		//   历史状态也立刻跟着正确）。
		bool MenuEvtContainerOpenLocked()
		{
			for (int i = 0; i < g_state.menuEvtOpenCount; ++i) {
				if (ContainerMenuNameMatchesLocked(g_state.menuEvtOpenNames[i])) {
					return true;
				}
			}
			return false;
		}

		// 菜单事件回调（可能来自 UI 线程 ⇒ 只做「记状态 + 少量日志」，绝不做引擎调用）。
		void OnMenuEvent(const char* a_name, bool a_opening)
		{
			if (!a_name || !*a_name) {
				return;
			}
			g_state.menuEvtTotal.fetch_add(1, std::memory_order_relaxed);
			bool isContainer  = false;
			bool firstOfName  = false;  // 这个名字头一次出现（那几条一定进日志，见下）
			{
				std::scoped_lock lock{ g_state.menuEvtLock };
				auto* openNames = g_state.menuEvtOpenNames;
				if (a_opening) {
					bool dup = false;
					for (int i = 0; i < g_state.menuEvtOpenCount; ++i) {
						if (::_stricmp(openNames[i], a_name) == 0) {
							dup = true;
							break;
						}
					}
					if (!dup) {
						if (g_state.menuEvtOpenCount < static_cast<int>(kMenuEvtOpenListMax)) {
							std::snprintf(openNames[g_state.menuEvtOpenCount], kMenuEvtNameMax, "%s", a_name);
							++g_state.menuEvtOpenCount;
						}
					}
					std::snprintf(g_state.menuEvtLastOpen, kMenuEvtNameMax, "%s", a_name);
					g_state.menuEvtLastOpenMs = NowMs();
				} else {
					for (int i = 0; i < g_state.menuEvtOpenCount; ++i) {
						if (::_stricmp(openNames[i], a_name) == 0) {
							for (int j = i + 1; j < g_state.menuEvtOpenCount; ++j) {
								std::snprintf(openNames[j - 1], kMenuEvtNameMax, "%s", openNames[j]);
							}
							--g_state.menuEvtOpenCount;
							openNames[g_state.menuEvtOpenCount][0] = '\0';
							break;
						}
					}
				}
				isContainer            = MenuEvtContainerOpenLocked();
				g_state.menuEvtOpen    = isContainer;  // atomics：主线程无锁读

				// ★ v4.12：记「按名字统计」+「最近几条事件」（都在锁内，回调线程写、主线程读）
				{
					std::size_t slot = g_state.menuEvtNameCount;
					for (std::size_t i = 0; i < g_state.menuEvtNameCount; ++i) {
						if (::_stricmp(g_state.menuEvtNames[i].name, a_name) == 0) {
							slot = i;
							break;
						}
					}
					if (slot == g_state.menuEvtNameCount) {
						if (slot < kMenuEvtNameStatMax) {
							std::snprintf(g_state.menuEvtNames[slot].name, kMenuEvtNameMax, "%s", a_name);
							g_state.menuEvtNames[slot].count = 0;
							++g_state.menuEvtNameCount;
						} else {
							slot = kMenuEvtNameStatMax;  // 表满：不再统计（只影响日志详略）
						}
					}
					if (slot < kMenuEvtNameStatMax) {
						firstOfName = g_state.menuEvtNames[slot].count == 0;
						++g_state.menuEvtNames[slot].count;
					}
				}
				// 最近事件：整体后移一格，新的放头部（最多 4 条，代价可忽略）
				{
					const std::size_t keep = std::min<std::size_t>(g_state.menuEvtRecentCount, kMenuEvtRecentMax - 1);
					for (std::size_t i = keep; i > 0; --i) {
						g_state.menuEvtRecent[i] = g_state.menuEvtRecent[i - 1];
					}
					std::snprintf(g_state.menuEvtRecent[0].name, kMenuEvtNameMax, "%s", a_name);
					g_state.menuEvtRecent[0].opening = a_opening;
					g_state.menuEvtRecent[0].ms      = NowMs();
					g_state.menuEvtRecentCount       = keep + 1;
				}
			}
			if (isContainer) {
				const auto n = g_state.menuEvtContainer.fetch_add(1, std::memory_order_relaxed) + 1;
				REX::INFO("menu event (container): \"{}\" {} -> 容器界面现在 {}（事件第 {} 条）",
					a_name, a_opening ? "opening" : "closing", a_opening ? "开着" : "关着", n);
			} else if (firstOfName ||
					   (g_state.menuEvtLogged < static_cast<std::uint64_t>(g_cfg.menuEventLogMax))) {
				// ★ v4.12：**每个菜单名的头几条一定记**（上一轮是全局 48 条，玩久了额度烧光，
				//   于是「搜刮界面到底叫什么名字」这个最关键的取证反而看不到）。
				++g_state.menuEvtLogged;
				REX::INFO("menu event: \"{}\" {}（普通菜单事件；每个名字头 {} 条必记，其余最多 {} 条）",
					a_name, a_opening ? "opening" : "closing", kMenuEvtPerNameLogMax, g_cfg.menuEventLogMax);
			}
		}

		// 前置声明（定义在 ResolveNativeOutline 之后）—— vtable 硬比对要用
		std::uintptr_t ModuleBase();

		// ----------------------------------------------------------------
		// ★ 事件负载的**安全读取**（2026-09-19）
		// ----------------------------------------------------------------
		// 为什么不能直接 `a_event.menuName.c_str()`：万一 `UI + 0x20` 不是
		// `BSTEventSource<MenuOpenCloseEvent>`（commonlibsf 的头文件偏移有过
		// `TESObjectCELL` 那种整体错位的前科，见 docs/02），我们就会按「菜单事件」
		// 去解释**别的事件**的负载 —— `BSFixedString::c_str()` 会去解引用第一字段里的
		// 指针 ⇒ 直接崩游戏。
		// ⇒ 两条保险：
		//   ① 全部内存读取走 `ReadProcessMemory`（读不到只是返回 false，**不会**抛访问异常）；
		//   ② 按 `BSStringPool::Entry` 的真实布局（commonlibsf `RE/B/BSStringPool.h`：
		//      `_left@0 / _length@8（或 _right）/ _refCount@0x10 / _flags@0x14`，
		//      字符串数据紧跟在 `entry + 0x18`）解出字符串，并要求它是
		//      「长度 3..31、只含 [A-Za-z0-9_] 的 ASCII 串」—— 菜单名天然满足，
		//      垃圾数据几乎不可能满足。
		//   ③ 实在读不出来 ⇒ 只记一次 `menu event (bad)`，累计到阈值就**注销 sink**
		//      （彻底断掉这条通道，退回轮询，绝不带着风险继续跑）。
		// ★★★ 订正 R4（性能）：默认走「SEH 直读」（见 SehReadMem 的说明），
		//   只有直读真的撞上不可读地址（罕见）才退回内核读一次。
		//   为什么这条是动态场景卡顿的主因候选：本插件一轮扫描里 SafeReadMem
		//   要被调几百次（容器 / 尸体判空的库存链），而 ReadProcessMemory 每次都要
		//   拿**进程地址空间锁** —— 游戏流式加载（走动 / 战斗）时那把锁被抢，
		//   单次从 ~10µs 涨到几百 µs ⇒ 一轮就多出几十毫秒。
		bool SafeReadMem(const void* a_src, void* a_dst, std::size_t a_len)
		{
			if (!a_src || !a_dst || a_len == 0) {
				return false;
			}
			if (g_cfg.fastReadMem) {
				if (SehReadMem(a_src, a_dst, a_len)) {
					return true;
				}
				++g_sehFaults;
				++g_kernelReads;  // 诊断：真的走了内核兜底
			} else {
				++g_kernelReads;
			}
			SIZE_T read = 0;
			return ::ReadProcessMemory(::GetCurrentProcess(), a_src, a_dst, a_len, &read) != 0 && read == a_len;
		}

		// ★★★ 订正 R4：直读（SEH）**启动自检** —— 决定 `FastReadMem` 敢不敢用。
		//   三条判据（全部在启动时跑，把「有问题」暴露在日志里而不是游戏崩/卡里）：
		//     ① 可读内存读得出来且内容一致；
		//     ② 不可读内存（现 reserve 一页、不 commit）**安全返回 false**、不崩、不卡死
		//        —— 这一条同时验证「没有别的模块把我们的访问违例吞掉」；
		//     ③ 顺带把「直读 vs 内核读」的单次耗时打出来（用户日志里就能看到差距）。
		bool SelfTestFastRead(std::uint64_t* a_outSehUs, std::uint64_t* a_outRpmUs)
		{
			// ① 可读
			std::uint64_t v   = 0x1122334455667788ULL;
			std::uint64_t got = 0;
			if (!SehReadMem(&v, &got, sizeof(got)) || got != v) {
				return false;
			}
			// ② 不可读：RESERVE（不 COMMIT）+ PAGE_NOACCESS ⇒ 读它必然触发访问违例
			auto* reserved = static_cast<std::uint8_t*>(
				::VirtualAlloc(nullptr, 0x1000, MEM_RESERVE, PAGE_NOACCESS));
			if (reserved) {
				std::uint8_t b    = 0;
				const bool   safe = !SehReadMem(reserved, &b, 1);
				::VirtualFree(reserved, 0, MEM_RELEASE);
				if (!safe) {
					return false;  // 真的读了却不报失败 ⇒ 不能用（可能是保护页语义异常）
				}
			}
			// ③ 耗时对照（200 次足够看出量级）
			constexpr int kLoop = 200;
			{
				std::uint64_t tmp = 0, sink = 0;
				const auto    t0 = NowUs();
				for (int i = 0; i < kLoop; ++i) {
					if (SehReadMem(&v, &tmp, sizeof(tmp))) {
						sink += tmp;
					}
				}
				if (a_outSehUs) {
					*a_outSehUs = NowUs() - t0;
				}
				if (sink == 0xDEADBEEFULL) {
					return false;  // 防优化（不可能命中）
				}
			}
			{
				std::uint64_t tmp = 0, sink = 0;
				SIZE_T        read = 0;
				const auto    t0 = NowUs();
				for (int i = 0; i < kLoop; ++i) {
					if (::ReadProcessMemory(::GetCurrentProcess(), &v, &tmp, sizeof(tmp), &read)) {
						sink += tmp;
					}
				}
				if (a_outRpmUs) {
					*a_outRpmUs = NowUs() - t0;
				}
				if (sink == 0xDEADBEEFULL) {
					return false;
				}
			}
			return true;
		}

		// ----------------------------------------------------------------
		// ★★ v4.15：引用对象「还活着吗」的强判据（崩溃 B 的修复，docs/15 §4）
		// ----------------------------------------------------------------
		// 崩溃 B 的现场（转储 Starfield_09-21-01-50.dmp）：
		//   `SAS+0x2023A` = `AlwaysScan.cpp:4442`（RefLootState 读库存 size），
		//   rcx（库存指针）= `0x1A486`、读地址 `0x1A4AE` = 0x1A486 + 0x28。
		//   把 R8（那个"引用"）的内存 dump 出来一看：**根本不是 TESObjectREFR**
		//   —— 头部是 `0x27C0000333C00003`（不是 vtable 指针），通篇是浮点数与小整数。
		// ⇒ 真根因：**展示柜逐帧观察表里留着一个已经被释放的引用**（换场景 / 载入期
		//   对象被销毁），那块堆内存被别的数据复用，于是 `*(ref + 0xA0)` 读出一串垃圾
		//   当库存指针用。**旧判据 `IsReadable` 挡不住这种情况**：内存还 mapped，
		//   VirtualQuery 一律通过（它只回答「能不能读」，回答不了「读出来的是什么」）。
		// ⇒ 三层判据（全部是纯读 + 比较，不做 VirtualQuery）：
		//     ① 引用头部（0x30 字节）能读出来 —— 走 SafeReadMem，读不出直接算死；
		//     ② `[ref + 0x00]` 是**落在游戏主模块映像内**的 vtable 指针
		//        （TESObjectREFR / Actor 的 vtable 都在 Starfield.exe 的 .rdata）；
		//     ③ `[ref + 0x2E]`（formType）∈ {REFR, ACHR}。
		//   ②③ 是 TESForm 的固定成员，垃圾内存同时满足的概率可以忽略。
		std::uintptr_t GameImageEnd()
		{
			static const std::uintptr_t end = [] {
				const auto* base = reinterpret_cast<const std::uint8_t*>(::GetModuleHandleA(nullptr));
				if (!base) {
					return std::uintptr_t{ 0 };
				}
				const auto lfanew = *reinterpret_cast<const std::uint32_t*>(base + 0x3C);
				const auto size   = *reinterpret_cast<const std::uint32_t*>(base + lfanew + 0x50);  // SizeOfImage
				return reinterpret_cast<std::uintptr_t>(base) + size;
			}();
			return end;
		}

		bool LooksLikeLiveRef(std::uint64_t a_ref)
		{
			const auto  base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleA(nullptr));
			const auto  end  = GameImageEnd();
			if (base == 0 || end == 0 || !IsPlausiblePointer(a_ref)) {
				return false;
			}
			std::uint8_t head[0x30]{};
			if (!SafeReadMem(reinterpret_cast<const void*>(a_ref), head, sizeof(head))) {
				return false;  // ① 连头部都读不出来 ⇒ 当它死了
			}
			const auto vtbl = *reinterpret_cast<const std::uint64_t*>(head);
			if (vtbl < base || vtbl >= end) {
				return false;  // ② 对象头不是「游戏映像里的 vtable」⇒ 内存已被复用
			}
			const auto ft = head[kOffFormType];
			return ft == kFormTypeREFR || ft == kFormTypeACHR;  // ③
		}

		bool LooksLikeMenuName(const char* a_s)
		{
			if (!a_s) {
				return false;
			}
			const std::size_t len = std::strlen(a_s);
			if (len < 3 || len > 31) {
				return false;
			}
			for (std::size_t i = 0; i < len; ++i) {
				const unsigned char c = static_cast<unsigned char>(a_s[i]);
				const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
								(c >= '0' && c <= '9') || c == '_';
				if (!ok) {
					return false;
				}
			}
			return true;
		}

		// 从事件负载里安全解出 `{ 菜单名, opening }`；任何一步不对就返回 false。
		bool SafeReadMenuEvent(const SasMenuOpenCloseEvent* a_ev, char* a_out, std::size_t a_outLen, bool& a_outOpening)
		{
			if (!a_ev || !a_out || a_outLen < 32) {
				return false;
			}
			std::uint64_t entry = 0;
			if (!SafeReadMem(a_ev, &entry, sizeof(entry))) {
				return false;  // 连 menuName._data 都读不到
			}
			// external（`_flags & 0x02`）时字符串在 `_right` 指的那一条上，跟着走几跳
			std::uint32_t flags = 0;
			std::uint32_t len   = 0;
			for (int hop = 0; hop < 4; ++hop) {
				if (!IsPlausiblePointer(entry)) {
					return false;
				}
				if (!SafeReadMem(reinterpret_cast<const void*>(entry + 0x14), &flags, 1)) {
					return false;
				}
				if (!SafeReadMem(reinterpret_cast<const void*>(entry + 0x08), &len, sizeof(len))) {
					return false;
				}
				if ((flags & 0x02) == 0) {
					break;  // 不是 external ⇒ 这就是 leaf
				}
				std::uint64_t next = 0;
				if (!SafeReadMem(reinterpret_cast<const void*>(entry + 0x08), &next, sizeof(next)) ||
					!IsPlausiblePointer(next)) {
					return false;
				}
				entry = next;
			}
			if ((flags & 0x02) != 0) {
				return false;  // 跳了 4 次还是 external ⇒ 形状不对劲
			}
			if (len == 0 || len >= a_outLen) {
				return false;
			}
			char tmp[64]{};
			if (len >= sizeof(tmp) || !SafeReadMem(reinterpret_cast<const void*>(entry + 0x18), tmp, len)) {
				return false;
			}
			tmp[len] = '\0';
			if (!LooksLikeMenuName(tmp)) {
				return false;
			}
			std::snprintf(a_out, a_outLen, "%s", tmp);
			std::uint8_t opening = 0;
			if (!SafeReadMem(reinterpret_cast<const std::uint8_t*>(a_ev) + 8, &opening, 1)) {
				return false;
			}
			a_outOpening = opening != 0;
			return true;
		}

		// ================================================================
		// ★★ v4.13：游戏事件通道 —— 负载结构 + sink + 「事件 → 主线程」队列
		//   （离线取证 / 判决分层 / 为什么换掉「面板 = 菜单」的前提，见常量区「v4.13」）
		// ----------------------------------------------------------------
		// 两个事件都由**引擎自己**发，`ProcessEvent` 的调用线程不保证是主线程 ⇒
		//   回调里**只做两件事**：把负载安全读出来、往队列塞一条记录；
		//   判决与日志全在主线程（`ProcessLootEvents`，每帧一次）做。
		// ================================================================
		struct SasContainerChangedEvent
		{
			std::uint32_t source;      // 00 旧容器（物品从这里出去）—— 非 0 = 一次「拿走」
			std::uint32_t target;      // 04 新容器（物品进到这里）—— 非 0 = 一次「放进去」
			std::uint32_t baseObject;  // 08 物品 base form
			std::uint32_t itemCount;   // 0C 数量
			std::uint32_t itemRef;     // 10
			std::uint16_t uniqueID;    // 14
			std::uint8_t  pad0[2];     // 16
			std::uint64_t unk18;       // 18
			std::uint32_t unk20;       // 20
			std::uint8_t  pad1[4];     // 24
		};
		static_assert(sizeof(SasContainerChangedEvent) == 0x28, "must match RE::TESContainerChangedEvent");

		// `QuickContainerOpenedEvent` 的负载 = 一个指向**容器引用**的指针（反汇编实证）。
		struct SasQuickContainerOpenedEvent
		{
			void* ref;  // 00
		};
		static_assert(sizeof(SasQuickContainerOpenedEvent) == 0x8);

		enum class LootEvtKind : std::uint8_t
		{
			kTake      = 0,  // 物品从这个 ref 出去
			kPut       = 1,  // 物品进这个 ref
			kQuickOpen = 2   // 快速搜刮面板为这个 ref 打开
		};

		struct LootEvtRec
		{
			std::uint8_t  kind    = 0;  // LootEvtKind
			std::uint32_t refFid  = 0;  // 涉及的容器引用 FormID（take / put）
			std::uint32_t baseFid = 0;  // 物品 base form（take / put）
			std::uint32_t count   = 0;  // 数量（take / put）
			std::uint64_t refPtr  = 0;  // 容器引用指针（kQuickOpen）
			std::uint64_t ms      = 0;  // 事件时间
		};

		std::mutex                               g_lootEvtLock;
		std::array<LootEvtRec, kLootEvtQueueMax> g_lootEvtQueue{};
		std::size_t                              g_lootEvtQueueCount = 0;
		std::atomic<std::uint64_t>               g_lootEvtTotal{ 0 };
		std::atomic<std::uint64_t>               g_lootEvtTakeTotal{ 0 };
		std::atomic<std::uint64_t>               g_lootEvtPutTotal{ 0 };
		std::atomic<std::uint64_t>               g_lootEvtQuickTotal{ 0 };
		std::atomic<std::uint64_t>               g_lootEvtDropped{ 0 };
		std::atomic<std::uint64_t>               g_lootEvtBad{ 0 };

		void PushLootEvt(const LootEvtRec& a_rec)
		{
			std::scoped_lock lock{ g_lootEvtLock };
			if (g_lootEvtQueueCount >= kLootEvtQueueMax) {
				g_lootEvtDropped.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			g_lootEvtQueue[g_lootEvtQueueCount++] = a_rec;
		}

		class SasInvEvtSink final : public RE::BSTEventSink<SasContainerChangedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SasContainerChangedEvent& a_event,
				RE::BSTEventSource<SasContainerChangedEvent>*) override
			{
				if (!g_state.lootEvtActive) {
					return RE::BSEventNotifyControl::kContinue;
				}
				SasContainerChangedEvent ev{};
				if (!SafeReadMem(&a_event, &ev, sizeof(ev))) {
					g_lootEvtBad.fetch_add(1, std::memory_order_relaxed);
					return RE::BSEventNotifyControl::kContinue;
				}
				// 「从 A 搬到 B」两侧都非 0 ⇒ 两边各记一条（我们只关心命中观察表的那些）。
				if (ev.source != 0) {
					LootEvtRec rec{};
					rec.kind    = static_cast<std::uint8_t>(LootEvtKind::kTake);
					rec.refFid  = ev.source;
					rec.baseFid = ev.baseObject;
					rec.count   = ev.itemCount;
					rec.ms      = NowMs();
					PushLootEvt(rec);
					g_lootEvtTakeTotal.fetch_add(1, std::memory_order_relaxed);
				}
				if (ev.target != 0) {
					LootEvtRec rec{};
					rec.kind    = static_cast<std::uint8_t>(LootEvtKind::kPut);
					rec.refFid  = ev.target;
					rec.baseFid = ev.baseObject;
					rec.count   = ev.itemCount;
					rec.ms      = NowMs();
					PushLootEvt(rec);
					g_lootEvtPutTotal.fetch_add(1, std::memory_order_relaxed);
				}
				g_lootEvtTotal.fetch_add(1, std::memory_order_relaxed);
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		SasInvEvtSink g_invEvtSink;

		class SasQuickOpenSink final : public RE::BSTEventSink<SasQuickContainerOpenedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SasQuickContainerOpenedEvent& a_event,
				RE::BSTEventSource<SasQuickContainerOpenedEvent>*) override
			{
				if (!g_state.quickOpenActive) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::uint64_t refPtr = 0;
				if (!SafeReadMem(&a_event, &refPtr, sizeof(refPtr))) {
					g_lootEvtBad.fetch_add(1, std::memory_order_relaxed);
					return RE::BSEventNotifyControl::kContinue;
				}
				LootEvtRec rec{};
				rec.kind   = static_cast<std::uint8_t>(LootEvtKind::kQuickOpen);
				rec.refPtr = refPtr;
				rec.ms     = NowMs();
				PushLootEvt(rec);
				g_lootEvtQuickTotal.fetch_add(1, std::memory_order_relaxed);
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		SasQuickOpenSink g_quickOpenSink;

		// 「我们的 sink 还在不在这个事件源的 sink 数组里」（引擎清表 / 重建时防哑）。
		bool SinkStillInArray(const std::uint8_t* a_src, const void* a_sink)
		{
			if (!a_src || !a_sink) {
				return false;
			}
			const auto sz = *reinterpret_cast<const std::uint32_t*>(a_src + 0x08);
			const auto cp = *reinterpret_cast<const std::uint32_t*>(a_src + 0x0C);
			const auto dp = *reinterpret_cast<const std::uint64_t*>(a_src + 0x10);
			if (sz == 0 || sz > cp || cp > 4096) {
				return false;
			}
			if (!IsReadable(reinterpret_cast<const void*>(dp), static_cast<std::size_t>(sz) * 8)) {
				return false;
			}
			auto* const* sinks = reinterpret_cast<void* const*>(dp);
			for (std::uint32_t i = 0; i < sz; ++i) {
				if (sinks[i] == a_sink) {
					return true;
				}
			}
			return false;
		}

		// 注册一个**静态**事件源的 sink（v4.13 的两个源都是静态对象，直接取地址）。
		//   ★ 与 v4.11/v4.12 同一套安全姿势：vtable 硬核对 + BSTEventSource 形状校验；
		//     核对不过就**不注册**（最坏结果 = 少一条信号，绝不会按错误布局解释负载）。
		template <class T>
		bool RegisterEngineEventSink(std::uintptr_t a_srcRva, std::uintptr_t a_vtblRva,
			RE::BSTEventSink<T>* a_sink, const char* a_name)
		{
			const auto  base = ModuleBase();
			auto* const src  = reinterpret_cast<std::uint8_t*>(base + a_srcRva);
			if (!IsReadable(src, 0x20)) {
				REX::WARN("loot events: {} 的静态源不可读（RVA 0x{:X}）-> 不注册", a_name, a_srcRva);
				return false;
			}
			const auto sz = *reinterpret_cast<const std::uint32_t*>(src + 0x08);
			const auto cp = *reinterpret_cast<const std::uint32_t*>(src + 0x0C);
			const auto dp = *reinterpret_cast<const std::uint64_t*>(src + 0x10);
			if (sz > cp || cp > 4096 || (sz != 0 && !IsReadable(reinterpret_cast<const void*>(dp), 8))) {
				REX::WARN("loot events: {} 的形状不像事件源（size={} cap={} data=0x{:X}，RVA 0x{:X}）-> 不注册",
					a_name, sz, cp, dp, a_srcRva);
				return false;
			}
			const auto vtbl     = *reinterpret_cast<const std::uintptr_t*>(src);
			const auto expected = base + a_vtblRva;
			if (vtbl != expected) {
				REX::WARN("loot events: {} 的 vtable=0x{:X}，期望 0x{:X}（RVA 0x{:X}）-> **不注册**"
						  "（宁可少一条信号，也绝不按错的布局解释负载）",
					a_name, vtbl, expected, a_vtblRva);
				return false;
			}
			reinterpret_cast<RE::BSTEventSource<T>*>(src)->RegisterSink(a_sink);
			REX::INFO("loot events: sink registered for {}（源 RVA 0x{:X}，vtable=0x{:X} 已核对；"
					  "sinks size={} cap={}）",
				a_name, a_srcRva, vtbl, sz, cp);
			return true;
		}

		// 主线程：确保两个事件 sink 挂着（失败每 4 秒重试；挂上后每 60 秒核对一次）。
		void EnsureLootEventSinks(std::uint64_t a_nowMs)
		{
			if (!g_cfg.containerLootEvents) {
				return;
			}
			const auto base = ModuleBase();
			if (g_state.invEvtRegistered || g_state.quickOpenRegistered) {
				if (a_nowMs - g_state.lootEvtSinkCheckMs < kLootEvtSinkCheckMs) {
					return;
				}
				g_state.lootEvtSinkCheckMs = a_nowMs;
				// ★ 只重挂「真的掉出去的那个」—— 两个都重挂会让还挂着的那个变成
				//   数组里有两条 ⇒ 事件被处理两次 ⇒ 记账会被多减一次（真会算错账）。
				if (g_state.invEvtRegistered &&
					!SinkStillInArray(reinterpret_cast<const std::uint8_t*>(base + kInvEvtSourceRva), &g_invEvtSink)) {
					g_state.invEvtRegistered = false;
					g_state.lootEvtRetryAtMs = 0;  // 立刻重挂（不等 4 秒节流）
					REX::WARN("loot events: 物品事件的 sink 从事件源数组里掉出去了 -> 重挂它");
				}
				if (g_state.quickOpenRegistered &&
					!SinkStillInArray(reinterpret_cast<const std::uint8_t*>(base + kQuickOpenSourceRva), &g_quickOpenSink)) {
					g_state.quickOpenRegistered = false;
					g_state.lootEvtRetryAtMs    = 0;
					REX::WARN("loot events: 快速面板事件的 sink 从事件源数组里掉出去了 -> 重挂它");
				}
				if (g_state.invEvtRegistered && g_state.quickOpenRegistered) {
					return;
				}
			}
			if (g_state.lootEvtRetryAtMs > a_nowMs) {
				return;
			}
			g_state.lootEvtRetryAtMs = a_nowMs + 4000;

			if (!g_state.invEvtRegistered) {
				if (RegisterEngineEventSink<SasContainerChangedEvent>(kInvEvtSourceRva, kInvEvtSourceVtblRva,
						&g_invEvtSink, "TESContainerChangedEvent（物品进出容器）")) {
					g_state.invEvtRegistered = true;
					g_state.lootEvtActive     = true;
				} else {
					++g_state.lootEvtSinkFailures;
				}
			}
			if (!g_state.quickOpenRegistered) {
				if (RegisterEngineEventSink<SasQuickContainerOpenedEvent>(kQuickOpenSourceRva, kQuickOpenSourceVtblRva,
						&g_quickOpenSink, "QuickContainerOpenedEvent（快速搜刮面板打开）")) {
					g_state.quickOpenRegistered = true;
					g_state.quickOpenActive     = true;
				} else {
					++g_state.lootEvtSinkFailures;
				}
			}
			if (g_state.invEvtRegistered && g_state.quickOpenRegistered) {
				g_state.lootEvtSinkCheckMs = a_nowMs;
				REX::INFO("loot events: 两个通道就绪 -> 容器界面信号 = 记账（精确）+ 兜底（快速面板 + 连续读到空）"
						  "；展示柜「拿空即灭」不再依赖「面板是不是菜单」");
			}
		}

		void DisableMenuEventSink(const char* a_why);

		// 事件 sink：`ProcessEvent` 由引擎通过 vtable 调用（第 1 槽）。
		class SasMenuEventSink final : public RE::BSTEventSink<SasMenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SasMenuOpenCloseEvent& a_event,
				RE::BSTEventSource<SasMenuOpenCloseEvent>*) override
			{
				// ★ v4.12：通道停用后**立刻返回**（sink 仍然挂在源上，但什么都不做）
				if (!g_state.menuEvtActive) {
					return RE::BSEventNotifyControl::kContinue;
				}
				char name[32]{};
				bool opening = false;
				if (SafeReadMenuEvent(&a_event, name, sizeof(name), opening)) {
					OnMenuEvent(name, opening);
				} else {
					// ★ 读到的东西不像菜单事件 ⇒ 记一笔；累计到阈值就停用解释。
					const auto bad = g_state.menuEvtBad.fetch_add(1, std::memory_order_relaxed) + 1;
					if (bad <= 3) {
						REX::WARN("menu event (bad): 负载不像 `MenuOpenCloseEvent`（第 {} 条）-> "
								  "若总数到 {} 会停用事件通道（退回轮询，绝不会因此崩）",
							bad, kMenuEvtBadMax);
					}
					if (bad >= kMenuEvtBadMax) {
						DisableMenuEventSink("事件负载连续不像菜单事件");
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		SasMenuEventSink g_menuEventSink;

		// ★ v4.12：**只停用解释，不调引擎注销**。
		//   为什么不去调 `UnregisterSink`（REL::ID 123822，commonlibsf 给的）：本轮离线核对
		//   发现它的实现形状与 `RegisterSink`（123821 = 锁 + sink 数组 push，已逐条对上）
		//   **不是同一套结构** —— 123822 在遍历一个 `[node+8]` 的链表，多半不是我们要的函数；
		//   而且在事件回调里注销自己还有重入 / 死锁风险。
		//   ⇒ 安全做法：把解释开关关掉（`ProcessEvent` 立刻返回），sink 留在数组里无害。
		void DisableMenuEventSink(const char* a_why)
		{
			if (!g_state.menuEvtActive) {
				return;
			}
			g_state.menuEvtActive     = false;
			g_cfg.containerMenuEvents = false;  // 本会话不再注册
			REX::WARN("menu events: 事件通道已停用解释（{}）-> 容器界面信号退回轮询（对 "
					  "ContainerMenu 可能读不到；此时看 `menu dump` 行取证）。sink 留在原地不动（无害）",
				a_why);
		}

		// ★ v4.12：找「UI 里那个 `MenuOpenCloseEvent` 事件源」——返回源地址，偏移写进 `a_outOff`。
		//   ★ 为什么不再写死偏移：**vtable 是类型的唯一指纹**。先试 commonlibsf 声明的
		//     `UI + 0x20`（本轮离线已用 UI 构造函数逐条证实），不对就沿 UI 对象每 8 字节往后
		//     找一个「首 qword == base+0x4D7E3D8」的槽位 —— 只有**精确等于**那个 vtable
		//     才算命中，所以即使将来布局位移，也绝不会误挂到别的事件源上（最坏 = 找不到 ⇒ 退回轮询）。
		std::uint8_t* FindMenuEventSource(std::size_t* a_outOff)
		{
			if (a_outOff) {
				*a_outOff = 0;
			}
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return nullptr;
			}
			const auto  expected = ModuleBase() + kMenuOpenCloseSourceVtblRva;
			auto* const b8       = reinterpret_cast<std::uint8_t*>(ui);
			// ① 首选：UI + 0x20（commonlibsf 基类偏移；本项目实测确认）
			if (*reinterpret_cast<const std::uintptr_t*>(b8 + kOffUiMenuEventSource) == expected) {
				if (a_outOff) {
					*a_outOff = kOffUiMenuEventSource;
				}
				return b8 + kOffUiMenuEventSource;
			}
			// ② 兜底：向后扫（先确认整段可读，一次 VirtualQuery）
			if (!IsReadable(b8, kMenuEvtSrcScanMax)) {
				return nullptr;
			}
			for (std::size_t off = 0x08; off + 8 <= kMenuEvtSrcScanMax; off += 8) {
				if (off == kOffUiMenuEventSource) {
					continue;
				}
				if (*reinterpret_cast<const std::uintptr_t*>(b8 + off) == expected) {
					if (a_outOff) {
						*a_outOff = off;
					}
					return b8 + off;
				}
			}
			return nullptr;
		}

		// vtable 对不上时的取证：把 UI 对象前 0x60 字节按 8 字节打成 RVA（最多两次）。
		//   ★ 只有这一行，就能和离线反汇编「构造函数写了什么 vtable」逐条对照 —— 本轮正是这么定位的。
		void LogUiLayoutFingerprint()
		{
			if (g_state.menuFingerprints >= 2) {
				return;
			}
			++g_state.menuFingerprints;
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}
			const auto* b8 = reinterpret_cast<const std::uint8_t*>(ui);
			if (!IsReadable(b8, 0x60)) {
				return;
			}
			const auto  base = ModuleBase();
			char        buf[512]{};
			std::size_t used = 0;
			for (std::size_t off = 0; off < 0x60; off += 8) {
				const auto q  = *reinterpret_cast<const std::uint64_t*>(b8 + off);
				const auto qq = (q >= base && q < base + 0x20000000) ? (q - base) : q;
				const int  n  = std::snprintf(buf + used, sizeof(buf) - used, "%s+0x%02zX=0x%llX",
					 off ? " " : "", off, static_cast<unsigned long long>(qq));
				if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(buf) - used) {
					break;
				}
				used += static_cast<std::size_t>(n);
			}
			REX::WARN("menu events: UI 布局指纹（VA 已换算成 RVA，便于和离线反汇编对照）: {}", buf);
		}

		// 「我们的 sink 还在不在那个 sink 数组里」——UI 重建 / 引擎清表时会掉出去。
		bool MenuSinkStillRegistered(const std::uint8_t* a_src)
		{
			if (!a_src) {
				return false;
			}
			const auto sz = *reinterpret_cast<const std::uint32_t*>(a_src + 0x08);
			const auto cp = *reinterpret_cast<const std::uint32_t*>(a_src + 0x0C);
			const auto dp = *reinterpret_cast<const std::uint64_t*>(a_src + 0x10);
			if (sz > cp || cp > 4096 || sz == 0) {
				return false;
			}
			if (!IsReadable(reinterpret_cast<const void*>(dp), static_cast<std::size_t>(sz) * 8)) {
				return false;
			}
			auto* const* sinks = reinterpret_cast<void* const*>(dp);
			for (std::uint32_t i = 0; i < sz; ++i) {
				if (sinks[i] == &g_menuEventSink) {
					return true;
				}
			}
			return false;
		}

		// 注册 sink（主线程、只做一次；失败每 4 秒重试 —— 与库存标定同一套「永不放弃」）。
		//   ★ v4.12：挂上之后**每 60 秒核对一次**「sink 还在数组里吗」，不在就重挂
		//     （防「UI 重建 / 引擎清表」把通道悄悄弄哑）。
		void EnsureMenuEventSink(std::uint64_t a_nowMs)
		{
			if (!g_cfg.containerMenuEvents) {
				return;
			}

			if (g_state.menuSinkRegistered) {
				if (a_nowMs - g_state.menuSinkCheckMs < kMenuSinkCheckMs) {
					return;
				}
				g_state.menuSinkCheckMs = a_nowMs;
				std::size_t off = 0;
				auto*       src = FindMenuEventSource(&off);
				if (src && MenuSinkStillRegistered(src)) {
					return;
				}
				g_state.menuSinkRegistered = false;  // 掉出去了 ⇒ 下面重挂
				REX::WARN("menu events: sink 不在事件源的 sink 数组里了（UI 重建 / 引擎清表？）-> 重新注册");
			}

			if (g_state.menuSinkRetryAtMs > a_nowMs) {
				return;
			}
			g_state.menuSinkRetryAtMs = a_nowMs + 4000;

			std::size_t off = 0;
			auto*       src = FindMenuEventSource(&off);
			if (!src) {
				++g_state.menuSinkFailures;
				REX::WARN("menu events: 在 UI 里找不到 vtable=0x{:X}（RVA 0x{:X} = BSTEventSource<MenuOpenCloseEvent>）"
						  "的槽位 -> **不注册**（退回轮询；第 {} 次失败，4 秒后重试）",
					ModuleBase() + kMenuOpenCloseSourceVtblRva, kMenuOpenCloseSourceVtblRva, g_state.menuSinkFailures);
				LogUiLayoutFingerprint();
				return;
			}
			// ★ 形状校验再注册（BSTEventSource：vtable@0 + BSTArray sinks@0x08：size/cap/data）
			const auto sz = *reinterpret_cast<const std::uint32_t*>(src + 0x08);
			const auto cp = *reinterpret_cast<const std::uint32_t*>(src + 0x0C);
			const auto dp = *reinterpret_cast<const std::uint64_t*>(src + 0x10);
			if (sz > cp || cp > 4096 || (sz != 0 && !IsReadable(reinterpret_cast<const void*>(dp), 8))) {
				++g_state.menuSinkFailures;
				REX::WARN("menu events: UI+0x{:X} 的形状不像事件源（size={} cap={} data={}) -> 退回轮询"
						  "（容器界面信号可能仍读不到；第 {} 次失败，4 秒后重试）",
					off, sz, cp, dp, g_state.menuSinkFailures);
				LogUiLayoutFingerprint();
				return;
			}
			// ★★ 硬比对 vtable：确认这个源就是 `BSTEventSource<MenuOpenCloseEvent>`
			//   （常量区有完整推导）。**不是它就不注册** —— 宁可退回轮询，也绝不把 sink
			//   挂到一个「别的事件」的源上（那样会按错误布局解释负载）。
			const auto vtbl     = *reinterpret_cast<const std::uintptr_t*>(src);
			const auto expected = ModuleBase() + kMenuOpenCloseSourceVtblRva;
			if (vtbl != expected) {
				++g_state.menuSinkFailures;
				REX::WARN("menu events: UI+0x{:X} 的 vtable=0x{:X}，期望 0x{:X}（BSTEventSource<MenuOpenCloseEvent>，"
						  "RVA 0x{:X}）-> **不注册**（退回轮询）",
					off, vtbl, expected, kMenuOpenCloseSourceVtblRva);
				LogUiLayoutFingerprint();
				return;
			}
			reinterpret_cast<RE::BSTEventSource<SasMenuOpenCloseEvent>*>(src)->RegisterSink(&g_menuEventSink);
			g_state.menuSinkRegistered = true;
			g_state.menuEvtActive      = true;
			g_state.menuSinkCheckMs    = a_nowMs;
			REX::INFO("menu events: sink registered at UI+0x{:X}（vtable=0x{:X} 已核对 = BSTEventSource<MenuOpenCloseEvent>；"
					  "sinks size={} cap={}）-> 菜单开/关事件将进日志（每个菜单名前 {} 条必记），"
					  "容器界面状态改由「事件 ∨ 轮询」驱动",
				off, vtbl, sz, cp, kMenuEvtPerNameLogMax);
		}

		// 「此刻开着的菜单」快照（诊断 / 自动学习用）—— 纯 `IsMenuOpen` 查询，零内存猜测。
		//   返回：开着的菜单名列表（写进 `a_out`，返回条数）。
		std::size_t SnapshotOpenMenus(std::string& a_out)
		{
			a_out.clear();
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return 0;
			}
			std::size_t n = 0;
			static std::vector<RE::BSFixedString> names;   // 只在主线程用（每帧最多一次）
			if (names.size() != kMenuNameCandidatesCount) {
				names.clear();
				names.reserve(kMenuNameCandidatesCount);
				for (std::size_t i = 0; i < kMenuNameCandidatesCount; ++i) {
					names.emplace_back(kMenuNameCandidates[i]);
				}
			}
			for (std::size_t i = 0; i < kMenuNameCandidatesCount; ++i) {
				if (!ui->IsMenuOpen(names[i])) {
					continue;
				}
				if (!a_out.empty()) {
					a_out += ", ";
				}
				a_out += kMenuNameCandidates[i];
				++n;
			}
			return n;
		}

		// 某个路径的文件存不存在（只用 WIN32，不引 <filesystem>）
		bool FileExists(const std::string& a_path)
		{
			if (a_path.empty()) {
				return false;
			}
			const auto attr = ::GetFileAttributesA(a_path.c_str());
			return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
		}

		// ★★★ v4.33 / v5.1：按引用记忆的落盘（定义见 ProbeFloraEngineState 之前）。
		//   前置声明必须在这里：LoadConfig 里会调用它（载入 = 配置文件读完之后）。
		void LoadFloraLearnTable();
		// ★★★ v5.1.7（订正 R7）：游戏时间（读档的时间指纹；定义见 FloraEntryInScope 之前）。
		//   前置声明必须在这里：LoadConfig 的启动日志会**当场探一次**（「现在读到的时间 = …」）。
		bool  ReadGameDays(float* a_out);
		float CurrentGameDays();  // 同上（1 秒缓存版；`RememberFloraBase` 等更早的定义要用）

		void LoadConfig()
		{
			// ★★ 需求（AGENTS.md，2026-09-25）：「**配置文件要放在和 esm 文件同级目录里**」。
			//   路径规则与日志完全一致（见 main.cpp 的 EsmDir()）：DLL 目录（SFSE\Plugins）
			//   上溯两级 = `<游戏>\Data`（MO2 下 = mod 目录根）⇒ 就是
			//   `StarfieldAlwaysScan.esm` 旁边。
			//   ★ 兼容老安装：新位置**不存在**时回退到老位置（DLL 旁边），并打一条 WARN
			//     提示搬过去 —— 免得升级用户「改了 INI 却怎么都不生效」。
			{
				const std::string dllDir = ModuleDir();
				std::string       esmDir;
				if (const auto cut1 = dllDir.find_last_of('\\'); cut1 != std::string::npos && cut1 > 0) {
					if (const auto cut2 = dllDir.find_last_of('\\', cut1 - 1); cut2 != std::string::npos) {
						esmDir = dllDir.substr(0, cut2);
					}
				}
				const std::string newIni = esmDir.empty() ? std::string{} : esmDir + "\\SAS_AlwaysScan.ini";
				const std::string oldIni = dllDir + "\\SAS_AlwaysScan.ini";
				if (FileExists(newIni)) {
					g_iniPath = newIni;
				} else if (FileExists(oldIni)) {
					g_iniPath = oldIni;
					REX::WARN("config file: 没找到「和 esm 同级」的 SAS_AlwaysScan.ini（{}）"
							  "—— 回退到老位置 {}，建议把配置文件搬到 esm 旁边",
						newIni, oldIni);
				} else {
					g_iniPath = newIni.empty() ? oldIni : newIni;  // 两个都没有：报新位置（排查更有用）
				}
			}
			const char* ini = g_iniPath.c_str();
			REX::INFO("config file: {}（读不到就全部用内置默认值，高亮照常工作）", g_iniPath);

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
			// ★★★ v4.23：举着原版扫描仪时「星球扫描目标」类别让位（理由见 Config）
			g_cfg.yieldTargetsWhileScanning = getInt("YieldTargetsWhileScanning", 1) != 0;
			// ★★★ v4.25：星球目标「已扫描 ⇒ 也用原版那个绿」（放下扫描仪之后）
			g_cfg.floraScannedByResource = getInt("FloraScannedByResource", 1) != 0;
			// ★★★ v4.28：主判据 = 引擎自己的 `GetOutlineState(ref) == 2`（IsScanned）
			g_cfg.floraScannedByEngineState = getInt("FloraScannedByEngineState", 1) != 0;
			g_cfg.stateFloraScanned      = std::clamp(getInt("StateFloraScanned", 5), 0, 11);
			g_cfg.floraScanProbeMax      = std::clamp(getInt("FloraScanProbeMax", 8), 0, 64);
			// ★★★ v4.33：「低概率变青」二次加固的三个键（详见 Config 里 v4.33 段）
			g_cfg.floraLearnPersist     = getInt("FloraLearnPersist", 1) != 0;
			g_cfg.floraLearnClearOnLoad = getInt("FloraLearnClearOnLoad", 0) != 0;
			g_cfg.floraUnscannedTtlMs   = std::clamp(getInt("FloraUnscannedTtlMs", 5000), 0, 30000);
			// ★★★ v5.1.5（订正 R5）：「已扫描」记忆按**存档**隔离（详见 Config 里 v5.1.5 段）
			g_cfg.floraMemoryScope      = std::clamp(getInt("FloraMemoryScope", 1), 0, 2);
			// ★★★ v5.1.5（订正 R5）：按物种扩散只在**同一颗星球**内（同上）
			//   ★ v5.1.8（订正 R8）：默认改成 **0**（引擎口径 = 全局物种表；见 Config 里那段证据）
			g_cfg.floraSpeciesPlanetScope = getInt("FloraSpeciesPlanetScope", 0) != 0;
			// ★★★ v5.1.6（订正 R6）：「读档边界」证据驱动（详见 Config 里 v5.1.6 段）
			g_cfg.floraLoadBoundaryEvidence = getInt("FloraLoadBoundaryEvidence", 1) != 0;
			// ★★★ v5.1.7（订正 R7）：读档边界 = 游戏时间指纹（详见 Config 里 v5.1.7 段）
			g_cfg.floraSaveFingerprint   = getInt("FloraSaveFingerprint", 1) != 0;
			// ★★★ v5.2：植物「已扫描」直读引擎扫描进度表（详见 Config 里 v5.2 段）
			g_cfg.floraEngineProgress    = getInt("FloraEngineProgress", 1) != 0;
			// ★★★ v5.2：自建记忆层总开关（默认 0 = 抛弃记忆；详见 Config 里 v5.2 段）
			g_cfg.floraUseMemory         = getInt("FloraUseMemory", 0) != 0;
			g_cfg.unhighlightGraceMs = std::clamp(getInt("UnhighlightGraceMs", 1500), 0, 60000);
			g_cfg.maxOutlineOpsPerScan = std::clamp(getInt("MaxOutlineOpsPerScan", 64), 0, 4096);

			// ★ v4.7：外景连续性 / 高亮丢失自愈（背景见常量区长注释）
			g_cfg.exteriorContinuous  = getInt("ExteriorContinuous", 1) != 0;
			g_cfg.settleOnCellCrossMs = std::clamp(getInt("SettleOnCellCrossMs", static_cast<int>(kSettleOnCellCrossMs)), 0, 2000);
			g_cfg.streamJumpTolerance = std::clamp(getInt("StreamJumpTolerance", static_cast<int>(kRefsStreamJumpMax)), 0, 65536);
			g_cfg.verify3DPerScan     = std::clamp(getInt("Verify3DPerScan", static_cast<int>(kVerify3DPerScan)), 0, 1024);
			// ★★★ 2026-09-27（订正 R3，性能）：环内 cell 分片遍历（定义见 Config 注释）。
			//   读取后立刻按「分片周期必须 < 宽限期」钳出**有效片数**（防目标闪烁）：
			//   周期 = eff × ScanIntervalMs，宽限期 = UnhighlightGraceMs，留一整轮余量。
			//   例：默认 1500 / 200 ⇒ 上限 6 ⇒ eff = min(5, 6) = 5（周期 1.0s < 1.5s ✓）；
			//       若用户把 UnhighlightGraceMs 调小到 500 ⇒ eff 自动落到 1（不分片）。
			g_cfg.ringSliceMaxRounds =
				std::clamp(getInt("RingSliceMaxRounds", static_cast<int>(kRingSliceMaxRounds)), 0, 64);
			// ★★★ 订正 R4（性能）：热路径去内核化 + 判空缓存（定义与完整推导见 Config）。
			g_cfg.fastReadMem = getInt("FastReadMem", 1) != 0;
			g_cfg.lootCacheTtlMs = std::clamp(getInt("LootCacheTtlMs", 1500), 0, 60000);
			// 直读自检（一次性）：不可读内存必须能被安全捕获 ⇒ 否则自动退回内核读。
			//   把「有问题」留在启动日志里，而不是留到游戏里崩 / 卡。
			if (g_cfg.fastReadMem) {
				std::uint64_t sehUs = 0, rpmUs = 0;
				if (!SelfTestFastRead(&sehUs, &rpmUs)) {
					g_cfg.fastReadMem = false;
					REX::WARN("saferead selftest: 直读（SEH）**不可用**"
							  "（读到不可读内存时没能安全返回 false）-> 自动退回内核读"
							  "（等价的旧行为；性能会差一些）。把这一行发出来即可定位。");
				} else {
					REX::INFO("saferead selftest: 直读可用（可读 ✓ / 不可读安全捕获 ✓）；"
							  "200 次耗时 直读={}µs vs 内核读={}µs（{}倍）-> FastReadMem 生效",
						sehUs, rpmUs, sehUs ? std::max<std::uint64_t>(1, rpmUs / std::max<std::uint64_t>(1, sehUs)) : 0);
				}
			}
			{
				const auto iv    = static_cast<std::uint32_t>(std::max(1, g_cfg.scanIntervalMs));
				const auto grace = static_cast<std::uint32_t>(std::max(0, g_cfg.unhighlightGraceMs));
				std::uint32_t maxByGrace = grace > iv ? (grace - iv) / iv : 0;
				if (maxByGrace < 1) {
					maxByGrace = 1;  // 极窄宽限期 / 分片关闭：1 = 每轮全扫（旧行为）
				}
				std::uint32_t eff = static_cast<std::uint32_t>(std::max(0, g_cfg.ringSliceMaxRounds));
				if (eff == 0) {
					eff = 1;  // 0 = 关（与 1 同义）
				}
				if (eff > maxByGrace) {
					eff = maxByGrace;
				}
				g_cfg.ringSliceRoundsEff = eff;
			}
			// ★★★ v4.29：放下扫描仪后的「恢复提速」（见常量区 kResyncBoostMs）
			g_cfg.resyncBoostMs     = static_cast<std::uint64_t>(
				std::clamp(getInt("ResyncBoostMs", static_cast<int>(kResyncBoostMs)), 0, 30000));
			g_cfg.resyncBoostBudget = static_cast<std::uint32_t>(
				std::clamp(getInt("ResyncBoostBudget", static_cast<int>(kResyncBoostBudget)), 0, 4096));

			// --- ★ v4.0：分类分色（每个类别一个 outline 状态 0..11）---
			//   （★ v4.2 追加 corpse，默认 9 = 与容器同色，理由见 Config::stateByCategory）
			//   （★ v4.17 追加 5 个物品栏子类 —— 武器 / 服饰 / 弹药救援 / 笔记 / 资源）
			const char* const kStateKeys[kCategoryCount] = {
				"StateLoot", "StateWeapon", "StateApparel", "StateAmmoAid", "StateNote", "StateResource",
				"StateContainer", "StateDevice", "StateDoor", "StateFlora", "StateOther", "StateCorpse"
			};
			// ★★★ v4.23：这张表必须与 Config::stateByCategory 的默认值**逐项一致**
			//   （INI 缺键时用的就是它；v4.22 忘了同步 resource/other，这次一并订正：
			//    resource 7→3、flora 5→7、other 3→2）。
			//   ★★★ v4.31：整套重排（NPC 原色 ⇒ 容器 / 尸体橙、门白必须回来，
			//    代价是把「武器红 / 服饰品红 / 笔记黄」里最小的两个让位 ——
			//    完整推理见 Config::stateByCategory 的 v4.31 段）：
			//    武器 9→**10**、服饰 10→**1**、笔记 6→**0**、容器 1→**9**、
			//    门 0→**6**、尸体 1→**9**。
			//   ★★★ v4.32：分组配色定稿 —— **服饰 1→10**（并进武器红组）、
			//    **笔记 0→3**（并进资源紫组）；其余不动（见 v4.32 段）。
			const int kStateDef[kCategoryCount] = { 2, 10, 10, 5, 3, 3, 9, 4, 6, 7, 2, 9 };
			for (std::size_t i = 0; i < kCategoryCount; ++i) {
				g_cfg.stateByCategory[i] = std::clamp(getInt(kStateKeys[i], kStateDef[i]), 0, 11);
			}

			// --- ★ v4.1：类别开关（默认只有 kOther = MSTT 关着，理由见 Config 里的长注释）---
			{
				const char* const kEnableKeys[kCategoryCount] = {
					"EnableLoot", "EnableWeapon", "EnableApparel", "EnableAmmoAid", "EnableNote", "EnableResource",
					"EnableContainer", "EnableDevice", "EnableDoor", "EnableFlora", "EnableOther", "EnableCorpse"
				};
				const int kEnableDef[kCategoryCount] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1 };
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
			// ★ v4.10：展示柜「拿空即灭」（默认 1；理由与判据见常量区「v4.10」）
			g_cfg.displayCaseUiEmpty = getInt("DisplayCaseUiEmpty", 1) != 0;
			//   容器界面菜单名（默认 "ContainerMenu"；游戏自带脚本实证）
			{
				char menuName[64]{};
				::GetPrivateProfileStringA("General", "ContainerMenuName", "ContainerMenu",
					menuName, sizeof(menuName), ini);
				if (menuName[0] == '\0') {
					std::snprintf(menuName, sizeof(menuName), "%s", "ContainerMenu");
				}
				std::snprintf(g_cfg.containerMenuName, sizeof(g_cfg.containerMenuName), "%s", menuName);
			}
			// ★ v4.11：容器界面信号 —— 引擎菜单事件通道 + 自愈学习 + 逐帧观察
			//   （全部理由见 Config 里的说明与常量区「v4.11」长注释）
			g_cfg.containerMenuEvents     = getInt("ContainerMenuEvents", 1) != 0;
			g_cfg.containerMenuLearn      = getInt("ContainerMenuLearn", 1) != 0;
			g_cfg.menuDumpMax             = std::clamp(getInt("MenuDumpMax", static_cast<int>(kMenuDumpMax)), 0, 64);
			g_cfg.menuEventLogMax         = std::clamp(getInt("MenuEventLogMax", static_cast<int>(kMenuEventLogMaxDefault)), 0, 4096);
			g_cfg.displayCaseTraceMax     = std::clamp(getInt("DisplayCaseTraceMax", static_cast<int>(kDisplayCaseTraceMaxDefault)), 0, 256);
			g_cfg.displayCaseFrameWatch   = getInt("DisplayCaseFrameWatch", 1) != 0;
			// ★ v4.13：游戏事件通道（理由 / 离线取证见常量区「v4.13」长注释）
			g_cfg.containerLootEvents          = getInt("ContainerLootEvents", 1) != 0;
			g_cfg.displayCaseQuickOpenEmpty    = getInt("DisplayCaseQuickOpenEmpty", 0) != 0;  // v4.14 默认关（误灭面见 Config 注释）
			g_cfg.lootEventLogMax              = std::clamp(getInt("LootEventLogMax", 32), 0, 4096);
			REX::INFO("config: corpseUnconscious={} corpseLifeState={} corpseBleedout={} skipEmptyLoot={}",
				g_cfg.corpseUnconscious, g_cfg.corpseLifeState, g_cfg.corpseBleedout, g_cfg.skipEmptyLoot);
			REX::INFO("config: actorProbeMax={} actorChangeProbeMax={} lootProbeMax={} contProbeMax={} treatNullInvAsEmpty={} skipNonPlayableLoot={} skipEquippedLoot={} skipDisplayCaseEmpty={}",
				g_cfg.actorProbeMax, g_cfg.actorChangeProbeMax, g_cfg.lootProbeMax, g_cfg.contProbeMax,
				g_cfg.treatNullInvAsEmpty, g_cfg.skipNonPlayableLoot, g_cfg.skipEquippedLoot,
				g_cfg.skipDisplayCaseEmpty);
			REX::INFO("config: displayCaseUiEmpty={} containerMenu=\"{}\" -> 展示柜拿空判据={}",
				g_cfg.displayCaseUiEmpty, g_cfg.containerMenuName,
				(g_cfg.skipDisplayCaseEmpty && g_cfg.displayCaseUiEmpty) ? "同一段打开期读到空即熄灭" : "关闭也亮（v4.9 行为）");
			REX::INFO("config: containerMenuEvents={} containerMenuLearn={} menuDumpMax={} menuEventLogMax={} "
					  "-> 容器界面信号 = 菜单事件 ∨ 轮询（v4.10 的轮询对 ContainerMenu 实测读不到）"
					  "；事件源 = UI+0x{:X} 且 vtable RVA 必须是 0x{:X}（v4.12 已用 UI 构造函数逐条证实）",
				g_cfg.containerMenuEvents, g_cfg.containerMenuLearn, g_cfg.menuDumpMax, g_cfg.menuEventLogMax,
				kOffUiMenuEventSource, kMenuOpenCloseSourceVtblRva);
			REX::INFO("config: displayCaseFrameWatch={} displayCaseTraceMax={} -> 展示柜逐帧观察={}",
				g_cfg.displayCaseFrameWatch, g_cfg.displayCaseTraceMax,
				g_cfg.displayCaseFrameWatch ? "开（拿空窗口缩到 1 帧）" : "关（只在 200ms 扫描节拍上判）");
			REX::INFO("config: containerLootEvents={} displayCaseQuickOpenEmpty={} lootEventLogMax={} "
					  "-> 展示柜拿空判据 = 记账（TESContainerChangedEvent 减账到 0）{}；"
					  "事件源 = 静态对象 RVA 0x{:X}/0x{:X}（vtable 0x{:X}/0x{:X} 注册前硬核对）",
				g_cfg.containerLootEvents, g_cfg.displayCaseQuickOpenEmpty, g_cfg.lootEventLogMax,
				g_cfg.displayCaseQuickOpenEmpty
					? " + 兜底（QuickContainerOpenedEvent + 连续读到空；★ v4.14 起非默认——会误灭「打开不拿就关掉」）"
					: "（兜底已关＝v4.14 默认：只信记账，避免「打开不拿就关掉」被误灭）",
				kInvEvtSourceRva, kQuickOpenSourceRva, kInvEvtSourceVtblRva, kQuickOpenSourceVtblRva);

			// --- ★ v4.0.1：可选的自定义类别颜色（ColorLoot=RRGGBB …，留空 = 用引擎原生配色）---
			//   ★ v4.17：`ColorNote` / `ColorResource` **留空时保留结构体里的默认值**
			//     （黄 FFD700 / 紫 AA6EFF）—— 这两个类别挑的 state 原生色不可用，
			//     必须覆盖；其余类别的默认值仍是 kColorUnset（= 不覆盖）。
			{
				const char* const kColorKeys[kCategoryCount] = {
					"ColorLoot", "ColorWeapon", "ColorApparel", "ColorAmmoAid", "ColorNote", "ColorResource",
					"ColorContainer", "ColorDevice", "ColorDoor", "ColorFlora", "ColorOther", "ColorCorpse"
				};
				for (std::size_t i = 0; i < kCategoryCount; ++i) {
					char hex[32]{};
					::GetPrivateProfileStringA("General", kColorKeys[i], "", hex, sizeof(hex), ini);
					char* p = hex;
					if (*p == '#') {
						++p;
					}
					if (*p == '\0') {
						continue;  // 没写 = 保持默认（多数类别 = 不覆盖）
					}
					char*        end = nullptr;
					const auto   v   = std::strtoul(p, &end, 16);
					if (end == p || v > 0xFFFFFFu) {
						REX::WARN("config: {}='{}' 解析失败（要 6 位十六进制，例如 ColorLoot=FF8000）-> 忽略",
							kColorKeys[i], p);
						continue;
					}
					g_cfg.colorOverride[i] = static_cast<std::uint32_t>(v) & 0xFFFFFFu;
				}
				// 打「实际生效」的覆盖表（含代码默认值），配色出问题时一眼对上
				std::string colorLog;
				for (std::size_t i = 0; i < kCategoryCount; ++i) {
					if (g_cfg.colorOverride[i] == kColorUnset) {
						continue;
					}
					if (!colorLog.empty()) {
						colorLog += ", ";
					}
					colorLog += kCategoryName[i];
					colorLog += "=#";
					char hexv[8];  // 不叫 buf：外层那个 buf[64] 是 getFloat 用的（C4456 遮蔽警告）
					std::snprintf(hexv, sizeof(hexv), "%06X", g_cfg.colorOverride[i] & 0xFFFFFFu);
					colorLog += hexv;
				}
				REX::INFO("config: colorOverride（实际生效）: {}",
					colorLog.empty() ? "（无 —— 全部用引擎原生配色）" : colorLog);
			}

			// ★★★ v5.0：完全自建颜色通道（总开关 + 「植物已扫描」通道色；见 docs/32）
			g_cfg.channelMode = getInt("ChannelMode", 1) != 0;
			{
				char hex[32]{};
				::GetPrivateProfileStringA("General", "ColorFloraScanned", "", hex, sizeof(hex), ini);
				char* p = hex;
				if (*p == '#') {
					++p;
				}
				if (*p != '\0') {
					char*        end = nullptr;
					const auto   v   = std::strtoul(p, &end, 16);
					if (end == p || v > 0xFFFFFFu) {
						REX::WARN("config: ColorFloraScanned='{}' 解析失败（要 6 位十六进制）-> 用内置绿 #27C684", p);
					} else {
						g_cfg.colorFloraScanned = static_cast<std::uint32_t>(v) & 0xFFFFFFu;
					}
				}
				REX::INFO("config: channelMode={} -> {}",
					g_cfg.channelMode,
					g_cfg.channelMode
						? "★ v5.0 自建颜色通道：13 条独立通道（不写状态表 / 不覆盖引擎配色块；"
						  "原版扫描仪 / NPC / 星球目标颜色 100% 原版）；"
						  "引擎签名不符或模板标定失败时自动回退旧路径"
						: "旧路径（v4.33 行为：借 state 槽位 + 覆盖引擎配色块）");
			}

			// --- ★★ v4.20：覆盖不透明度（AlphaXxx，0~255；0 = 保留引擎原值）---
			//   动机与写法见 Config::colorAlpha 的长注释（用户：门/武器/防具「覆盖太深」）。
			//   日志只列非 0 的（非 0 = 真的在改不透明度；0 = 与 v4.19 行为一致）。
			{
				const char* const kAlphaKeys[kCategoryCount] = {
					"AlphaLoot", "AlphaWeapon", "AlphaApparel", "AlphaAmmoAid", "AlphaNote", "AlphaResource",
					"AlphaContainer", "AlphaDevice", "AlphaDoor", "AlphaFlora", "AlphaOther", "AlphaCorpse"
				};
				std::string alphaLog;
				for (std::size_t i = 0; i < kCategoryCount; ++i) {
					const auto v = std::clamp(getInt(kAlphaKeys[i], static_cast<int>(g_cfg.colorAlpha[i])), 0, 255);
					g_cfg.colorAlpha[i] = static_cast<std::uint8_t>(v);
					if (!v) {
						continue;  // 0 = 保留引擎原值（默认；不打进日志，免得噪音）
					}
					if (!alphaLog.empty()) {
						alphaLog += ", ";
					}
					alphaLog += kCategoryName[i];
					alphaLog += '=';
					alphaLog += std::to_string(v);
					alphaLog += " (" + std::to_string((v * 100 + 127) / 255) + "%)";
				}
				REX::INFO("config: colorAlpha（覆盖不透明度，0=保留引擎原值，255=不透明）: {}",
					alphaLog.empty() ? "（全部保留引擎原值 —— 覆盖强度与 v4.19 相同）" : alphaLog);
			}

			// --- ★★ v4.21：「不填充」总开关（基色 alpha=0 ⇒ 物品材质透出）---
			//   动机/证据见 Config::noFill 的长注释（v4.20 的 alpha 实测「逐像素无变化」）。
			g_cfg.noFill = getInt("NoFill", 1) != 0;
			REX::INFO("config: noFill={} -> 描边基色（+0x80）写 {}（{}）",
				g_cfg.noFill,
				g_cfg.noFill ? "RGB + alpha=0" : "RGB + 引擎原 alpha（alpha=0 时补 0xFF）",
				g_cfg.noFill ? "不填充：只有彩色轮廓，物品材质透出（v4.21 默认）"
							 : "填充：v4.19/v4.20 的实心覆盖行为");

			// ★ v4.19：渲染侧参数探针（把「渲染器实际收到的每状态基色/脉冲色」打进日志）
			g_cfg.rendererProbe = getInt("RendererProbe", 1) != 0;
			REX::INFO("config: rendererProbe={} -> 每个会话最多打 3 次 `renderer params[...]`"
					  "（基色 = 真正画出来的颜色；排「颜色没生效」时看它）",
				g_cfg.rendererProbe);

			// ★★★ v4.24：管理器占用探针（举着扫描仪 1.5s 后打 11 个管理器的元素数）
			g_cfg.managerOccupancyProbe = getInt("ManagerOccupancyProbe", 1) != 0;
			REX::INFO("config: managerOccupancyProbe={} -> 每次「举起扫描仪」1.5 秒后打一行 "
					  "`manager occupancy[...]`（11 个 state 各自的元素数；直接看引擎在写哪些槽位，"
					  "每会话最多 6 次）",
				g_cfg.managerOccupancyProbe);

			// ★ v4.17：「资源」判据（MISC 的 ResourceType* 关键词）总开关
			g_cfg.resourceByKeyword = getInt("ResourceByKeyword", 1) != 0;
			REX::INFO("config: resourceByKeyword={} -> 资源判据{}（MISC 记录上的 ResourceType* 关键词；"
					  "★★ v4.23 已订正 BSTArray 布局，日志里有 `关键词数组标定 = base+0x…` 一行可核对）",
				g_cfg.resourceByKeyword, g_cfg.resourceByKeyword ? "启用" : "关闭（全部 MISC 归杂项）");

			// ★★★ v4.23：举着原版扫描仪时「星球扫描目标」类别让位原版（理由见 Config）
			REX::INFO("config: yieldTargetsWhileScanning={} -> 举着扫描仪（MonocleMenu）时，"
					  "「植物 / 矿脉 / 气泉 / 液池」（state {} = 原版扫描目标槽位）{}",
				g_cfg.yieldTargetsWhileScanning,
				g_cfg.stateByCategory[static_cast<std::size_t>(Category::kFlora)],
				g_cfg.yieldTargetsWhileScanning
					? "不挂、不重申、不摘（原版自己的 扫描前/扫描后/正在扫描 颜色原样保留；"
					  "放下扫描仪后自动整批重挂）；其余类别照常（圆圈外也高亮不受影响）"
					: "照常重申（= v4.22 行为，会盖掉原版刚写的状态）");

			// ★★★ v4.24：state 4 / 5 归还引擎 —— 用户实测「扫描后没有变成原版扫描后的
			//   绿色」的真根因：引擎的扫描求值函数（0x159ED90）对**已经扫描过的星球目标**
			//   （矿石/气体/液体/植物：base=FLOR → produceItem → MISC → BGSResource(IRES)
			//   → 「该资源已扫描」⇒ `add edx,4`）写 **state 4（远）/ 5（近）**，
			//   而这两个槽位的原生色 = **绿色 #27C684**（= 用户说的那个绿）。
			//   详证见 Config 配色数组上方的长注释 / `docs/23`。
			REX::INFO("config: state4_5 归还引擎 -> 「设备」(state {}) / 「弹药救援」(state {}) "
					  "的颜色**不再覆盖**（= 引擎原生绿 #27C684）：引擎用这两个槽位画"
					  "「已扫描的星球目标」（近 = 5 / 远 = 4）⇒ 举着扫描仪时看到的是原版色",
				g_cfg.stateByCategory[static_cast<std::size_t>(Category::kDevice)],
				g_cfg.stateByCategory[static_cast<std::size_t>(Category::kLootAmmoAid)]);

			// ★★★ v4.32：分组配色定稿（用户需求 `颜色分类.md`）—— 只并组、不新增：
			//   红 = 武器 / 投掷物 / 太空服 / 背包 / 头盔 / 服饰（state 10）、
			//   橙 = 容器 / 尸体（state 9）、紫 = 笔记 / 资源（state 3）、
			//   绿 = 弹药 / 救援（state 5，原版绿）、白 = 门（state 6）；
			//   杂项 = 2（原版蓝）、植物 / 矿石 = 7（原版青）、NPC / 星球目标不动。
			//   覆盖槽位仍是 **5 个（2/3/6/9/10）**、覆盖色一字未变 ⇒ 零新风险。
			REX::INFO("config: 配色分组(v4.32) -> 「武器 / 投掷物 / 太空服 / 背包 / 头盔 / 服饰」"
					  "= state {}（红 #FF2E2E）、「容器 / 尸体」= {}（橙 #FF9500）、"
					  "「笔记 / 资源」= {}（紫 #B36BFF）、「弹药 / 救援」= {}（原版绿）、"
					  "「门」= {}（白 #FFFFFF）；杂项 = {}（原版蓝）、0/1/4/5/7/8 仍归还引擎",
				g_cfg.stateByCategory[static_cast<std::size_t>(Category::kLootWeapon)],
				g_cfg.stateByCategory[static_cast<std::size_t>(Category::kContainer)],
				g_cfg.stateByCategory[static_cast<std::size_t>(Category::kLootResource)],
				g_cfg.stateByCategory[static_cast<std::size_t>(Category::kLootAmmoAid)],
				g_cfg.stateByCategory[static_cast<std::size_t>(Category::kDoor)],
				g_cfg.stateByCategory[static_cast<std::size_t>(Category::kLoot)]);

			// ★★★ v4.25 / v4.28：星球目标「已扫描」⇒ 也用原版那个绿（放下扫描仪之后）
			{
				const int floraState = g_cfg.stateByCategory[static_cast<std::size_t>(Category::kFlora)];
				// ★ 注意：这条日志在 ResolveNativeOutline 之前打，所以这里只看**配置**；
				//   「引擎函数到底拿到没有」由后面 `flora scanned: …` 那几行报告。
				const bool judgeOff = !g_cfg.floraScannedByEngineState && !g_cfg.floraScannedByResource;
				const std::string how = judgeOff
					? "全部用 StateFlora（= v4.24 行为，扫没扫过看起来一样）"
					: (g_cfg.stateFloraScanned == floraState
							? "两种状态是同一个值 ⇒ 观感同 v4.24（等于把这项关掉）"
							: "**已扫描过的**用 state " + std::to_string(g_cfg.stateFloraScanned) +
								  "（原版「已扫描」绿 #27C684）、没扫描过的用 state " +
								  std::to_string(floraState) +
								  "（原版青色脉冲）；主判据 = 引擎自己的 `GetOutlineState(ref)`（IsScanned），"
								  "资源链只对「产出物品是 LVLI」的（矿脉 / 气泉 / 液池）生效 —— 日志里搜 `flora scan:` 可核对");
				REX::INFO("config: floraScannedByEngineState={} floraScannedByResource={} stateFloraScanned={} -> "
						  "「植物 / 矿脉 / 气泉 / 液池」放下扫描仪后：{}",
					g_cfg.floraScannedByEngineState, g_cfg.floraScannedByResource,
					g_cfg.stateFloraScanned, how);
			}

			// ★★★ v4.33：「已扫描植物低概率变青」二次加固（证据链见 docs/31）
			//   ★★★ v5.1：记忆粒度改成**引用**（docs/33）；读取引擎状态表改成**只读**。
			REX::INFO("config: floraLearnPersist={} floraLearnClearOnLoad={} floraUnscannedTtlMs={} -> "
					  "按**引用**的单向记忆：{}；换场景 / 读档{}清空；判成「未扫描」的判据缓存 TTL = {}ms"
					  "（引擎状态一旦恢复「已扫描」，最多这么久变绿）",
				g_cfg.floraLearnPersist, g_cfg.floraLearnClearOnLoad, g_cfg.floraUnscannedTtlMs,
				g_cfg.floraLearnPersist ? "落盘（行格式 `引用 base`；文件不存在 = 首次运行）" : "只在内存里",
				g_cfg.floraLearnClearOnLoad ? "会" : "**不**",
				g_cfg.floraUnscannedTtlMs > 0 ? g_cfg.floraUnscannedTtlMs : 30000);
			//   （这里用字面量 RVA：常量区在文件的后面，见 kRvaOutlineStateTree 那段证据）
			REX::INFO("config: 星球目标记忆(v5.1 / 订正 R2) -> 引用级记忆 + **按物种（base）扩散**："
					  "任一实例被权威确认「已扫描」（引擎画过 4/5 ∨ 引擎状态==2 ∨ 资源链命中）⇒ "
					  "同 species / 同资源的**所有**实例一起变绿（引擎知识库本来就是这一级）；"
					  "★ 物种表**不落盘**（避免跨存档外溢），重启后靠引用级落盘记忆**动态激活**；"
					  "引擎状态表探针 = **只读走树（RVA 0x5F39CE0 + 节点 +0x20/+0x28）**，"
					  "不再调 LookupOrAdd（那会插入条目 + 对 REFR 加引用计数 ⇒ 玩得越久越卡）；"
					  "「青」记忆只当提示（不再短路主判据 / 资源链，任何一条给出「已扫描」都会升级成绿）");
			// ★★★ v5.1.5（订正 R5）→ ★★★ v5.1.8（订正 R8）：扩散的星球口径
			REX::INFO("config: floraSpeciesPlanetScope={} -> 按物种（base）扩散的星球口径：{}"
					  "（作用域由「存档边界」管：物种表会话级不落盘、读档按**游戏时间锚点**剪枝、"
					  "落盘记忆只在作用域内才播种 —— 见 docs/41）",
				g_cfg.floraSpeciesPlanetScope,
				g_cfg.floraSpeciesPlanetScope
					? "**R5 严格口径**：只在同一颗 worldspace 内扩散（跨星球一律拒绝 —— 会把"
					  "「本存档里已扫描、但见证发生在别的星球」的目标误判成青色；只为对照 / 回退）"
					: "★ **引擎口径（默认，v5.1.8）**：物种知识**跨星球**生效 —— 引擎自己"
					  "（一举扫描仪）会给「本存档里扫过的 species」在**任何**星球画绿 4/5；"
					  "我们只要在作用域内见证过该物种就整片变绿（`跨星球放行=` 计数可见）");
			REX::INFO("config: floraMemoryScope={} -> 「已扫描」记忆按存档隔离（读档 = 存档边界，"
					  "用引擎 `TESLoadGameEvent` 当**候选信号**；★ v5.1.6 起还要**资源链证据**成立才作废"
					  "——部分传送 / 换世界空间也会发这个事件）：{}",
				g_cfg.floraMemoryScope,
				g_cfg.floraMemoryScope == 0
					? "**旧行为**（记忆永远有效、物种表不清）—— 假绿会回来，只为对照"
					: (g_cfg.floraMemoryScope == 1
								? "本会话**第一次**读档仍信任落盘记忆（重开游戏继续玩同一存档 = "
								  "扫过的目标不用再开一遍扫描仪）；之后再遇到读档事件时：**链复核有证伪**"
								  "才推进作用域 / 清物种表 / 丢假绿，一个都证伪不了 ⇒ 判定「传送 / 读同一存档」"
								  "⇒ 记忆保持有效（★ v5.1.6）"
								: "**最严格**：落盘记忆只当提示（连第一次读档也不认），一律要求"
								  "「本存档内当场见证」（不依赖证据、每次事件都推进 —— 假绿投诉仍存在时的对照开关）"));
			// ★★★ v5.1.6（订正 R6）：读档边界 = 证据驱动（详见 Config 里 v5.1.6 段）
			REX::INFO("config: floraLoadBoundaryEvidence={} -> 「读档边界」的判定口径：{}；"
					  "复核延后 {}ms（等到世界稳定）后才做；日志里搜 `读档候选` / `读档 #` 可核对",
				g_cfg.floraLoadBoundaryEvidence,
				(g_cfg.floraLoadBoundaryEvidence && g_cfg.floraMemoryScope == 1)
					? "★ 证据驱动（默认）—— 资源链复核**一个 base 都证伪不了** ⇒ 判定「传送 / 读同一存档」"
					  "⇒ 记忆保持有效（治用户实测的「传送后已扫描物品变青」）；有证伪才是真读档"
					: "**v5.1.5 旧行为**（事件即边界：无条件推进作用域 / 清物种表 / 丢条目）—— 只为对照",
				kFloraScopeEvalDelayMs);
			// ★★★ v5.1.7（订正 R7）：读档边界 = **游戏时间指纹**（详见 Config 里 v5.1.7 段 / docs/40）
			{
				float probeDays = 0.0f;
				const bool timeOk = ReadGameDays(&probeDays);
				REX::INFO("config: floraSaveFingerprint={} -> ★ 读档边界 = **游戏时间指纹**"
						  "（`Calendar::gameDaysPassed`）：条目自带「学习时刻」（落盘行第 3 字段），"
						  "读档时把本存档的游戏时间当**锚点** —— 学习时刻 ≤ 锚点 ⇒ 记忆有效；"
						  "> 锚点 ⇒ 不在本存档作用域（**不删条目**，换更新的存档就回来）；"
						  "传送 / 继续同一存档 ⇒ 锚点几乎不动 ⇒ 什么都不作废（本次修复的核心）；"
						  "0 = 回退到 v5.1.6 的链证据路径。"
						  "★ 现在读到的时间 = {}（Calendar 反汇编证据见常量区 / docs/40）",
					g_cfg.floraSaveFingerprint,
					timeOk ? std::to_string(probeDays) + " 天"
						   : std::string("此刻读不到（启动时还没进世界 —— 正常；读档时仍读不到才会回退）"));
			}
			// ★★★ v5.2：植物「已扫描」= 直读引擎扫描进度表（详见 Config 里 v5.2 段）
			REX::INFO("config: floraEngineProgress={} -> ★ 植物「已扫描」直读引擎扫描进度表"
					  "（`PlayerKnowledge` 物种槽 percent == 100 = 引擎写 4/5 的同一依据）："
					  "key1=[0x81组件+0x28]、key2=0x1307180(ref)、两级只读哈希 0x24105D0/0x23467B0、"
					  "进度@[元素+0x20]；★ **订正 R10：key 的类型 word 每次查询现读**"
					  "（它在 .data 未初始化段，启动时必为 0 —— R9 因此把判据整个禁用了）；"
					  "★★ **订正 R11：key1 改为「先组件、后引擎兜底 0x910690」**"
					  "（实测植物 ref 没有 0x81 组件 —— 引擎画绿走的也是这条兜底，R10 只复刻了"
					  "组件主路径 ⇒ 判据从未成功；probe 行看 `k1来源=`、统计行看 `兜底K1=`）；"
					  "★ **订正 R12（只改诊断）：R11 实测 `兜底K1=104 == 问(35)+失败(69)` ⇒ key1 获取"
					  "已 100% 成功、失败全在表查找层；stage=8 细分成 12/13/14/15"
					  "（14 = 空表 = 这个物种没扫过）；probe 失败行照实打印 key1/key2；"
					  "每 stage 首条 + 首条成功必打；判绿行独立额度；`冲突=` 计数已实现"
					  "（这两局 R11 的前 8 条 probe 全是 stage=8、判绿行一条都没留下 —— 本次修的就是可观测性）；"
					  "按 base 缓存（绿 = 10 分钟 / 未满 = 见 FloraUnscannedTtlMs）、"
					  "读档时清缓存；签名 / 形状校验不过会自动回退（日志搜 `flora progress:`）",
				g_cfg.floraEngineProgress);
			REX::INFO("config: floraUseMemory={} -> 自建记忆层（按引用记忆 / 按物种扩散 / 落盘播种 / 「沿用旧结论」）：{}",
				g_cfg.floraUseMemory,
				g_cfg.floraUseMemory
					? "保留 = v5.1.8 旧行为（只为对照 / 回退）"
					: "★ **关闭**（默认，v5.2）—— 「已扫描」只由引擎自己的数据回答："
					  "扫描进度直读 + 引擎状态表探针（4/5）+ GetOutlineState + 资源链（LVLI）");
			if (!g_cfg.floraUseMemory) {
				REX::INFO("flora learn: ★ v5.2 记忆层关闭（FloraUseMemory=0）—— 不读 / 不写落盘学习表、"
						  "不播种物种表、不查询引用记忆；旧的 `SAS_AlwaysScan.flora-learn.txt` 原样保留"
						  "（想启用记忆时把 FloraUseMemory 设回 1 即可）");
			}
			LoadFloraLearnTable();

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
			// ★★★ 2026-09-27（订正 R3，性能）：环内 cell 分片遍历 —— 治用户报告的
			//   「帧数仍有下降」（当轮日志 scan avg=79ms：loop 55ms 几乎全是「每轮全量
			//   遍历环内 cell 的引用」，环总量可达 kRingRefsCap=60000）。
			{
				const std::uint32_t eff = std::max<std::uint32_t>(1, g_cfg.ringSliceRoundsEff);
				const std::uint64_t periodMs =
					static_cast<std::uint64_t>(eff) * static_cast<std::uint64_t>(std::max(1, g_cfg.scanIntervalMs));
				const bool sliceOn = eff > 1 && g_cfg.ringSliceMaxRounds > 1;
				REX::INFO("config: ringSliceMaxRounds={} -> 环内 cell 分片{}：有效片数={}（每轮只遍历每 cell 的 1/{} 片，"
						  "小 cell（≤{} 个引用）不分片；分片周期 ≈ {}ms 必须 < UnhighlightGraceMs={}ms，"
						  "否则目标会「摘掉再挂回」闪烁 —— 有效片数已按这两项钳死）；"
						  "想回退设 RingSliceMaxRounds=0（每轮全扫）",
					g_cfg.ringSliceMaxRounds,
					sliceOn ? "" : "（关：每轮全扫）",
					eff, eff, kRingSliceMinRefs, periodMs, g_cfg.unhighlightGraceMs);
			}
			// ★★★ 订正 R4（性能）：消灭热路径上的内核调用 —— 治用户报告的
			//   「静态场景帧数正常、动态场景（战斗 / 走动）卡顿」（见 Config 长注释）。
			{
				REX::INFO("config: fastReadMem={} -> 安全读{}；可读性校验{}"
						  "（★ 直读 = SEH 兜底 + memcpy，正常内存**零内核调用**；"
						  "旧行为 = ReadProcessMemory / VirtualQuery，游戏流式加载时"
						  "单次可慢 10~30 倍 —— 这是动态场景卡顿的主因候选）",
					g_cfg.fastReadMem,
					g_cfg.fastReadMem ? "= 直读（SEH 兜底）" : "= 内核读（旧行为，仅用于对照）",
					g_cfg.fastReadMem ? "= 直读探针（逐页 1 字节）" : "= VirtualQuery");
				REX::INFO("config: lootCacheTtlMs={} -> 容器 / 尸体判空结果按引用缓存 {}ms"
						  "（拿 / 放物品事件、容器界面开关会**立刻**作废缓存 ⇒ 判空延迟不受 TTL 影响；"
						  "0 = 关，退回每轮实时判空）",
					g_cfg.lootCacheTtlMs,
					g_cfg.lootCacheTtlMs > 0 ? std::to_string(g_cfg.lootCacheTtlMs) : std::string("（关）"));
			}
			// ★★★ v4.29：放下扫描仪后的「恢复提速」—— 治「植物 / 矿石比其它物品慢一拍」。
			REX::INFO("config: resyncBoost={}ms budget={} -> 放下扫描仪（引擎拆 Monocle HUD 清表）之后的头 {}ms 内，"
					  "把「全量重挂」的每轮预算从 {} 提到 {} 条，并让「青→绿」这类状态变化**优先**换色"
					  "（否则那一局 268 个目标要 5 轮 ≈ 1 秒才铺完）；想回退把两项任一设 0",
				g_cfg.resyncBoostMs, g_cfg.resyncBoostBudget, g_cfg.resyncBoostMs,
				g_cfg.maxOutlineOpsPerScan,
				g_cfg.resyncBoostBudget ? std::to_string(g_cfg.resyncBoostBudget) : std::string{ "off" });

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
		// ★★ v4.17 / ★★★ v4.23：「资源」判据（MISC + `ResourceType*` 关键词）
		// ====================================================================
		// 完整背景、28 个关键词的 FormID、形状校验链见常量区「v4.17 资源判据」。
		//   这个函数只做纯内存读 + 校验，**零引擎调用**；任何一步校验不过 ⇒ false
		//   （= 按「杂项」处理 —— 失败方向永远是安全的那一边）。
		//   读的是 `TESObjectMISC` 的 `BGSKeywordForm::keywords`（首选 base + 0x208）。
		//
		// ★★★ v4.23 **订正 BSTArray 布局**（这是「资源和杂物同色」的真根因）：
		//   v4.17~v4.22 一直按「`data@+0 / size@+8 / capacity@+0xC`」读，而 commonlibsf
		//   的 `BSTArray` 是：
		//     ```cpp
		//     class BSTArrayBase { std::uint32_t _size;      // +0x00
		//                          std::uint32_t _capacity;  // +0x04
		//                          static_assert(sizeof(BSTArrayBase) == 0x8); };
		//     template <class T, class Allocator = BSTArrayHeapAllocator>
		//     class BSTArray : public BSTArrayBase, public Allocator
		//     { … void* _data{ nullptr };  // +0x08  ⇒ sizeof == 0x10 };   // BSTArray.h:52/136/380
		//     ```
		//   ⇒ 正确布局 = **`size@+0x00 / capacity@+0x04 / data@+0x08`**。
		//   旧读法里 `data = *(+0x00)` 拿到的其实是 `size | capacity<<32`（一个几万的小
		//   整数）⇒ `IsPlausiblePointer` 必然否掉 ⇒ **任何 MISC 都判不出「资源」**，
		//   全程静默归「杂项」（用户实测 46 条统计行 `resource=` 恒为 0；那一局
		//   `resource keyword:` 连一条「通过」都没有 —— 7 个正样本里唯一在内存里的那个
		//   也不命中，正是因为读法本身错了，见 docs/22 §一）。
		//   ★ 同文件里 cell 引用数组用的 `kOffArraySize/Capacity/Data`（+0/+4/+8）**一直
		//     是对的**，这里的常量注释当年还特意写了「不套用那一套」——错的就是这一句。
		//
		// ★★ v4.23 自适应：`commonlibsf` 的成员偏移错过不止一次（`TESObjectCELL` 整体
		//   +8，见 docs/02），所以偏移不写死一个：按候选表逐个试，**形状校验全过**
		//   （头自洽 + data 可读 + 每个元素都是 KYWD 记录）才采纳，并缓存到会话结束。
		//   猜错的最坏结果仍然只是「不命中 ⇒ 资源归杂项」，而且会在日志里留下
		//   `misc kw probe:` 的原始读数（下一条见下）。
		//
		//   ★ 诊断（一眼定性，不用再猜）：
		//     `resource keyword: 关键词数组标定 = base+0x208（…）首样本 … kw=[…]` = 读法找到了；
		//     `misc kw probe: base=… 候选全不合格 …` = 5 个候选都不像关键词数组 ⇒
		//       偏移/布局又不对（把这行发出来即可）。
		bool ScanKeywordsAt(const std::uint8_t* a_raw, std::size_t a_off, bool* a_outShapeOk,
			std::uint32_t* a_outCount, std::uint32_t* a_outKw, std::uint32_t a_outKwMax)
		{
			*a_outShapeOk = false;
			*a_outCount   = 0;
			if (!IsReadable(a_raw + a_off, 16)) {
				return false;
			}
			// ★★ v4.23：布局 = size@+0 / capacity@+4 / data@+8（见上面的长注释）
			const std::uint32_t size = *reinterpret_cast<const std::uint32_t*>(a_raw + a_off + kOffArraySize);
			const std::uint32_t cap  = *reinterpret_cast<const std::uint32_t*>(a_raw + a_off + kOffArrayCapacity);
			const std::uint64_t data = *reinterpret_cast<const std::uint64_t*>(a_raw + a_off + kOffArrayData);
			if (size == 0 && cap == 0) {
				*a_outShapeOk = true;  // 合法空数组（MISC 可以一个关键词都没有，如 Credits）
				return false;
			}
			if (size < 1 || size > kMiscKeywordMax) {
				return false;
			}
			if (cap < size || cap > kMiscKeywordCapMax) {
				return false;
			}
			if (!IsPlausiblePointer(data) ||
				!IsReadable(reinterpret_cast<const void*>(data), static_cast<std::size_t>(size) * sizeof(void*))) {
				return false;
			}
			for (std::uint32_t i = 0; i < size; ++i) {
				const auto kw = *reinterpret_cast<const std::uint64_t*>(
					reinterpret_cast<const std::uint8_t*>(data) + i * sizeof(void*));
				if (!IsPlausiblePointer(kw) || !IsReadable(reinterpret_cast<const void*>(kw), kOffFormType + 1)) {
					return false;
				}
				const auto* kwRaw = reinterpret_cast<const std::uint8_t*>(kw);
				// 元素必须是**关键词记录**（KYWD）—— 这一条能挡住「偏移猜错时读到的垃圾指针」
				//   （例如 0x1F8 = `formFolderKeywordLists`，元素是 BGSFormFolderKeywordList*）
				if (kwRaw[kOffFormType] != static_cast<std::uint8_t>(RE::FormType::kKYWD)) {
					return false;
				}
				const auto id = *reinterpret_cast<const std::uint32_t*>(kwRaw + kOffFormID);
				if (i < a_outKwMax) {
					a_outKw[i] = id;
				}
				for (const auto r : kResourceKeywords) {
					if (r == id) {
						*a_outShapeOk = true;
						*a_outCount   = size;
						return true;
					}
				}
			}
			*a_outShapeOk = true;  // 全数组都验过 = 形状合格（只是没命中资源关键词）
			*a_outCount   = size;
			return false;
		}

		bool IsResourceBaseRaw(const RE::TESForm* a_base)
		{
			if (!a_base) {
				return false;
			}
			const auto* raw = reinterpret_cast<const std::uint8_t*>(a_base);

			// ① 已标定：只看那一个偏移（热路径 = 1 次头读 + ≤4 个元素校验，零额外成本）
			if (g_state.resKwOff != 0) {
				bool          shapeOk = false;
				std::uint32_t count   = 0;
				return ScanKeywordsAt(raw, g_state.resKwOff, &shapeOk, &count, nullptr, 0);
			}

			// ② 未标定：逐个候选试；**第一个给出「形状合格且非空」的偏移**被采纳（会话级）。
			//    （空的合法数组不能用来标定 —— 很多垃圾位置也能「碰巧」读成全 0。）
			for (const auto off : kMiscKwOffCandidates) {
				bool          shapeOk = false;
				std::uint32_t count   = 0;
				std::uint32_t kw[8]{};
				const bool hit = ScanKeywordsAt(raw, off, &shapeOk, &count, kw, 8);
				if (shapeOk && count > 0) {
					g_state.resKwOff = off;
					char kwList[160];
					std::size_t p = 0;
					const std::uint32_t shown = count < 8 ? count : 8;
					for (std::uint32_t i = 0; i < shown && p + 16 < sizeof(kwList); ++i) {
						p += static_cast<std::size_t>(std::snprintf(kwList + p, sizeof(kwList) - p,
							"%s0x%08X", i ? "," : "", kw[i]));
					}
					REX::INFO("resource keyword: 关键词数组标定 = base+0x{:X}（布局 size@+0 / capacity@+4 / "
							  "data@+8；首样本 base=0x{:08X} n={} kw=[{}]）-> {}",
						off, a_base->GetFormID(), count, kwList,
						hit ? "★ 命中 ResourceType* ⇒ 「资源」分类生效（state 3，颜色 #B36BFF）"
							: "该样本不是资源（判据仍需逐个命中 ResourceType*）");
					return hit;
				}
			}

			// ③ 5 个候选全不合格 ⇒ 限流打一条原始读数探针（这是「偏移/布局又不对」的唯一证据）
			if (g_state.miscKwProbes < kMiscKwProbeMax) {
				++g_state.miscKwProbes;
				char detail[320];
				std::size_t p = 0;
				for (const auto off : kMiscKwOffCandidates) {
					if (p + 40 >= sizeof(detail) || !IsReadable(raw + off, 16)) {
						continue;
					}
					p += static_cast<std::size_t>(std::snprintf(detail + p, sizeof(detail) - p,
						"%s0x%X(s=%u,c=%u,d=0x%llX)", p ? " " : "", static_cast<unsigned>(off),
						*reinterpret_cast<const std::uint32_t*>(raw + off + kOffArraySize),
						*reinterpret_cast<const std::uint32_t*>(raw + off + kOffArrayCapacity),
						static_cast<unsigned long long>(
							*reinterpret_cast<const std::uint64_t*>(raw + off + kOffArrayData))));
				}
				REX::WARN("misc kw probe: base=0x{:08X} 候选偏移全不合格（都不像「元素是 KYWD 的 BSTArray」）"
						  " -> 该 MISC 按「杂项」处理；原始读数 {} —— 把这一行发出来即可定位偏移/布局",
					a_base->GetFormID(), detail);
			}
			return false;
		}

		// 带开关 + 自检门控的版本（ClassifyBase 用这个）。
		//   ★★★ v4.22：门控从「必须自检通过（== 1）」改成「**只要没被自检否决**（!= 2）」。
		//   理由（用户实测反馈 2：「资源和杂物也无法区分（都是原版扫描仪的蓝色）」）：
		//   0x5556E 在整局里 LookupByID 都是 null（自检永远等不到正样本），旧门控把
		//   「资源」**静默**冻在「杂项」上 ⇒ 两者同色。现在是「默认生效」：
		//   · 读法本身已有三层形状校验（BSTArray 头自洽 + 每个元素可读且 formType==KYWD
		//     + 命中 28 个 ResourceType* FormID 之一）⇒ 偏移猜错时最坏是**不命中**
		//     （资源归杂项），不会误判；
		//   · 自检只在拿到**明确反证**时才否决（负样本命中，或 ≥2 个正样本都不命中）。
		bool IsResourceBase(const RE::TESForm* a_base)
		{
			return g_cfg.resourceByKeyword && g_state.resKeywordTest != 2 && IsResourceBaseRaw(a_base);
		}

		// ★★ v4.17 起：启动自检 —— 用「已知答案」的原版记录验证上面那条读法。
		//   · 正样本池 `kResSelfTestSamples`（7 个资源 MISC）：**任一**「拿得到且命中」即通过；
		//   · `Digipick`(0x0000000A) / `Credits`(0x0000000F) 必须不命中（真杂物）。
		//   ★★ v4.22 重做（旧版见 docs/21 §二）：
		//     通过（有正样本命中） ⇒ resKeywordTest = 1（日志一条 INFO，带命中样本 FormID）；
		//     **否决**（负样本命中 ∨ 拿到 ≥2 个正样本但全不命中） ⇒ =2（资源归杂项 + WARN）；
		//     **样本不足**（都没进内存） ⇒ 保持 0（= 判据照常生效，只限流打一条说明）。
		//     这样「样本不在内存」再也不会让资源分类静默失效。
		void ResourceKeywordSelfTest()
		{
			if (g_state.resKeywordTest != 0) {
				return;
			}
			// ★ v4.22：节流 —— 这个自检是在「逐 cell」的循环里被调的（一圈最多 8 个 cell），
			//   样本没进内存时每轮要白跑 ~9 次 LookupByID ⇒ 限成 500ms 最多一次。
			const auto nowMs = NowMs();
			if (nowMs < g_state.resKeywordNextTryMs) {
				return;
			}
			g_state.resKeywordNextTryMs = nowMs + 500;
			++g_state.resKeywordTries;
			// ---- 正样本：能拿到几个拿几个，任一命中即算「读法正确」 ----
			std::uint32_t resolved = 0;
			std::uint32_t hits     = 0;
			std::uint32_t firstHit = 0;
			for (const auto id : kResSelfTestSamples) {
				auto* f = RE::TESForm::LookupByID(id);
				if (!f) {
					continue;  // 这个样本还没进内存（实测很常见，见常量区注释）
				}
				++resolved;
				if (IsResourceBaseRaw(f)) {
					++hits;
					if (firstHit == 0) {
						firstHit = id;
					}
				}
			}
			// ---- 负样本：真杂物命中 = 判据有误（会误报资源） ----
			auto* pick = RE::TESForm::LookupByID(kResKeywordDigipick);
			auto* cred = RE::TESForm::LookupByID(kResKeywordCredits);
			const bool pickHit = pick && IsResourceBaseRaw(pick);
			const bool credHit = cred && IsResourceBaseRaw(cred);
			if (pickHit || credHit) {
				g_state.resKeywordTest = 2;
				REX::WARN("resource keyword: 判据自检**否决**（真杂物被误判：digipick={} credits={}）"
						  "-> 资源暂归「杂项」（颜色不变）。读法/校验链见常量区「v4.17 资源判据」；"
						  "若要强制关掉这条判据，INI 里写 `ResourceByKeyword=0`",
					pickHit ? 1 : 0, credHit ? 1 : 0);
				return;
			}
			if (hits > 0) {
				g_state.resKeywordTest = 1;
				REX::INFO("resource keyword: 判据自检**通过**（正样本 {}/{} 个在内存、命中 {} 个，"
						  "如 0x{:X}；Digipick/Credits=杂项）-> 「资源」分类生效：state={}（颜色 #B36BFF）",
					resolved, static_cast<std::uint32_t>(std::size(kResSelfTestSamples)), hits, firstHit,
					g_cfg.stateByCategory[static_cast<std::size_t>(Category::kLootResource)]);
				return;
			}
			if (resolved >= 2) {
				g_state.resKeywordTest = 2;
				REX::WARN("resource keyword: 判据自检**否决**（拿到 {} 个正样本但一个都没命中，"
						  "离线核对 ResourceType* 都在）-> 资源暂归「杂项」（颜色不变）；"
						  "读法/校验链见常量区「v4.17 资源判据」",
					resolved);
				return;
			}
			// 样本不足：**不否决**（判据按形状校验兜底继续生效），限流打一条说明。
			const auto now = NowMs();
			if (g_state.resKeywordWarns < 6 && now >= g_state.resKeywordWarnAtMs) {
				++g_state.resKeywordWarns;
				g_state.resKeywordWarnAtMs = now + 5000;
				REX::WARN("resource keyword: 自检正样本还没进内存（{} 个样本只拿到 {} 个；第 {} 次尝试）"
						  "-> 「资源」判据**仍在生效**（形状校验兜底），首次命中会另行打一条 INFO",
					static_cast<std::uint32_t>(std::size(kResSelfTestSamples)), resolved, g_state.resKeywordTries);
			}
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
			// ================================================================
			// ★★ v4.17：可拾取 —— 按「物品栏分类」分色（用户需求 1.6）
			//   判据全部离线取证（Starfield.esm，见 docs/16）：
			//     武器 / 投掷物 = WEAP（`FragGrenade` 实测就是 WEAP）
			//     太空服 / 背包 / 头盔 / 服饰 = ARMO
			//     弹药 = AMMO，救援 = ALCH（含食品 / 饮料）
			//     笔记 = BOOK —— ★ 用户要求「书籍 / 技能杂志 / 数据板 / 音频日志**都要**」，
			//       离线全量核对（`out/esm_notes_probe.py`，Starfield.esm 382 万条记录）：
			//         · 数据板 EDID 含 `Slate` 的：**BOOK 282 条**（+4 条 MISC 是道具/占位）
			//         · 音频日志含 `AudioLog/Recording` 的：**BOOK 58 条**
			//         · 技能杂志含 `Skill_Magazine` 的：**BOOK 115 条**
			//       ⇒ 这四类在数据里**全是 BOOK**，`kBOOK → 笔记` 一条就全盖住
			//       （引擎里根本没有 NOTE 记录，下面那行只是留着兜底）。
			//     资源 = MISC + `ResourceType*` 关键词（见 IsResourceBase）
			//     其余（含 KEYM 钥匙） = 杂项 ⇒ **颜色与 1.5 完全一致（2 蓝）**
			// ================================================================
			case RE::FormType::kWEAP:  // 武器、投掷物
				return static_cast<int>(Category::kLootWeapon);
			case RE::FormType::kARMO:  // 太空服、背包、头盔、服饰
				return static_cast<int>(Category::kLootApparel);
			case RE::FormType::kAMMO:  // 弹药
			case RE::FormType::kALCH:  // 救援（含食品 / 饮料 / 飞船修理包）
				return static_cast<int>(Category::kLootAmmoAid);
			case RE::FormType::kBOOK:  // 笔记（书 / 杂志 / 数据板 / 便条）
			case RE::FormType::kNOTE:  // （Starfield.esm 里没有 NOTE 记录，留作兜底）
				return static_cast<int>(Category::kLootNote);
			case RE::FormType::kMISC:  // 资源（关键词命中）↔ 杂项
				if (IsResourceBase(a_base)) {
					// ★ v4.22：世界里**首个**命中 —— 「资源分类真的在干活」的最快证据
					//   （用户实测过「资源和杂物同色」，复盘时就 grep 这一行）。
					if (g_state.resKeywordFirstHit == 0) {
						g_state.resKeywordFirstHit = a_base->GetFormID();
						REX::INFO("resource keyword: 首个资源命中 base=0x{:08X} -> 资源分类生效"
								  "（state={}，颜色 #B36BFF）",
							g_state.resKeywordFirstHit,
							g_cfg.stateByCategory[static_cast<std::size_t>(Category::kLootResource)]);
					}
					return static_cast<int>(Category::kLootResource);
				}
				return static_cast<int>(Category::kLoot);
			// ---- 杂项（颜色不变）----
			case RE::FormType::kINGR:
			case RE::FormType::kKEYM:
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
		//   ★ v4.15：改走 SafeReadMem —— 这个指针来自库存条目，**清单本身可能是
		//     垃圾**（引用被释放后内存复用，见 docs/15 §4）⇒ 裸读有 AV 风险。
		//     读不出来时返回 false（= 当作「可拿的物品」），由调用方后面的形状校验兜住。
		bool IsNonPlayableForm(std::uint64_t a_obj)
		{
			std::uint32_t flags = 0;
			if (!SafeReadMem(reinterpret_cast<const void*>(a_obj + kOffFormFlags), &flags, sizeof(flags))) {
				return false;
			}
			return (flags & kFormFlagNonPlayable) != 0;
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
		// ★ v4.13：「内容快照」的可选输出（**只在展示柜那条路上要**）——
		//   记账判据的基准：`base -> count`（见常量区「v4.13」）。
		//   传了它 ⇒ 不早退，把整份库存走完（上限 kDcSnapMax 条 / kDcSnapStackBudget
		//   个 stack）；超出上限 / 有条目被跳过（np、eq）⇒ `overflow`/`uncertain` = true
		//   ⇒ 记账**不许下「拿空」结论**（宁可少灭一次，也绝不误灭）。
		struct DcSnapOut
		{
			std::uint32_t base[kDcSnapMax]{};
			std::uint32_t cnt[kDcSnapMax]{};
			std::uint8_t  count     = 0;
			bool          overflow  = false;  // 条目/stack 超出枚举上限
			bool          uncertain = false;  // 有条目被跳过（拿不走的那两类）⇒ 快照不完整
			bool          sawTp     = false;  // 这一份里有没有 kTemporary 投影条目
		};
		//   ★ v4.10：可选输出 `a_outSawTemporary`（本轮有没有见到 kTemporary
		//     投影条目）—— 展示柜「拿空即灭」判据要用（见常量区「v4.10」）。
		//   ★ v4.13：可选输出 `a_outSnap`（内容快照，见上）。
		int                RefLootState(const RE::TESObjectREFR* a_ref, bool* a_outSawTemporary = nullptr,
						   DcSnapOut* a_outSnap = nullptr);

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
				// ★ v4.15：探针里的裸读一并改走 SafeReadMem（docs/15 §4：同一类风险）
				SafeReadMem(raw + g_invOff, &invPtr, sizeof(invPtr));
				loot = RefLootState(a_ref);
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

			// ★ v4.15：探针的每一次「按指针取字段」都改走 SafeReadMem（docs/15 §4）
			std::uint64_t ptrRaw = 0;
			if (!SafeReadMem(raw + g_invOff, &ptrRaw, sizeof(ptrRaw))) {
				REX::INFO("loot probe: ref={:08X} d={:.1f}m inv 读不出来（引用已失效？）", fid, dM);
				return;
			}
			if (ptrRaw == 0) {
				REX::INFO("loot probe: ref={:08X} d={:.1f}m inv=null（引擎没给这个引用建过库存）", fid, dM);
				return;
			}
			if (!IsPlausiblePointer(ptrRaw)) {
				REX::INFO("loot probe: ref={:08X} d={:.1f}m inv=垃圾指针 0x{:X}", fid, dM, ptrRaw);
				return;
			}
			const auto* inv = reinterpret_cast<const std::uint8_t*>(ptrRaw);
			std::uint32_t size = 0;
			std::uint32_t cap  = 0;
			std::uint64_t data = 0;
			if (!SafeReadMem(inv + kOffInvData, &size, sizeof(size)) ||
				!SafeReadMem(inv + kOffInvData + 4, &cap, sizeof(cap)) ||
				!SafeReadMem(inv + kOffInvData + 8, &data, sizeof(data))) {
				REX::INFO("loot probe: ref={:08X} d={:.1f}m 库存头读不出来（指针 0x{:X} 已失效？）", fid, dM, ptrRaw);
				return;
			}
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
				// ★ v4.15：条目头整块安全读（一次 SafeReadMem 覆盖下面用到的所有字段）
				std::uint8_t itemBuf[kOffInvItemSize]{};
				if (!SafeReadMem(reinterpret_cast<const std::uint8_t*>(data) + i * kOffInvItemSize,
						itemBuf, sizeof(itemBuf))) {
					break;  // 条目数组已经读不动 ⇒ 后面的都不用看了
				}
				const auto* item = itemBuf;
				const auto  obj  = *reinterpret_cast<const std::uint64_t*>(item + kOffInvItemObject);
				if (!IsPlausibleFormPtr(obj)) {
					continue;
				}
				const auto sn = *reinterpret_cast<const std::uint32_t*>(item + kOffInvItemStacks);
				if (sn == 0 || sn > kInvMaxStacks) {
					continue;
				}
				const auto sd = *reinterpret_cast<const std::uint64_t*>(item + kOffInvItemStacks + 8);
				if (!IsPlausiblePointer(sd)) {
					continue;
				}
				std::uint64_t sum = 0;
				for (std::uint32_t j = 0; j < sn; ++j) {
					std::uint32_t cnt = 0;
					if (!SafeReadMem(reinterpret_cast<const std::uint8_t*>(sd) + j * kOffStackSize + kOffStackCount,
							&cnt, sizeof(cnt))) {
						sum = 0;  // 读不动 ⇒ 这一条按「读不出来」处理（不计入任何桶）
						break;
					}
					sum += cnt;
				}
				if (sum == 0) {
					continue;
				}
				// ★ v4.15：物品记录标志也走 SafeReadMem（读不出 = 0，桶归属只看 np/eq）
				std::uint32_t objFlags = 0;
				SafeReadMem(reinterpret_cast<const std::uint8_t*>(obj) + kOffFormFlags,
					&objFlags, sizeof(objFlags));
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
			// ★ v4.10：展示柜的判决状态（`ui=` 容器界面是否开着；`st=` 0=未知 1=有东西
			//   2=已确认拿空；`tpS=` 这一段打开期里见过投影条目）—— 「拿空即灭」
			//   有没有生效、卡在哪一步，看这三个字段（见常量区「v4.10」）。
			//   ★ v4.11：再加 `ev=`（**事件侧**判定「容器界面开着」）与 `mEv=`（收到的
			//     菜单事件总数）—— v4.10 实测 `ui=` 全程 0，这两个字段一眼区分
			//     「事件通道没生效」还是「菜单名不对」（见常量区「v4.11」）。
			char dcBuf[96]{};
			if (a_displayCase) {
				const auto dsIt = g_state.displayCaseVerdict.find(a_ref);
				const int  st   = (dsIt != g_state.displayCaseVerdict.end()) ? dsIt->second.lastKnown : 0;
				const bool tpS  = (dsIt != g_state.displayCaseVerdict.end()) &&
								  dsIt->second.lastTpSerial != 0 &&
								  dsIt->second.lastTpSerial == g_state.containerUiSerial;
				std::snprintf(dcBuf, sizeof(dcBuf), " ui=%d st=%d tpS=%d ev=%d mEv=%llu",
					g_state.containerUiOpen ? 1 : 0, st, tpS ? 1 : 0,
					g_state.menuEvtOpen.load() ? 1 : 0,
					static_cast<unsigned long long>(g_state.menuEvtTotal.load()));
			}
			const auto* raw     = reinterpret_cast<const std::uint8_t*>(a_ref);

			if (g_invOff == 0) {
				REX::INFO("cont probe{}: ref={:08X} base={:08X} d={:.1f}m loot={} -> 库存偏移还没标定（不判空，容器照常亮）",
					tag, fid, bid, dM, a_loot);
				return;
			}

			// ★ v4.15：探针的每一次「按指针取字段」都改走 SafeReadMem（docs/15 §4）
			std::uint64_t ptrRaw = 0;
			if (!SafeReadMem(raw + g_invOff, &ptrRaw, sizeof(ptrRaw))) {
				REX::INFO("cont probe{}: ref={:08X} base={:08X} d={:.1f}m loot={} inv 读不出来（引用已失效？）",
					tag, fid, bid, dM, a_loot);
				return;
			}
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
			const auto* inv = reinterpret_cast<const std::uint8_t*>(ptrRaw);
			std::uint32_t size = 0;
			std::uint32_t cap  = 0;
			std::uint64_t data = 0;
			if (!SafeReadMem(inv + kOffInvData, &size, sizeof(size)) ||
				!SafeReadMem(inv + kOffInvData + 4, &cap, sizeof(cap)) ||
				!SafeReadMem(inv + kOffInvData + 8, &data, sizeof(data))) {
				REX::INFO("cont probe{}: ref={:08X} base={:08X} d={:.1f}m loot={} 库存头读不出来（指针 0x{:X} 已失效？）",
					tag, fid, bid, dM, a_loot, ptrRaw);
				return;
			}
			if (size > kInvMaxItems || cap < size || cap > (1u << 20) ||
				(size != 0 && !IsPlausiblePointer(data))) {
				REX::INFO("cont probe{}: ref={:08X} base={:08X} d={:.1f}m loot={} 形状不对（size={} cap={} data=0x{:X}）",
					tag, fid, bid, dM, a_loot, size, cap, data);
				return;
			}
			if (size == 0) {
				REX::INFO("cont probe{}: ref={:08X} base={:08X} d={:.1f}m loot={} inv=ok size=0{} -> 判空原因=库存数组为空（一条都没有）",
					tag, fid, bid, dM, a_loot, dcBuf);
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
				// ★ v4.15：条目头整块安全读（一次 SafeReadMem 覆盖下面用到的所有字段）
				std::uint8_t itemBuf[kOffInvItemSize]{};
				if (!SafeReadMem(reinterpret_cast<const std::uint8_t*>(data) + i * kOffInvItemSize,
						itemBuf, sizeof(itemBuf))) {
					break;  // 条目数组已经读不动 ⇒ 后面的都不用看了
				}
				const auto* item = itemBuf;
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
							std::uint32_t cnt = 0;
							if (!SafeReadMem(
									reinterpret_cast<const std::uint8_t*>(sd) + j * kOffStackSize + kOffStackCount,
									&cnt, sizeof(cnt))) {
								sum = 0;
								break;
							}
							sum += cnt;
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
					// ★ v4.15：明细里的「物品记录」三个字段同样走 SafeReadMem（纯诊断，读到 0 也无妨）
					const auto* objRaw  = reinterpret_cast<const std::uint8_t*>(obj);
					std::uint8_t objFt = 0;
					std::uint32_t objFid = 0;
					std::uint32_t objFlg = 0;
					SafeReadMem(objRaw + kOffFormType, &objFt, sizeof(objFt));
					SafeReadMem(objRaw + kOffFormID, &objFid, sizeof(objFid));
					SafeReadMem(objRaw + kOffFormFlags, &objFlg, sizeof(objFlg));
					const auto objType = objFt;
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
				// ★ v4.10：展示柜的「空」分两种 —— 按当前状态预判（判决随后就会
				//   落到同一套判据上，这里的文案要和它一致，见常量区「v4.10」）。
				const auto dsIt       = g_state.displayCaseVerdict.find(a_ref);
				const bool stickEmpty = (dsIt != g_state.displayCaseVerdict.end()) && dsIt->second.lastKnown == 2;
				const bool sameOpen   = (dsIt != g_state.displayCaseVerdict.end()) &&
										dsIt->second.lastTpSerial != 0 &&
										dsIt->second.lastTpSerial == g_state.containerUiSerial &&
										g_state.containerUiOpen;
				if (stickEmpty || sameOpen) {
					std::snprintf(whyBuf, sizeof(whyBuf),
						"展示柜：%s ⇒ 判「拿空」、会熄灭（关掉后靠粘性保持熄灭）",
						stickEmpty ? "此前已确认拿空（粘性）" : "同一段打开期里读到空（内容被拿光）");
				} else {
					std::snprintf(whyBuf, sizeof(whyBuf),
						"展示柜：关闭状态 / 从没打开过读到「空」= 内容未知 ⇒ 照常亮（v4.9 行为）");
				}
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
			REX::INFO("cont probe{}: ref={:08X} base={:08X} d={:.1f}m loot={} size={} keep={}(sum={}) withCount={} sumRaw={} skipNp={} skipEq={} why={}{}{}",
				tag, fid, bid, dM, a_loot, size, keepCnt,
				static_cast<unsigned long long>(sumKeep), withCount,
				static_cast<unsigned long long>(sumRaw), skipNp, skipEq, whyBuf, detail, dcBuf);
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
					// ★ v4.15：候选偏移上的指针也用 SafeReadMem 取（换场景期引用随时可能失效）
					std::uint64_t p = 0;
					if (!SafeReadMem(refRaw + kOffInvCand[c], &p, sizeof(p)) ||
						!IsPlausiblePointer(p) || !IsReadable(reinterpret_cast<const void*>(p), 0x38)) {
						++s.invBad[c];
						continue;
					}
					const auto* inv = reinterpret_cast<const std::uint8_t*>(p);
					std::uint32_t size = 0;
					std::uint32_t cap  = 0;
					std::uint64_t data = 0;
					if (!SafeReadMem(inv + kOffInvData, &size, sizeof(size)) ||
						!SafeReadMem(inv + kOffInvData + 4, &cap, sizeof(cap)) ||
						!SafeReadMem(inv + kOffInvData + 8, &data, sizeof(data))) {
						++s.invBad[c];
						continue;
					}
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

		// ★ v4.10：展示柜判决缓存 —— 每个展示柜引用一份（见常量区「v4.10」）。
		//   上限防呆：真到上限就整体清空（最坏结果只是退回「关着也亮」的旧行为，
		//   绝不会崩 —— 换场景时也会清）。
		State::DisplayCaseVerdict& DisplayCaseVerdictFor(const RE::TESObjectREFR* a_ref)
		{
			auto it = g_state.displayCaseVerdict.find(a_ref);
			if (it != g_state.displayCaseVerdict.end()) {
				return it->second;
			}
			if (g_state.displayCaseVerdict.size() >= kDisplayCaseVerdictMax) {
				g_state.displayCaseVerdict.clear();
			}
			return g_state.displayCaseVerdict.emplace(a_ref, State::DisplayCaseVerdict{}).first->second;
		}

		// ★ v4.10：展示柜读到「有东西」时更新状态：记「有东西」+ 刷新「这一段打开期」。
		//   `a_sawTemporary`（本轮见到 kTemporary 投影条目）⇒ 把当前 UI 段号记下，
		//   后面「同一段打开期里读到空」就是「拿空」的判据。
		//   ★ v4.13：同时**刷新内容快照**（`a_snap`，记账判据的基准）+ 清掉
		//     「连续读到空」的计时（内容又出现了 ⇒ 兜底判据必须作废）。
		void NoteDisplayCaseOccupied(const RE::TESObjectREFR* a_ref, bool a_sawTemporary,
			const DcSnapOut* a_snap = nullptr)
		{
			auto& ds = DisplayCaseVerdictFor(a_ref);
			ds.lastKnown = 1;
			// ★ v4.14：这一段打开期里确实读到过内容（兜底判据的前置证据，见字段注释）
			ds.sawContentInSession = true;
			if (a_sawTemporary) {
				ds.lastTpSerial = g_state.containerUiSerial;
			}
			ds.emptySinceMs = 0;
			if (a_snap) {
				ds.snap.count     = a_snap->count;
				ds.snap.valid     = true;
				ds.snap.uncertain = a_snap->overflow || a_snap->uncertain;
				for (std::uint8_t i = 0; i < a_snap->count; ++i) {
					ds.snap.base[i] = a_snap->base[i];
					ds.snap.cnt[i]  = a_snap->cnt[i];
				}
			}
		}

		// ★ v4.10：展示柜读到「空」时的判决（**只看状态，不改状态**）——
		//   返回 true = 这是「真拿空」（调用方会改成「不进候选 ⇒ 熄灭」）。
		//   判据（完整推导见常量区「v4.10」/「v4.13」）：
		//     ① 粘性：此前已确认拿空过（`lastKnown==2`）⇒ 关闭后也保持熄灭
		//        —— 这正是用户要的「拿空即灭」；
		//     ② v4.10 的「同一段打开期」（UI 是菜单时代的产物 —— 实测拿不到，
		//        保留只为兼容「万一某个菜单真的开了」的情形）；
		//     ③ ★ v4.13 兜底：**快速搜刮面板确认为这个 ref 开着**、这一段打开期里
		//        **一次拿走事件都没收到**、且连续读到空 ≥ kDcEmptyConfirmMs。
		//        （收到过拿走事件的情形由记账判据直接落粘性，不走这里。）
		//   其余（关着 / 从没见过投影 / 从没打开过）⇒ 内容未知 ⇒ 照常亮（v4.9 目标）。
		bool DisplayCaseReadsEmpty(const RE::TESObjectREFR* a_ref, std::uint64_t a_nowMs = 0)
		{
			const auto it = g_state.displayCaseVerdict.find(a_ref);
			if (it == g_state.displayCaseVerdict.end()) {
				return false;  // 从没见过它的投影 ⇒ 未知 ⇒ 照常亮
			}
			const auto& ds = it->second;
			if (ds.lastKnown == 2) {
				return true;  // ① 粘性：已确认拿空（关掉后仍保持熄灭）
			}
			if (g_state.containerUiOpen && ds.lastTpSerial != 0 &&
				ds.lastTpSerial == g_state.containerUiSerial) {
				return true;  // ② 同一段打开期（v4.10 判据）
			}
			// ③ ★ v4.13 兜底（★ v4.14 默认关 + 加严，完整依据见常量区「v4.13」尾部的 v4.14 段）
			//   加严：`sawContentInSession` —— 这一段打开期里必须**真的读到过内容**。
			//   没有它时会误判：面板刚打开、投影还没建立的那一段（关闭状态库存本来就空）
			//   会被当成「连续读到空 ≥ 400ms」⇒ 一打开就判拿空（2026-09-19 的日志实证）。
			if (!g_cfg.displayCaseQuickOpenEmpty || ds.quickOpenMs == 0 ||
				ds.sawTakeEvt || ds.emptySinceMs == 0 || !ds.sawContentInSession) {
				return false;
			}
			const auto now = a_nowMs ? a_nowMs : NowMs();
			if (now - ds.quickOpenMs > kQuickOpenSessionMs) {
				return false;  // 这一段打开期早就过期了（防呆：别拿很久以前的事件当依据）
			}
			return (now - ds.emptySinceMs) >= kDcEmptyConfirmMs;
		}

		// ★ v4.10 / v4.13：把上面的判决**真正落到状态**（只有调用方决定「熄灭」时
		//   才调，所以粘性只会因为「确认拿空」而建立）。`a_why` 进日志（诊断哪条判据）。
		void MarkDisplayCaseEmptied(const RE::TESObjectREFR* a_ref, const char* a_why, bool a_fallback = false)
		{
			auto& ds = DisplayCaseVerdictFor(a_ref);
			if (ds.lastKnown == 2) {
				return;  // 已经粘住了（避免每帧重复计数 / 重复写日志）
			}
			ds.lastKnown = 2;
			++g_state.displayCaseEmptied;
			if (a_fallback) {
				++g_state.quickOpenFallbackDecided;
			}
			REX::INFO("display case emptied ({}): ref={:08X} -> 关掉后保持熄灭（粘性）",
				a_why, a_ref ? a_ref->GetFormID() : 0);
		}

		// ★ v4.13：`AddToSnap`（快照累加）定义在 `RefLootState` 前面（读库存那段），
		//   这里先用一下 ⇒ 前置声明。
		void AddToSnap(DcSnapOut& a_snap, std::uint32_t a_baseFid, std::uint32_t a_count);

		// ================================================================
		// ★★ v4.13：调试「事件 → 主线程」队列（每帧一次；判据见常量区「v4.13」）
		// ----------------------------------------------------------------
		// 记账（精确）：收到「从这个 ref 拿走 base×N」⇒ 从内容快照里减，
		//   减到 0（且快照完整）⇒ **判拿空**（粘性熄灭）。拿空后立刻关掉也不影响。
		//   「放进去」⇒ 加账 + 清粘性（东西又有了 ⇒ 重新亮）。
		// 快速面板打开 ⇒ 给该 ref 记 `quickOpenMs`，并把**别的** ref 的会话清掉
		//   （同一时刻只有一个搜刮面板）⇒ 兜底判据只在「就是它」时才可能生效。
		// ================================================================
		const RE::TESObjectREFR* FindWatchedRefByFid(std::uint32_t a_fid, std::size_t* a_outIdx = nullptr)
		{
			if (a_fid == 0) {
				return nullptr;
			}
			for (std::size_t i = 0; i < g_state.dcWatchCount; ++i) {
				const auto* ref = g_state.dcWatch[i].ref;
				if (ref && ref->GetFormID() == a_fid) {
					if (a_outIdx) {
						*a_outIdx = i;
					}
					return ref;
				}
			}
			return nullptr;
		}

		// 事件日志限流（上限 cfg.lootEventLogMax；返回 true = 可以打这一条）。
		bool LootEvtMayLog()
		{
			if (g_state.lootEvtLogs >= static_cast<std::uint32_t>(std::max(0, g_cfg.lootEventLogMax))) {
				return false;
			}
			++g_state.lootEvtLogs;
			return true;
		}

		void ProcessLootEvents(std::uint64_t a_nowMs)
		{
			std::array<LootEvtRec, kLootEvtQueueMax> local{};
			std::size_t                              n = 0;
			{
				std::scoped_lock lock{ g_lootEvtLock };
				n = g_lootEvtQueueCount;
				if (n > kLootEvtQueueMax) {
					n = kLootEvtQueueMax;
				}
				for (std::size_t i = 0; i < n; ++i) {
					local[i] = g_lootEvtQueue[i];
				}
				g_lootEvtQueueCount = 0;
			}
			// ★★★ 订正 R4：**任何物品进出事件都作废判空缓存** —— 拿 / 放都会改变
			//   某个容器 / 尸体的内容，而「内容变化」正是判空结果唯一的权威信号。
			//   代价：下一次扫描（≤ ScanIntervalMs）把附近容器重读一遍；
			//   换来：「拿空即灭」的延迟**不受缓存 TTL 影响**（与旧行为逐帧一致）。
			if (n > 0) {
				++g_state.lootMemoSerial;
			}
			if (!g_cfg.containerLootEvents) {
				return;  // 功能关着：**照旧把队列排空**（否则它会一直涨），只是不判决
			}
			for (std::size_t i = 0; i < n; ++i) {
				const auto& rec = local[i];
				if (rec.kind == static_cast<std::uint8_t>(LootEvtKind::kQuickOpen)) {
					// ---- 快速搜刮面板打开：per-ref 的「面板开着」会话 ----
					const auto* ref = reinterpret_cast<const RE::TESObjectREFR*>(rec.refPtr);
					g_state.quickOpenLastMs  = a_nowMs;
					std::uint32_t fid        = 0;
					bool          matched    = false;
					for (std::size_t k = 0; k < g_state.dcWatchCount; ++k) {
						if (g_state.dcWatch[k].ref == ref) {
							fid     = ref->GetFormID();
							matched = true;
							break;
						}
					}
					if (matched) {
						// 同一时刻只有一个面板 ⇒ 先清掉别的 ref 的会话
						for (auto& [r, ds] : g_state.displayCaseVerdict) {
							if (r != ref && ds.quickOpenMs != 0) {
								ds.quickOpenMs         = 0;
								ds.sawTakeEvt          = false;
								ds.emptySinceMs        = 0;
								ds.sawContentInSession = false;  // ★ v4.14
							}
						}
						auto& ds        = DisplayCaseVerdictFor(ref);
						ds.quickOpenMs  = a_nowMs;
						ds.sawTakeEvt   = false;
						ds.emptySinceMs = 0;
						// ★ v4.14：新会话 = 还没见过内容（打开瞬间投影可能尚未建立，
						//   关闭状态的库存本来就空 ⇒ 没有这条就会把「打开瞬间」误判成拿空）
						ds.sawContentInSession = false;
						++g_state.quickOpenMatched;
						g_state.quickOpenLastFid = fid;
						if (ds.evtLogs < kLootEvtPerRefLogMax && LootEvtMayLog()) {
							++ds.evtLogs;
							REX::INFO("loot event (quick open): ref={:08X} 的快速搜刮面板打开了"
									  "（在逐帧观察表里）-> 兜底判据就绪：这一段打开期里只要收不到"
									  "「拿走」事件、且连续读到空 ≥ {}ms 就判拿空",
								fid, kDcEmptyConfirmMs);
						}
					} else {
						++g_state.quickOpenUnmatched;
						if (LootEvtMayLog()) {
							REX::INFO("loot event (quick open): 面板为引用 0x{:X} 打开，但它不在逐帧观察表里"
									  "（不在附近 / 不是展示柜 / 表满）-> 这一条不参与判决",
								static_cast<unsigned long long>(rec.refPtr));
						}
					}
					continue;
				}

				// ---- 物品进出容器（take / put）----
				const bool isTake = (rec.kind == static_cast<std::uint8_t>(LootEvtKind::kTake));
				const auto* ref   = FindWatchedRefByFid(rec.refFid);
				if (!ref) {
					++g_state.lootEvtUnmatched;
					continue;  // 与我们无关的容器（世界里随时都有物品流动）
				}
				++g_state.lootEvtMatches;
				auto& ds = DisplayCaseVerdictFor(ref);
				if (isTake) {
					ds.sawTakeEvt = true;  // 有拿走事件 ⇒ 兜底判据让位（只信记账）
					if (!ds.snap.valid) {
						// 还没读到过内容快照 ⇒ 记不了账（最坏 = 这一轮不判拿空）
						if (ds.evtLogs < kLootEvtPerRefLogMax && LootEvtMayLog()) {
							++ds.evtLogs;
							REX::INFO("loot event (take): ref={:08X} base={:08X} x{} -> 还没读到内容快照，"
									  "这一次记不了账（下一帧读到投影时会补上基准）",
								rec.refFid, rec.baseFid, rec.count);
						}
						continue;
					}
					std::uint32_t left  = 0;
					bool          found = false;
					for (std::uint8_t k = 0; k < ds.snap.count; ++k) {
						if (ds.snap.base[k] == rec.baseFid) {
							found = true;
							const std::uint32_t have = ds.snap.cnt[k];
							if (have >= rec.count) {
								ds.snap.cnt[k] = have - rec.count;
							} else {
								ds.snap.cnt[k] = 0;
								// 拿走的比记的多 ⇒ 账不可信（快照是旧的 / 漏了条目）⇒ 不下结论
								ds.snap.uncertain = true;
							}
						}
						left += ds.snap.cnt[k];
					}
					if (!found) {
						ds.snap.uncertain = true;  // 拿走的 base 不在快照里 ⇒ 账不可信
					}
					++g_state.lootEvtTakeApplied;
					if (ds.evtLogs < kLootEvtPerRefLogMax && LootEvtMayLog()) {
						++ds.evtLogs;
						REX::INFO("loot event (take): ref={:08X} base={:08X} x{} -> 记账后还剩 {}"
								  "（快照 {} 条，{}）",
							rec.refFid, rec.baseFid, rec.count, left,
							ds.snap.count, ds.snap.uncertain ? "有不可信标记" : "可信");
					}
					if (left == 0 && !ds.snap.uncertain && ds.snap.count > 0) {
						++g_state.lootEvtEmptyDecided;
						MarkDisplayCaseEmptied(ref, "记账（事件：最后一件被拿走）");
					}
				} else {
					// 放进去了 ⇒ 加账 + 清粘性（东西又有了 ⇒ 重新亮）
					++g_state.lootEvtPutApplied;
					if (ds.snap.valid && !ds.snap.uncertain) {
						bool found = false;
						for (std::uint8_t k = 0; k < ds.snap.count; ++k) {
							if (ds.snap.base[k] == rec.baseFid) {
								ds.snap.cnt[k] += rec.count;
								found = true;
								break;
							}
						}
						if (!found) {
							DcSnapOut tmp{};
							tmp.count = ds.snap.count;
							for (std::uint8_t k = 0; k < ds.snap.count; ++k) {
								tmp.base[k] = ds.snap.base[k];
								tmp.cnt[k]  = ds.snap.cnt[k];
							}
							AddToSnap(tmp, rec.baseFid, rec.count);
							ds.snap.count     = tmp.count;
							ds.snap.uncertain = tmp.overflow;
							for (std::uint8_t k = 0; k < tmp.count; ++k) {
								ds.snap.base[k] = tmp.base[k];
								ds.snap.cnt[k]  = tmp.cnt[k];
							}
						}
					}
					if (ds.lastKnown == 2) {
						ds.lastKnown = 1;  // 又有东西了 ⇒ 撤销「拿空」粘性（重新亮）
						REX::INFO("display case re-lit (事件：放进了东西): ref={:08X} base={:08X} x{}",
							rec.refFid, rec.baseFid, rec.count);
					}
					if (ds.evtLogs < kLootEvtPerRefLogMax && LootEvtMayLog()) {
						++ds.evtLogs;
						REX::INFO("loot event (put): ref={:08X} base={:08X} x{} -> 已加账（快照 {} 条）",
							rec.refFid, rec.baseFid, rec.count, ds.snap.count);
					}
				}
			}
		}

		// ================================================================
		// ★★ v4.11：展示柜「拿空即灭」的三件套（完整推导见常量区「v4.11」）
		//   ① DcWatchAdd：把近距离展示柜放进「逐帧观察表」——200ms 的扫描节拍会
		//      漏掉「拿空 ⇒ 立刻关掉」（同一段打开期里读到的空只存在很短一瞬）；
		//   ② DcTickWatch：每帧读一次它们的库存 ⇒ 观测窗口从 200ms 缩到 1 帧；
		//   ③ DcTrace / DcUiProbe：投影出现 / 消失 + 界面状态的取证日志，
		//      以及「界面标志为 false 却看到投影」时的自愈学习 + 菜单快照。
		// ================================================================
		void DcWatchAdd(const RE::TESObjectREFR* a_ref, float a_distSq)
		{
			if (!g_cfg.displayCaseFrameWatch || !a_ref) {
				return;
			}
			// ★ 注意：`a_distSq` 是**游戏单位²**（与 ContProbe / LootProbe 同一口径），
			//   换算成米要乘 `unitsPerMeter`。
			const float maxD = kDcWatchRadiusMeters * g_cfg.unitsPerMeter;
			if (a_distSq > maxD * maxD) {
				return;
			}
			for (std::size_t i = 0; i < g_state.dcWatchCount; ++i) {
				if (g_state.dcWatch[i].ref == a_ref) {
					g_state.dcWatch[i].miss = 0;
					return;
				}
			}
			if (g_state.dcWatchCount >= kDcWatchMax) {
				return;  // 表满就不再收（12 个近距离展示柜够用；满了说明站在展示柜堆里）
			}
			auto& w = g_state.dcWatch[g_state.dcWatchCount++];
			w.ref      = a_ref;
			w.miss     = 0;
			w.lastLoot = -2;
			w.lastTp   = false;
		}

		// 投影出现 / 消失的追踪日志（诊断主力，条数受 `DisplayCaseTraceMax` 限制）。
		//   ★ 只有在「状态真的变了」时才写 ⇒ 不会刷屏。
		//   ★ v4.13：行尾补 `qo=`（快速面板会话开着吗）/ `take=`（这一段打开期里
		//     收到过拿走事件吗）/ `snap=`（内容快照条数）/ `emptyMs=`（连续空多久）
		//     —— 判据生效与否，看这一行就够了。
		void DcTrace(const RE::TESObjectREFR* a_ref, int a_loot, bool a_sawTp)
		{
			if (g_state.dcTraceCount >= static_cast<std::uint32_t>(g_cfg.displayCaseTraceMax)) {
				return;
			}
			++g_state.dcTraceCount;
			std::uint32_t qo = 0, take = 0, snapN = 0;
			std::uint64_t emptyMs = 0;
			if (const auto it = g_state.displayCaseVerdict.find(a_ref); it != g_state.displayCaseVerdict.end()) {
				qo      = it->second.quickOpenMs ? 1 : 0;
				take    = it->second.sawTakeEvt ? 1 : 0;
				snapN   = it->second.snap.count;
				emptyMs = it->second.emptySinceMs ? (NowMs() - it->second.emptySinceMs) : 0;
			}
			REX::INFO("display case trace: ref={:08X} loot={} tp={} ui={}(ev={} serial={}) "
					  "qo={} take={} snap={} emptyMs={} -> {}",
				a_ref ? a_ref->GetFormID() : 0, a_loot, a_sawTp ? 1 : 0,
				g_state.containerUiOpen ? 1 : 0, g_state.menuEvtOpen.load() ? 1 : 0,
				g_state.containerUiSerial, qo, take, snapN, emptyMs,
				a_sawTp ? "投影出现（搜刮面板正开着）"
						: (a_loot == 0 ? "投影消失（判「拿空」看 qo/take/emptyMs；都不是 ⇒ 内容未知）" : "内容有东西"));
		}

		// 「看到投影，可我们的界面标志还是 false」⇒ 取证 +（可选）自愈学习。
		//   纯查询（`IsMenuOpen` 候选名 + 事件侧状态），零内存猜测。
		void DcUiProbe(const RE::TESObjectREFR* a_ref)
		{
			// 限流 1 秒：逐帧 / 每轮扫描都可能调到这里，而菜单快照要跑 ~60 次 IsMenuOpen。
			const auto nowMs = NowMs();
			if (g_state.dcUiProbeLastMs && nowMs - g_state.dcUiProbeLastMs < 1000) {
				return;
			}
			g_state.dcUiProbeLastMs = nowMs;

			std::string openList;
			const auto  n = SnapshotOpenMenus(openList);
			// 事件侧「此刻开着」的菜单名（锁内拷贝一份出来）
			std::string evtList;
			std::string recentList;  // ★ v4.12：最近几条事件（名字 + 开/关 + 多久以前）
			char        lastOpen[kMenuEvtNameMax]{};
			std::uint64_t lastOpenMs = 0;
			{
				std::scoped_lock lock{ g_state.menuEvtLock };
				for (int i = 0; i < g_state.menuEvtOpenCount; ++i) {
					if (!evtList.empty()) {
						evtList += ", ";
					}
					evtList += g_state.menuEvtOpenNames[i];
				}
				for (std::size_t i = 0; i < g_state.menuEvtRecentCount; ++i) {
					if (!recentList.empty()) {
						recentList += ", ";
					}
					recentList += g_state.menuEvtRecent[i].name;
					recentList += g_state.menuEvtRecent[i].opening ? "开" : "关";
					char tail[32]{};
					std::snprintf(tail, sizeof(tail), "(%llums 前)",
						static_cast<unsigned long long>(nowMs > g_state.menuEvtRecent[i].ms
															? nowMs - g_state.menuEvtRecent[i].ms
															: 0));
					recentList += tail;
				}
				if (recentList.empty()) {
					recentList = "（没有收到任何菜单事件）";
				}
				std::snprintf(lastOpen, sizeof(lastOpen), "%s", g_state.menuEvtLastOpen);
				lastOpenMs = g_state.menuEvtLastOpenMs;
			}
			if (g_state.menuDumps < static_cast<std::uint32_t>(g_cfg.menuDumpMax)) {
				++g_state.menuDumps;
				REX::INFO("menu dump (看到展示柜投影但 ui=0，取证): ref={:08X} | IsMenuOpen 快照: [{}]（{} 个）"
						  "| 事件侧开着: [{}] | 最近一次打开: \"{}\"（{}ms 前）| 事件总数={} 容器事件={} "
						  "| 最近事件: {}",
					a_ref ? a_ref->GetFormID() : 0, openList, n, evtList, lastOpen,
					lastOpenMs ? (NowMs() - lastOpenMs) : 0,
					g_state.menuEvtTotal.load(), g_state.menuEvtContainer.load(), recentList);
			}
			// ★ 自愈：事件侧「开着的菜单」里，若**恰好只有一个**不在忽略名单里 ⇒ 就是它
			//   （容器界面开着的时候，别的菜单基本都关着；HUD / 光标这类永远在的会被排除）。
			if (!g_cfg.containerMenuLearn || g_state.containerMenuLearnedCount >= kMenuLearnMax) {
				return;
			}
			char candidate[kMenuEvtNameMax]{};
			int  candidateCount = 0;
			{
				std::scoped_lock lock{ g_state.menuEvtLock };
				for (int i = 0; i < g_state.menuEvtOpenCount; ++i) {
					const char* name = g_state.menuEvtOpenNames[i];
					if (ContainerMenuNameMatchesLocked(name) || IsIgnoredMenuName(name)) {
						continue;
					}
					std::snprintf(candidate, sizeof(candidate), "%s", name);
					++candidateCount;
				}
			}
			if (candidateCount != 1) {
				if (candidateCount > 1) {
					REX::WARN("menu learn: 事件侧开着 {} 个候选菜单，无法唯一确定容器界面（看上面的 menu dump 取证）",
						candidateCount);
				}
				return;
			}
			g_state.containerMenuLearned[g_state.containerMenuLearnedCount++] = candidate;
			++g_state.containerMenuLearnedEvents;
			REX::INFO("menu learn: 把 \"{}\" 学成容器菜单名（本会话第 {} 个）-> "
					  "之后它一开就算「容器界面开着」（原版 / 第三方改过菜单名时靠这一步自愈）",
				candidate, g_state.containerMenuLearnedCount);
		}

		// ★ v4.11：逐帧观察（Tick 每帧调；表是空的时几乎零成本）。
		void DcTickWatch()
		{
			if (!g_cfg.displayCaseFrameWatch || g_state.dcWatchCount == 0 || g_invOff == 0) {
				return;
			}
			// ★ v4.15：载入 / 换场景期间*不碰旧世界的引用*（docs/15 §4 —— 崩溃 B 就是
			//   在这里读到「已经被释放、内存被别的数据复用」的引用）。
			//   这里用的是上一帧的值（`loadingNow` 在 Tick 里稍后才刷新）：只当优化，
			//   真正的安全由下面的 LooksLikeLiveRef 兜底。
			if (g_state.loadingNow) {
				return;
			}
			++g_state.dcWatchFrames;
			for (std::size_t i = 0; i < g_state.dcWatchCount;) {
				auto& w = g_state.dcWatch[i];
				// 形状校验（★ v4.15 加强）：不再只看「能不能读」，而是「**还像不像一个
				// 活着的引用对象**」—— 可读 + vtable 在游戏映像内 + formType ∈ {REFR,ACHR}。
				//   崩溃 B 的现场里那块内存**是可读的**（堆内存还 mapped），旧判据一律放行，
				//   于是 `*(ref+0xA0)` 读出一串垃圾（0x1A486）当库存指针用 ⇒ AV。
				const bool ok = w.ref && LooksLikeLiveRef(reinterpret_cast<std::uint64_t>(w.ref));
				if (!ok) {
					if (++w.miss >= kDcWatchMissMax) {
						g_state.dcWatch[i] = g_state.dcWatch[--g_state.dcWatchCount];
						continue;  // 踢掉，重新看同一个下标
					}
					++i;
					continue;
				}
				w.miss = 0;
				bool      sawTp = false;
				DcSnapOut snap{};
				// ★ v4.13：逐帧读的时候顺便刷**内容快照**（记账判据的基准）。
				const int loot = RefLootState(w.ref, &sawTp, &snap);
				const int prev = w.lastLoot;
				const auto now = NowMs();
				auto&      ds  = DisplayCaseVerdictFor(w.ref);
				// ★ v4.13：「连续读到空」的计时（兜底判据要求 ≥ kDcEmptyConfirmMs）
				if (loot == 0) {
					if (ds.emptySinceMs == 0) {
						ds.emptySinceMs = now;
					}
				} else {
					ds.emptySinceMs = 0;
				}
				if (loot == 1) {
					// 「有东西」：投影刚出现（`tp=1`）或数量变化时才留痕
					if (prev != 1 || (sawTp && !w.lastTp)) {
						DcTrace(w.ref, loot, sawTp);
					}
					NoteDisplayCaseOccupied(w.ref, sawTp, &snap);
				} else {
					// 空（刚变空 / 一直空）：每帧都按判据问一次 —— v4.13 的兜底判据
					//   需要「连续空一段时间」才成立，只在跳变那一帧问会漏掉它。
					const bool byFallback =
						(ds.lastKnown != 2) && ds.quickOpenMs != 0 && !ds.sawTakeEvt;
					if (DisplayCaseReadsEmpty(w.ref, now)) {
						if (prev != 0) {
							DcTrace(w.ref, loot, false);
							++g_state.displayCaseEmptiedFrame;
						}
						MarkDisplayCaseEmptied(w.ref,
							byFallback ? "兜底（快速面板 + 连续读到空）" : "打开期里读到空", byFallback);
					} else if (prev == 1) {
						// 投影消失但判不出「拿空」⇒ 内容未知（关掉了 / 信号没接上）
						DcTrace(w.ref, loot, false);
						if (!g_state.containerUiOpen) {
							DcUiProbe(w.ref);  // 取证 +（可选）自愈学习
						}
					}
				}
				w.lastLoot = loot;
				w.lastTp   = sawTp;
				++i;
			}
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
		//   ★ v4.10：可选输出 `a_outSawTemporary` —— 本轮走到过 `kTemporary` 投影
		//     条目（展示柜打开期间才有）。展示柜「拿空即灭」判决靠它判断
		//     「这一段打开期里投影出现过」（见常量区「v4.10 展示柜拿空即灭」）。
		//
		//   ★ 为什么还要逐条把 stack 里的 count 加起来，而不是只看 `data.size()`：
		//     物品被拿光之后，引擎**可能留下 count = 0 的空 stack / 空条目**
		//     （Starfield 里「0 件物品仍占一格」的现象），只看条目数会把已经搜空的箱子
		//     永远当成非空 ⇒ 高亮永不熄灭（这正是用户报的第 3 个问题）。
		//   ★ 全程纯内存读（**零引擎调用、零 VirtualQuery**）：先探指针，再校验
		//     BSTArray 头（size/cap 自洽），任何一步形状不对就返回「未知」。
		//     ⇒ 最坏结果是「和以前一样照常亮」，绝不会因为读错内存而崩。
		// ★ v4.13：往内容快照里累加一条 `base -> count`（条目满了就标 overflow
		//   —— 快照不完整时记账**不许**下「拿空」结论，宁可少灭一次）。
		void AddToSnap(DcSnapOut& a_snap, std::uint32_t a_baseFid, std::uint32_t a_count)
		{
			for (std::uint8_t i = 0; i < a_snap.count; ++i) {
				if (a_snap.base[i] == a_baseFid) {
					a_snap.cnt[i] += a_count;
					return;
				}
			}
			if (a_snap.count >= kDcSnapMax) {
				a_snap.overflow = true;
				return;
			}
			a_snap.base[a_snap.count] = a_baseFid;
			a_snap.cnt[a_snap.count]  = a_count;
			++a_snap.count;
		}

		int RefLootState(const RE::TESObjectREFR* a_ref, bool* a_outSawTemporary, DcSnapOut* a_outSnap)
		{
			// ★ v4.10：`a_outSawTemporary`（调用方传了才写）—— 本轮有没有见到
			//   `kTemporary` 投影条目。展示柜的「拿空即灭」判据要用（常量区「v4.10」）。
			if (a_outSawTemporary) {
				*a_outSawTemporary = false;
			}
			// ★ v4.13：内容快照（记账基准）。调用方传了它 ⇒ 这一轮**不早退**，
			//   把整份库存走完（只对展示柜开这条支路）。
			if (a_outSnap) {
				*a_outSnap = DcSnapOut{};
			}
			if (g_invOff == 0) {
				return -3;
			}
			const auto* raw = reinterpret_cast<const std::uint8_t*>(a_ref);
			// ★★ v4.15（崩溃 B 的修复，docs/15 §4）：**这一读改成 SafeReadMem**。
			//   实测现场：`ref` 已经被释放、那块堆内存被别的数据（一串浮点）复用，
			//   于是 `*(ref+0xA0)` 读出垃圾 `0x1A486`；它是「> 0x10000」的合法范围值，
			//   `IsPlausiblePointer` 放行 ⇒ 下面裸读 `*(inv+0x28)` 直接 AV（+0x2023A）。
			//   SafeReadMem（ReadProcessMemory）对不可读地址**只会返回 false**，不抛异常
			//   ⇒ 读不出来一律按「未知」处理（返回 -1 ⇒ 照常亮，绝不误灭）。
			std::uint64_t ptrRaw = 0;
			if (!SafeReadMem(raw + g_invOff, &ptrRaw, sizeof(ptrRaw))) {
				return -1;
			}
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
			const auto* inv = reinterpret_cast<const std::uint8_t*>(ptrRaw);
			// ★ v4.15：库存数组头三个字段也走 SafeReadMem（原因同上：指针可能已经失效）
			std::uint32_t size = 0;
			std::uint32_t cap  = 0;
			std::uint64_t data = 0;
			if (!SafeReadMem(inv + kOffInvData, &size, sizeof(size)) ||
				!SafeReadMem(inv + kOffInvData + 4, &cap, sizeof(cap))) {
				return -1;  // 读不出来 = 指针失效，不判空
			}
			if (size > kInvMaxItems || cap < size || cap > (1u << 20)) {
				return -1;  // 形状不对劲 = 读到垃圾，不判空
			}
			if (size == 0) {
				if (a_outSnap) {
					a_outSnap->count = 0;  // 一条都没有 ⇒ 快照就是「空」（这是有效信息！）
				}
				return 0;  // 一条都没有 = 空的
			}
			if (!SafeReadMem(inv + kOffInvData + 8, &data, sizeof(data)) ||
				!IsPlausiblePointer(data)) {
				return -1;
			}

			const std::uint32_t items = std::min<std::uint32_t>(size, kInvWalkItemsMax);
			bool                complete = (size <= kInvWalkItemsMax);
			// 快照模式：枚举上限更小（只记账，不需要全量），并且**不早退**。
			if (a_outSnap && size > kDcSnapStackBudget) {
				a_outSnap->overflow = true;
			}
			bool    hasAny = false;
			std::uint32_t snapBudget = kDcSnapStackBudget;
			std::uint32_t       budget = 1024;  // 单次判空最多看多少个 stack（防呆，正常远用不到）
			for (std::uint32_t i = 0; i < items; ++i) {
				// ★ v4.15：条目头**整块**安全读 —— 一次 SafeReadMem 拿到 0x28 字节，
				//   后面所有字段都从这个副本里取（`object` / `stacks` / `flags`），
				//   于是「条目数组本身是垃圾 / 越界」也不会 AV。
				std::uint8_t itemBuf[kOffInvItemSize]{};
				if (!SafeReadMem(reinterpret_cast<const std::uint8_t*>(data) + i * kOffInvItemSize,
						itemBuf, sizeof(itemBuf))) {
					return -1;  // 条目读不出来 ⇒ 不敢断言「空」
				}
				const auto* item = itemBuf;
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
					if (a_outSawTemporary) {
						*a_outSawTemporary = true;  // ★ v4.10：调用方（展示柜判决）要用
					}
					if (a_outSnap) {
						a_outSnap->sawTp = true;
					}
					RecordDisplayCaseRuntime(a_ref);
				}
				// ★★ v4.5：跳过「非玩家物品」（记录标志 0x04）—— NPC 穿在身上的隐形装备
				//   （`*_NOTPLAYABLE`），玩家拿不走、搜刮面板也不显示。把它们算作
				//   「有东西」正是「拿空还亮」的根因（实证见常量区 kFormFlagNonPlayable）。
				//   跳过之后：面板拿空 ⇒ 这里数到的 count 全 0 ⇒ 判「空」⇒ 熄灭。
				//   ★ 只有 `SkipNonPlayableLoot=1`（默认）时才跳；设 0 = 退回旧行为。
				if (g_cfg.skipNonPlayableLoot && IsNonPlayableForm(obj)) {
					++g_state.lootSkipNonPlayable;
					if (a_outSnap) {
						a_outSnap->uncertain = true;  // ★ v4.13：快照不完整 ⇒ 记账不许断言「拿空」
					}
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
					if (a_outSnap) {
						a_outSnap->uncertain = true;  // ★ v4.13：同上
					}
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
				std::uint32_t entryCnt = 0;  // 这一个条目所有 stack 的正数合计（快照模式用）
				for (std::uint32_t j = 0; j < sn; ++j) {
					// ★ v4.15：每个 stack 的 count 走 SafeReadMem（指针可能已失效）
					std::uint32_t cnt = 0;
					if (!SafeReadMem(reinterpret_cast<const std::uint8_t*>(sd) + j * kOffStackSize + kOffStackCount,
							&cnt, sizeof(cnt))) {
						return -1;  // 读不出来 ⇒ 不敢断言「空」
					}
					if (cnt > 0) {
						if (!a_outSnap) {
							return 1;  // 有东西，早退（老路径：只关心有没有）
						}
						// ★ v4.13：快照模式 —— 把该条目**所有 stack** 的正数加起来
						//   （一个条目可能有多个 stack；只取第一个会少记 ⇒ 之后
						//    「拿走的比记的多」被标成不可信 ⇒ 白丢一次判定）。
						//   上限 1<<20 与 count 的形状校验同一口径（防溢出）。
						if (entryCnt < (1u << 20)) {
							entryCnt += cnt;
						}
					}
					if (--budget == 0) {
						return -1;  // 条目/stack 多得离谱 ⇒ 不敢断言「空」
					}
				}
				if (a_outSnap && entryCnt > 0) {
					// ★ v4.15：物品的记录 ID 也走 SafeReadMem（读不出来就把快照标成不确定）
					std::uint32_t baseFid = 0;
					if (SafeReadMem(reinterpret_cast<const void*>(obj + kOffFormID), &baseFid, sizeof(baseFid))) {
						hasAny = true;
						AddToSnap(*a_outSnap, baseFid, entryCnt);
					} else {
						a_outSnap->uncertain = true;  // 记账基准不完整 ⇒ 不许断言「拿空」
					}
				}
				if (a_outSnap && --snapBudget == 0) {
					a_outSnap->overflow = true;  // 枚举预算用完 ⇒ 快照不完整
					break;
				}
			}
			if (!complete) {
				if (a_outSnap) {
					a_outSnap->overflow = true;
					return hasAny ? 1 : -1;
				}
				return -1;  // 条目太多没走完 ⇒ 不敢断言「空」
			}
			if (a_outSnap) {
				return hasAny ? 1 : 0;  // 快照走完了：有没有东西按累计结果给
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
			g_state.contProbed.clear();       // ★ v4.8
			g_state.displayCaseVerdict.clear();  // ★ v4.10（旧世界的引用随时可能被销毁）
			// ★★★ 订正 R4：判空缓存同样是「旧世界的引用 → 结论」⇒ 一起作废
			//   （不清的话：指针被回收后，新的容器会读到别的容器的旧结论）。
			g_state.lootMemo.clear();
			++g_state.lootMemoSerial;
			// ★ v4.11：逐帧观察表里存的也是「旧世界」的引用 ⇒ 一起作废
			//   （不清的话，指针被回收后再读就是野指针；虽然每帧都有 IsReadable 兜底，
			//    但这里清掉才是干净的）。
			g_state.dcWatchCount = 0;
			g_state.dcUiProbeLastMs = 0;
			// ★★★ v4.25 / v4.26：换场景 / 读档 ⇒ 「星球目标是否已扫描」的判据作废
			//   （换了一个存档 = 勘测数据可能完全不同；缓存不能跨世界复用）。
			//   ★ 直接用 clear()（而不是 InvalidateFloraScannedCache）—— 这里在
			//     flora 判据的定义之前，用不着为了打一行日志去挪一大堆声明。
			g_state.floraScannedCache.clear();
			// ★★★ v5.1.5（订正 R5）：worldspace 缓存也是「旧世界的 cell 指针 → worldspace」
			//   ⇒ 一起作废（cell 指针可能被释放 / 重排）。
			g_state.floraCellWsCache.clear();
			// ★ v4.26：「从引擎状态里学到的绿」也要一起作废 —— 换了存档，
			//   上一个世界的勘测数据与新存档无关。
			// ★★★ v4.33：改成**默认不清**（INI `FloraLearnClearOnLoad=1` 才清）——
			//   实测日志（docs/31 §一）里 `reset (载入画面关闭…)` 每几十秒一次，
			//   每次清空学习表后重问引擎大多答「未扫描」⇒ 换场景后整片植物变青，
			//   必须再开一次扫描仪才恢复。而「已扫描」是**单向**的（勘测数据不退回），
			//   跨场景 / 快速旅行保留它才是正确语义。
			//   ★ v5.1：粒度改成**引用** —— 保留的也只是「这一个实例扫过」这件事。
			//   ★★★ v5.1.2（订正 R2）：**按物种（base）扩散表**同样保留 ——
			//     换场景 / 快速旅行之间保留「这个物种已学习」才是正确语义
			//     （引擎知识库是 species 级、且不随场景退回）；只有配置要求
			//     清空学习表（FloraLearnClearOnLoad=1）时跟着一起清。
			if (g_cfg.floraLearnClearOnLoad) {
				const auto n = g_state.floraRefKnow.size();
				const auto nb = g_state.floraBaseKnow.size();
				g_state.floraRefKnow.clear();
				g_state.floraBaseKnow.clear();  // ★ v5.1.2：物种表跟着清（保持同一语义）
				REX::INFO("flora learn: 按引用记忆按配置清空（FloraLearnClearOnLoad=1，清了 {} 条引用 / "
						  "★ v5.1.2 物种表 {} 个 base）",
					n, nb);
			} else if (!g_state.floraRefKnow.empty() || !g_state.floraBaseKnow.empty()) {
				REX::INFO("flora learn: 记忆保留（{} 条引用 / ★ v5.1.2 物种表 {} 个 base；"
						  "★ v4.33 起换场景 / 读档不再清空、★ v5.1 粒度 = 引用、"
						  "★ v5.1.2 同 base 实例一起变绿）",
					g_state.floraRefKnow.size(), g_state.floraBaseKnow.size());
			}
			// ★ v4.13：事件队列里排队的记录也是「旧世界」的（FormID / 指针随时可能作废）
			//   ⇒ 一起丢掉，免得换场景后按旧 FormID 去减账。
			{
				std::scoped_lock lock{ g_lootEvtLock };
				g_lootEvtQueueCount = 0;
			}

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
			// ★★★ v5.0：自建通道号（ChannelMode=1 时用；旧路径忽略）。
			//   = 该目标该挂到哪条自建颜色通道（见 docs/32 与 EnsureChannels）。
			std::uint32_t      channel = 0;
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

		// ================================================================
		// ★★★ v5.1：引擎「引用 → outline 状态」红黑树（**只读**查询用，绝不插入）
		// ----------------------------------------------------------------
		// 证据（1.16.244.0，`tools/re/disasm.py`）：
		//   · 0x17D5BE0（LookupOrAdd）里树头地址 = `lea rbp,[rip+0x4764066]`
		//     ⇒ RVA **0x5F39CE0**；同一地址在函数开头是 `mov rbx,[rip+0x47640e2]`
		//     （读 = 取哨兵节点指针）。
		//   · 节点布局（MSVC _Tree，逐条从 0x17D5BE0 / 0x3222E0 对出来）：
		//       +0x00 左子 / +0x08 父 / +0x10 右子 / +0x18 颜色 / +0x19 IsNil
		//       +0x20 键（= TESObjectREFR*）/ +0x28 值（= outline 状态 dword ★ 我们要的）
		//   · 树头：+0x00 = 哨兵节点指针（哨兵 +0x08 = 根）/ +0x08 = 条目数
		//     （0x3222E0 = 插入函数，第一条指令就是 `inc qword ptr [rcx+8]`，rcx = 0x5F39CE0）。
		//   · 「查不到就插入」的插入路径在 0x17D5C5C 起：new 0x30 字节节点 →
		//     `lock xadd [ref+8], 0x80000200001`（对 REFR 做一次**引用计数 ++**）→
		//     写状态 0 → 插进红黑树。
		//   · 引擎拆 Monocle HUD（0x17D4B30）时会**清空这张表**：函数尾
		//     `mov [rip+0x476503f], rbp(=0)` = `树头+0x08 = 0`（条目数归零）+ 哨兵
		//     三个链接指回自己 ⇒ 「举着扫描仪时有条目、放下就没了」。
		//
		// ★★ 为什么必须自己走树、而不是继续调 0x17D5BE0（v5.1 的 FPS 修复核心）：
		//   0x17D5BE0 的名字就是 Lookup**OrAdd** —— 它会插入。v4.33 起「⓪.5 状态表
		//   捡漏」在**每个 flora 引用**每次判据未命中时都调它 ⇒
		//     ① 引擎状态表里被塞进大量我们并不需要挂高亮的条目（只增不减，直到引擎
		//        自己拆 Monocle HUD 才会清）；
		//     ② 每条都持有该引用的**引用计数** ⇒ cell 卸载后那些 REFR 也放不掉
		//        （泄漏）。
		//   这两条都会让「玩得越久，引擎越慢」—— 正是用户报告的
		//   「1.8.1 随着游戏进行帧数持续降低」最可能的来源（v4.33 = 1.8.1）。
		//   只读走树 = 零副作用（不插入 / 不动引用计数），所以「捡漏」可以一直开着。
		//
		// ★★★ 2026-09-27 订正（本轮）：v5.1 的 **0x5949CE0 是手算 rip 相对地址时
		//   少看了一位**（`[rip+0x4764066]` 的 disp 是 4 字节 = **0x04764066**，
		//   0x17D5C7A + 0x04764066 = **0x5F39CE0**）。后果：v5.1 的只读探针一直在读
		//   一段**全 FF 的无关数据**（统计行 `条目=18446744073709551615` + `读=0`
		//   `绿=0` 就是铁证）⇒ 「引擎亲手画过的 4/5」这条最硬的证据在 v5.1 里
		//   **从未被读到过** ⇒ 扫描完的植物 / 矿脉有 ~20% 只能靠主判据
		//   （GetOutlineState 会答 0/1）⇒ 用户看到的「少部分扫描后未变色」。
		//   ★ 交叉验证：管理器数组 `kRvaOutlineManagers = 0x5F39CF0` = 树头 + 0x10
		//     （= 8 字节 _Myhead + 8 字节 _Mysize）—— 两个常量正好相邻，自洽。
		// ================================================================
		constexpr std::uintptr_t kRvaOutlineStateTree      = 0x5F39CE0;
		constexpr std::size_t    kOffStateTreeNodeLeft     = 0x00;
		constexpr std::size_t    kOffStateTreeNodeRight    = 0x10;
		constexpr std::size_t    kOffStateTreeNodeIsNil    = 0x19;
		constexpr std::size_t    kOffStateTreeNodeKey      = 0x20;
		constexpr std::size_t    kOffStateTreeNodeValue    = 0x28;
		constexpr std::size_t    kOffStateTreeNodeSize     = 0x30;
		constexpr std::size_t    kOffStateTreeSentinelRoot = 0x08;
		constexpr std::size_t    kOffStateTreeCount        = 0x08;
		constexpr std::uint32_t  kOutlineStateTreeDepthMax = 256;  // 兜底（红黑树深度远小于此）

		// 引擎自己的「确保 12 个管理器都存在并把配色参数刷成当前 GMST 值」函数。
		// 不开扫描仪时引擎可能还没建它们，我们就自己叫一次（幂等）。
		// ★ 2026-09-16 复核：这个函数**只创建缺失的管理器**（`if (managers[i]) 跳过创建，
		//   仅刷新参数`，见 0x17D47B0 里 `cmp eax,[rcx+0x30]; je next` 那段），
		//   所以反复调用**不会**清空高亮 —— 想清空必须走下面的 Remove。
		constexpr std::uintptr_t kRvaOutlineEnsureManagers = 0x17D47B0;

		// ================================================================
		// ★★★ v4.19：每状态「高亮参数块」—— 配色就住在这里（地址订正，见 docs/18）
		// ----------------------------------------------------------------
		// 每状态一块，stride = **0xA0**；块内字段（dword 布局 = `0xAARRGGBB`）：
		//     +0x00  脉冲 High 色      ┐
		//     +0x20  脉冲 Low 色       │ `0x17D47B0` 建/刷 HighlightManager 时读它们，
		//     +0x40  float 插值除数    │ 算出一份 32 字节参数块交给渲染侧
		//     +0x60  float（原样透传） ┘
		//     +0x80  ★★ 描边基色 —— 挂引用（`0x17D4CD0`）时读的就是它；引擎交给
		//             渲染侧的 32 字节参数块的 +0x00 也是它 ⇒ **画出来的就是这个颜色**
		//            （实证：只覆盖 state 6 的 +0x80 就把笔记描边变成了黄色）
		//   ⇒ 块基址 = **0x5919A88**（state k 的块 = 基址 + k*0xA0）。
		//     ★ 老代码/老文档记的「mgr 表 0x591E088」= 基址 + **112**×0xA0 ——
		//       那其实是**另一片 float 常量区**（镜像里 0x44D471E0 = 1702.11f 这类值）。
		//       v4.1~v4.18 一直在往那儿写 RGB：**既没有效果，又污染了那张常量表**
		//       （E3 之后 alpha 字节被我们保留，才没把 float 直接写坏）。
		//   ★ 订正依据（2026-09-25，反汇编 `0x17D47B0` 的循环体）：
		//       `lea r14,[rip+0x414525e]` ⇒ r14 = 0x5919AE8 = 块基址 + 0x60；
		//       循环尾 `add r14,0xA0`、`sub r12,1`（r12 = 0xB = 11 次）；
		//       循环内读 [r14-0x60] / [r14-0x40] / [r14-0x20] / [r14] / [r14+0x20]，
		//       其中 [r14+0x20] = 0x5919B08 —— 正是本 MOD 一直在写的「ref 表」。
		// ★ 这两组值**不是 0 初始化**：进程启动的静态初始化函数就把原版默认配色写进去了
		//   （反汇编实证：0xF2AD3E 橙、0xFFE872 金、0x695B11 橄榄 …），之后由
		//   `:Monocle` / `aHighlightScannableOutlineColorHigh|Low_<变体>` 设置刷新。
		//   动态调试时用 `LogOutlineColors()` 直接把 11 个状态的颜色打出来对着看。
		constexpr std::uintptr_t kRvaOutlineParams     = 0x5919A88;  // 块基址（state 0 的 +0x00）
		constexpr std::size_t    kOffStatePulseHigh    = 0x00;       // 脉冲 High（dword）
		constexpr std::size_t    kOffStatePulseLow     = 0x20;       // 脉冲 Low（dword）
		constexpr std::size_t    kOffStatePulseDivisor = 0x40;       // float
		constexpr std::size_t    kOffStateExtra        = 0x60;       // float
		constexpr std::size_t    kOffStateBaseColor    = 0x80;       // ★ 描边基色（dword）

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

		// ================================================================
		// ★★★ v5.0：完全自建颜色通道（完整逆向 / 方案 / 验收见 docs/32）★★★
		// ----------------------------------------------------------------
		// 一句话：引擎的「11 条通道」只存在于「状态表 → 管理器」这一层路由上；
		//   真正画描边的渲染层是**两张按 id 动态增长的存储表**：
		//       H+0xE8 → 存储 A：manager_id → 32 字节参数（颜色/脉冲/系数）★ 画出来的颜色
		//       H+0xF0 → 存储 B：ref_id     → manager_id（4 字节）      ★ 这个 ref 用哪套参数
		//   （H = [RVA 0x59751E8]；两个存储的 +0x2C8 = idTab、+0x3C8 = 数据数组，
		//     EnsureSlot 0x2936540 / 0x2910480 都是池化动态分配，没有任何 11 项检查。）
		//
		//   ⇒ 自己调管理器 ctor（0x6532F0）就能创建**任意多条**独立通道：
		//       对象内部：vtable / +0x18 内嵌哈希表 / +0x40 = 全局句柄 id（0x7CB120 分配）
		//       ctor 末尾自动把 32 字节参数注册进存储 A（0x653850）。
		//     挂载完全复刻引擎 Set 内部（0x17D4CD0）的 3D 图 visitor：
		//       visitor{vtbl=0x4B2FA10, data=&{mgr, mgr->0x40}} + 0x24181E0（递归遍历）
		//         → 0x653DD0：写存储 B（0x653A50）+ 管理器内嵌哈希表插入
		//       visitor{vtbl=0x4B2F9F0, data=&mgr} + 0x24181E0（递归遍历）
		//         → 0x653F60：Remove(map,&id) → Deactivate(0x653040)（清存储 B）
		//     ★ 两个 visitor 的 data 语义**不同**（挂上 = {mgr,id} 值本身；
		//       摘除 = &mgr（先解引用一次才是管理器）—— v2.3 起代码里就是这么用的，本轮坐实）。
		//
		//   全程**不写状态表**（LookupOrAdd/Set 一个字节不动）、**不碰引擎配色块** ⇒
		//   原版扫描仪 / NPC / 星球目标颜色 100% 原版；引擎清表（0x17D4B30）也碰不到我们
		//   （自建管理器不在 g_outlineManagers 数组里、ref 不在状态表里）。
		// ================================================================
		constexpr std::uintptr_t kRvaOutlineHost       = 0x59751E8;  // void** → 高亮宿主 H
		constexpr std::size_t    kOffHostParamStore    = 0xE8;       // H+0xE8 = 存储 A 对象
		constexpr std::size_t    kOffStoreIdTab        = 0x2C8;      // store+0x2C8 = u32*（id→槽号）
		constexpr std::size_t    kOffStoreData         = 0x3C8;      // store+0x3C8 = u8*（数据数组）
		constexpr std::size_t    kChannelParamBytes    = 0x20;       // 存储 A 每槽 32 字节
		constexpr std::uintptr_t kRvaChannelMgrCtor    = 0x6532F0;   // HighlightManager*(mem, &params32)
		constexpr std::uintptr_t kRvaChannelMountVisor = 0x4B2FA10;  // 挂上 visitor vtable
		constexpr std::uintptr_t kRvaChannelMountCb    = 0x653FC0;   // 挂上回调（add rcx,8; jmp 0x653DD0）
		constexpr std::uintptr_t kRvaChannelDeactVtCf  = 0x653F60;   // 摘除回调（已有 vtable 0x4B2F9F0）
		constexpr std::uint32_t  kChannelMgrBytes      = 0x48;       // 管理器对象大小（与 0x17D47B0 一致）
		// 32 字节参数块里的两个颜色 dword（布局见 docs/32 §2.3）：
		constexpr std::size_t    kOffChannelBaseColor  = 0x00;       // 基色（★ 真正画出来的颜色）
		constexpr std::size_t    kOffChannelPulseColor = 0x08;       // 脉冲色
		// 通道数 = 12 个类别各一条 + 1 条「植物已扫描」（青 / 绿两态各一条）。
		constexpr std::size_t    kChannelCount         = kCategoryCount + 1;
		constexpr std::size_t    kChannelFloraScanned  = kCategoryCount;

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
		// ★ v5.0：HighlightManager ctor（`HighlightManager*(mem, &params32)`）首 16 字节签名。
		//   这是「自建颜色通道」的入口函数（见 docs/32 §2.3）。
		constexpr std::uint8_t kSigChannelMgrCtor[16] = {
			0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC
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

		// ================================================================
		// ★★★ v5.0：自建颜色通道的运行时状态（见 docs/32）
		// ----------------------------------------------------------------
		// 管理器对象用**插件静态内存**（0x48 × 13 = 936 字节）：
		//   · 引擎只在 ctor / 挂载 / 摘除时写对象内部（全部 ≤0x48 字节）；
		//   · 引擎的销毁路径（0x17D4B30）只遍历它自己的 11 个数组槽 ⇒ 永远不会 free 这块；
		//   · 我们不销毁 ⇒ 不需要引擎的 operator new/delete 配对（也就不依赖分配器 RVA）。
		// ★ 数组必须是**地址稳定**的固定大小：摘除 visitor 的 data 需要 `&slot`
		//   （管理器指针的地址）—— 用 vector 扩容搬走就越界了。
		// ================================================================
		alignas(16) std::uint8_t g_channelMgrMem[kChannelCount][kChannelMgrBytes]{};
		struct ChannelSlot
		{
			void*         mgr   = nullptr;    // 自建 HighlightManager（= g_channelMgrMem[i]）
			std::uint32_t mgrId = 0xFFFFFFu;  // 全局句柄 id（ctor 写在 mgr+0x40）
		};
		ChannelSlot   g_channels[kChannelCount]{};
		bool          g_channelsReady  = false;  // 13 条全部建好（通道模式才挂载）
		bool          g_channelsFailed = false;  // 模板标定失败（只报一次；自动回退旧路径）
		std::uint32_t g_channelRetries = 0;      // 模板重试轮数（防死循环刷日志）
		// `HighlightManager* ctor(void* mem, void* params32)`（0x6532F0）
		using ChannelMgrCtor_t = void* (*)(void*, void*);
		ChannelMgrCtor_t g_channelMgrCtor = nullptr;
		std::uintptr_t   g_channelMountVisorVt = 0;  // 挂上 visitor vtable（0x4B2FA10）

		// ================================================================
		// ★★★ v4.25：星球目标「已经扫描过吗」—— **直接问引擎**（唯一权威来源）
		// ----------------------------------------------------------------
		// 起因（用户实测反馈，v4.24 修完之后剩下的那一半）：
		//   「矿石、气体、液体、植物、动物现在扫描后，在扫描仪里的颜色是原版颜色了，
		//     但是收起扫描以后，在 MOD 的高亮颜色里还是扫描前的原版蓝色」
		// v4.24 把 state 4/5 归还引擎之后：**举着扫描仪**时原版绿已经回来了；但
		// 放下扫描仪后 MOD 只用 StateFlora（= 7 = 原版青色脉冲）一个状态重挂 ⇒
		// 「扫过的」和「没扫过的」看起来一模一样。
		//
		// 引擎自己的判据链（本机 1.16.244.0 复核反汇编 `0x159ED90` 的
		// `0x159F4C2`~`0x159F569` 段，逐条对上）：
		//
		//   0159F4C7  mov rax,[rdi+0x98]       ; rdi = ref → base
		//   0159F4D7  cmp byte [rax+0x2e],0x2e ; formType == kFLOR (0x2E) ?
		//   0159F4E1  mov rax,[rax+0x260]      ; ★ FLOR+0x260 = produceItem（LVLI）
		//   0159F4F1  cmp byte [rax+0x2e],0x3f ; formType == kLVLI (0x3F) ?
		//   0159F4FB  cmp byte [rax+0x13a],0   ; 条目数 > 0 ?
		//   0159F504  mov rax,[rax+0x120]      ; 首个条目的指针
		//   0159F50B  mov rcx,[rax]            ; 条目的 form
		//   0159F513  cmp byte [rcx+0x2e],0x28 ; formType == kMISC (0x28) ?
		//   0159F519  mov rbx,[rcx+0x238]      ; ★ MISC+0x238 = BGSCraftingResourceOwner
		//                                      ;   的 componentData（指针 → BSTArray）
		//   0159F525  mov eax,[rbx]            ; size（BSTArray size@+0）
		//   0159F52B  mov rbx,[rbx+8]          ; data（★ data@+8，与 v4.23 的订正一致）
		//   0159F540  mov rcx,[rbx]            ; 每个元素（stride 0x18）的第一个字段
		//   0159F548  cmp byte [rcx+0x2e],0x9f ; formType == kIRES (0x9F) = BGSResource ?
		//   0159F54E  call 0x1597A50           ; ★「这个资源已经扫描过（进了勘测数据）？」
		//   0159F557  state = 4 + (在范围内 ? 1 : 0) ; ⇒ 4（远）/ 5（近）= 原生绿 #27C684
		//
		// ⇒ MOD 照抄同一条链 + **同一个函数**，就能在不举扫描仪时给出与原版完全一致的
		//   「已扫描」判定（而不是自己猜）。判定为「已扫描」⇒ 挂 StateFloraScanned
		//   （默认 5 = 原版「已扫描」绿；远/近两个槽位原生色相同，所以不分远近），
		//   否则保持 StateFlora（7 = 原版青色脉冲）。
		//   ★ 全部只读：链上每一步都做形状校验（formType 断言 + 指针/可读性），
		//     任何一步不通 ⇒ 按「未扫描」处理（失败方向永远是「保持原样」）。
		constexpr std::uintptr_t kRvaIsResourceScanned = 0x1597A50;
		// 函数首 16 字节签名（游戏版本一变 RVA 就不可靠，用之前硬核对）。
		constexpr std::uint8_t kSigIsResourceScanned[16] = {
			0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57
		};
		// =========================================================================
		// ★★★ v4.28：**引擎自己的「这个引用扫没扫过」** —— 主判据
		// -------------------------------------------------------------------------
		// `RE::ScannableComponent::GetOutlineState(ref)`
		//   （commonlibsf `IDs.h` → `ScannableComponent::GetOutlineState{ REL::ID 83007 }`
		//     ⇒ `tools/re/versionlib.py id 83007` = **RVA 0x1306E80**）；
		//   返回值 **1 = 未扫描 / 2 = 已扫描**。这不是猜的，两条硬证据：
		//     ① 原生函数注册表里有一条 **`IsScanned`**（函数名字符串 RVA 0x4BCA548，
		//        注册点 0x2088F49），它的实现是 RVA **0x2076660** 的 thunk：
		//            mov rcx, r8                ; 引用（VM 参数约定）
		//            call 0x1306E80             ; GetOutlineState(ref)
		//            cmp al, 2
		//            sete al                    ; ⇒ **== 2 才算「已扫描」**
		//        （复现：`tools/re/func.py strings "IsScanned"` + `func.py rip 0x2076660`）
		//     ② 函数体内部：先取 `[ref+0xC8]` 的 ScannableComponent（`call 0x2C5C90`，
		//        组件类型 0x2A），再按 `ref+0x28`（canonical id）去
		//        **玩家知识库**（`0x23FF640` = `BSGalaxy::GetKnowledgeManager`，
		//        `+0x8B0` = knowledge DB，见 commonlibsf `RE/P/PlayerKnowledge.h`）
		//        查物种槽（`SpeciesSlot.percent` / `scanFlag`），最后 `setne al; inc al`
		//        ⇒ 1 / 2。
		//   ⇒ 语义 = **原版扫描仪 HUD 的口径**（「这个目标 / 该物种扫描过没有」），
		//     对植物 / 矿脉 / 气泉 / 液池 / 动物都成立 —— 而资源链
		//     （`kRvaIsResourceScanned`，0x1597A50）**只对「产出物品是 LVLI」的 FLOR
		//     成立**（引擎 0x159ED90：`cmp byte [rax+0x2E],0x3F / jne <跳过>`）。
		//     ★ 植物（产出 MISC 的 FLOR）在 v4.25~v4.27 走了资源链 ⇒ 产出的那个资源
		//       只要在勘测数据里，**没扫描过的植物也被判成「已扫描」⇒ 显示成绿色**
		//       （用户实测反馈）。所以 v4.28 把它提为主判据，资源链降为「LVLI 才用」。
		//   ★ 拿不到只是「星球目标不再区分扫描前后」（退回 v4.24 行为），
		//     不该影响高亮本身 ⇒ 单独签名校验、单独降级。
		constexpr std::uintptr_t kRvaScannableOutlineState = 0x1306E80;
		constexpr std::uint8_t   kSigScannableOutlineState[16] = {
			0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8B
		};  // push rbx/push rbp/push rsi/push rdi/push r14/sub rsp,0x80/mov rbx,rcx
		constexpr std::uint8_t   kScannableStateScanned = 2;  // 1 = 未扫描
		// -------------------------------------------------------------------------
		// ★★★ v4.26：链上的偏移**不再只信一个值** —— 自适应 + 形状校验
		// -------------------------------------------------------------------------
		// v4.25 实测（用户反馈「还是不行」）日志：
		//     flora scan: base=0x75CAD produceItem=0x0(LVLI) misc=0x0(MISC) … 链失败=166
		// 连 **Starfield.esm 的原版矿脉**都读不出「产出物品」⇒ 偏移/类型判断有错。
		// 离线核对（`out/scan_flor_pfig.py`，扫 Starfield.esm）：
		//     0x75CAD = FLOR `MineralDepositCommonWaterLiquid01` → **PFIG = LVLI** 0x163A11
		//     0x25232B = FLOR `MineralDepositCommonLead01`  → **PFIG = LVLI** 0x163A0E
		//     0x383287 = FLOR `ResourceRockLiquid01`        → 无 PFIG（这类不走「已扫描」）
		// 而引擎另一处代码（`0x1479549` / `0x1479AC9`，同样先判 formType==0x2E）把
		// **同一个 +0x260 字段当 MISC(0x28)** 用，再走 `MISC+0x238` → 元素 stride 0x18
		// → 只要 `formType==0x9F`(IRES) ⇒ 同一条链的另一个入口。
		// ⇒ 结论：这个字段（produceItem）在运行期**可能装 MISC 也可能装 LVLI**，
		//   而且内存偏移必须实测确认（commonlibsf 与引擎读法差 8 字节，两处都见过）。
		//   v4.26 的做法 = **带形状校验的自适应搜索**（与 v4.23 资源关键词那套同一思路）：
		//     ① 先试引擎/文档记录过的偏移（0x260、0x258）；
		//     ② 不成就在 0x180~0x380 里逐 8 字节找一个「能把整条链走通」的指针：
		//        (MISC 或 LVLI[取其首条目的 form]) → MISC+0x238 资源数组
		//        → 元素 stride 0x18 里有 **IRES(0x9F)** —— 这一步的形状校验极强，
		//        垃圾指针不可能同时满足；
		//     ③ 找到就**记进会话级变量**，后面所有 FLOR 都用这两个偏移（热路径只读 2~3 次）；
		//     ④ 全书走不通 ⇒ 打一条**指针窗口探针**（前 3 个 base，带 base 的 vtable
		//        RVA 与 produceChance 交叉校验），按「未扫描」处理。
		constexpr std::size_t kOffFloraProduceCandidates[] = { 0x260, 0x258 };  // 先试这两个
		constexpr std::size_t kFloraPtrScanLo = 0x180;  // 兜底窗口（含，8 字节步进）
		constexpr std::size_t kFloraPtrScanHi = 0x380;  // 兜底窗口（含）
		constexpr std::size_t kOffLvliCount      = 0x13A;  // LVLI 条目数（byte）
		constexpr std::size_t kOffLvliFirstEntry = 0x120;  // LVLI 首个条目（指针）
		constexpr std::size_t kOffMiscResArray   = 0x238;  // MISC → 资源数组对象（指针）
		constexpr std::size_t kOffResElemStride  = 0x18;   // 资源数组元素 stride
		constexpr std::uint32_t kFloraResElemMax = 16;     // 元素数上限（防垃圾 size）
		constexpr std::uint32_t kFloraResMax     = 8;      // 一次最多取几个资源做判定
		constexpr std::uint32_t kFloraProbeBases = 3;      // 窗口探针最多几个 base
		// ★★★ v4.26：引擎给「星球目标」写的那几个 outline 状态（见上面反汇编）：
		//   4/5 = **已扫描**（far/near，原生绿 #27C684）、7/8 = **未扫描**（青脉冲）。
		//   MOD 举着扫描仪时**让位**，但可以读一眼引擎写的结果并学下来（零逆向的证据）。
		constexpr std::uint32_t kFloraStateEngineGreenA = 4;
		constexpr std::uint32_t kFloraStateEngineGreenB = 5;
		constexpr std::uint32_t kFloraStateEngineCyanA  = 7;
		constexpr std::uint32_t kFloraStateEngineCyanB  = 8;

		// ================================================================
		// ★★★ v5.2：植物「已扫描」= **直读引擎的扫描进度表**（抛弃自建记忆；用户 2026-09-27 指令）
		// ----------------------------------------------------------------
		// 为什么是这几个地址（全部 1.16.244.0 反汇编实证，`out/dis_159ED90.txt` /
		// `dis_130A270.txt` / `dis_1307180.txt` / `dis_24105D0.txt` / `dis_23467B0.txt`）：
		//
		//   引擎的「逐引用求值」函数 0x159ED90 在**给已扫描植物写 state 4/5** 时走
		//   `0x159F32F~0x159F4B2` 段（这段对**所有类型**共用，植物正是在这里拿到 4/5）：
		//     0130A2BA  movzx r8d, word [rip+0x4ed7ad2]  ; 类型常量（key 高 16 位）
		//     0130A2C6  edx = [key1]                     ; ★ key1 = 0x7BCBD0 的直路输出
		//     0130A2CB  r8 = ((常量<<32)|key1) << 16     ; key = 常量<<48 | key1<<16
		//     0130A2EB  call 0x24105D0([mgr+0x268], &out, &key)   ; ① 主表查找（只读）
		//     0130A31A  edi = word[bucket + idx*4 + 0x12]         ; ② 二级基址 = bucket+edi
		//     0130A32F  call 0x23467B0(base2+0x38, &key2)         ; ③ 二级查找（只读）
		//     0130A346  r14d = byte [elem + 0x20]                 ; ★★ 进度 byte（0..100）
		//     0130A34F  [out_byte] = r14b
		//     …
		//     0159F3AE  r12b = [rbp+0xe0]（= 上面写的 out_byte）
		//     0159F3B6  cmp r12b, 0x64                    ; == 100 ?
		//     ⇒ 4 + ([rbp+0xe8] ? 1 : 0) = state 4/5（原生绿 #27C684）
		//   上游：
		//     · key1 = `[0x81 组件 + 0x28]`：0x159ED90 经 `0x7BCBD0(ref, &k1, &k2)` 拿它；
		//       直路 = 组件存在且 [+0x28] 非 0（本实现**直接读内存**，不去碰 0x7BCBD0 ——
		//       它在组件缺失时会走一条带 `lock xadd` + 表写的兜底路径）。
		//     · key2 = `0x1307180(ref)`（0159F34B call 的纯查询）：有 0x2A 组件 ⇒ 返回
		//       `[ref+0x28]`（FormID）；否则查主表取 `[entry+0x24]`（映射值）。
		//     · mgr  = `[[0x23FF640() + 0x8B0]`（TLS 单例懒初始化；+0x8B0 = 知识库 DB，
		//       即 commonlibsf `RE/P/PlayerKnowledge.h` 的 `PlayerKnowledge`）。
		//     · 0x81 组件 = 可扫描组件容器里的"物种/资源"条目（0x7BCBD0 以 id 0x81 取它）；
		//       0x81 组件 +0x30 → 另一对象 +0x28 = 实例知识 ID（out2，本实现不需要）。
		//   ★ 和旧主判据（`GetOutlineState` 0x1306E80）的区别：0x1306E80 的路径
		//     0x7B8260 查的是**实例级**红黑树（`[base2+0x80]`，键 = {d0,d1,d2}）——
		//     对植物查不到（用户实测「问=446 已扫描=0」）；本判据查的是**物种级**
		//     哈希（`[base2+0x38]`，键 = 知识 ID），**和引擎画 4/5 用的是同一份数据**。
		//   ★ 全程只读：0x24105D0 / 0x23467B0 是 FNV-1a 哈希查找（不改表）；
		//     0x1307180 内部只查组件 / 查表（引用计数 ++/-- 平衡）；0x347170 组件查询
		//     走读锁；0x23FF640 是 TLS 懒初始化单例。**不碰** 0x159ED90（写状态 + 登记）、
		//     也**不碰** 0x130A270 本身（它会写一个 vector：本实现只复刻它的读表段）。
		constexpr std::uintptr_t kRvaFloraKnowledgeId = 0x1307180;  // ref → 知识 ID（key2）
		constexpr std::uint8_t   kSigFloraKnowledgeId[16] = {
			0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83
		};
		constexpr std::uintptr_t kRvaFloraGetComponent = 0x347170;  // GetComponent(容器, id) → 组件指针
		constexpr std::uint8_t   kSigFloraGetComponent[16] = {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57
		};
		constexpr std::uintptr_t kRvaFloraHashFind = 0x24105D0;  // 主表查找（只读）
		constexpr std::uint8_t   kSigFloraHashFind[16] = {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x33
		};
		constexpr std::uintptr_t kRvaFloraSubFind = 0x23467B0;  // 二级哈希查找（只读，FNV-1a）
		constexpr std::uint8_t   kSigFloraSubFind[16] = {
			0x4C, 0x8B, 0x51, 0x30, 0x4D, 0x8B, 0xCA, 0x4D, 0x85, 0xD2, 0x0F, 0x84, 0x81, 0x00, 0x00, 0x00
		};
		constexpr std::uintptr_t kRvaFloraSingleton = 0x23FF640;  // TLS 单例（知识库宿主）
		constexpr std::uint8_t   kSigFloraSingleton[16] = {
			0x48, 0x83, 0xEC, 0x28, 0xBA, 0xB8, 0x00, 0x00, 0x00, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00
		};
		// ★★★ 订正 R11（★ 关键）：key1 的**兜底来源** —— `0x910690(out1, ctx16, ref, 0)`。
		//
		// 实测证据（用户 2026-09-27 13:10~13:12 那一局）：直读判据 100% 停在 stage=3
		//   （`0x81 组件拿不到`，`问=0 失败=63`）—— 但同一批引用的 `GetOutlineState`
		//   （内部用 `0x2C5C90` 只查「组件存在位图」）工作正常 ⇒ `[ref+0xC8]` 容器解读没错，
		//   是这些引用**本来就没有 0x81 组件**（组件挂在别的宿主上，不在植物 ref 上）。
		//   引擎自己 (0x7BCBD0) 在「组件缺失 / [comp+0x28]==0」时**并不放弃** ——
		//   它调本函数从 cell / 世界空间数据里把 key1 兜底找出来（0x7BCC63 分支）；
		//   0x159ED90（引擎画绿）与 0x1306E80（GetOutlineState）都会走到这条兜底。
		//   R10 只复刻了组件主路径 ⇒ 漏了兜底 ⇒「已扫描的植物还是青 + 开关扫描仪不自愈」。
		//
		// 语义（反汇编 0x910690，RVA 见下）：
		//   · rcx = out1（uint32，*out = key1 的结果；进入时被清零）
		//   · rdx = ctx（16 字节缓冲，函数**会写**它 —— 传可写局部变量）
		//   · r8  = ref（TESObjectREFR*）
		//   · r9  = 0（0x7BCBD0 传的就是 0 ⇒ 走「从 [ref+0xB0]（parent cell）开始」的分支）
		//   · 返回 rax = out1；key1 为 0 = 没找到（引擎口径：视为「无物种 ID」）
		// 内部第 1 步也是 `GetComponent([ref+0xC8], 0x81)` ⇒ 组件存在时与主路径同源；
		// 缺失时走 cell / worldspace / 全局兜底链（只读查询 + 引用计数配平）。
		// ★ 副作用面已被历史验证：我们自 v4.25 起就在调 0x1306E80（GetOutlineState），
		//   它对「有 0x2A 组件」的引用**必然**调用 0x7BCBD0 → 组件失败时即本函数；
		//   那些版本从未出现崩溃 / 表增长 / 卡顿回归 ⇒ 本次直接调用不引入新副作用面。
		constexpr std::uintptr_t kRvaFloraKey1Fallback = 0x910690;  // (out1, ctx16, ref, 0) → out1
		constexpr std::uint8_t   kSigFloraKey1Fallback[16] = {
			0x48, 0x89, 0x5C, 0x24, 0x10, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41
		};
		// 0x130A270 里 key「类型 word」的提取点（`44 0F B7 05 disp32` = movzx r8d, word [rip+disp]）：
		//   启动时验证这 4 字节，再从 disp 算出 **word 的地址** —— RVA 表一变也不会读错地址。
		//   ★★★ 订正 R10（★ 关键）：这个 word 落在 `.data` 的**未初始化段**
		//   （目标 RVA `0x61E1D94` 超出 `.data` 的 raw size ⇒ 文件里根本没有它，
		//   是运行时才被写入的）—— **启动时读它必然是 0**，R9 就是因此把整条新判据
		//   禁用了（实测启动日志 `key 类型常量异常（0）` + 统计行 `ready=0 问=0`）。
		//   引擎自己也是**每次查询现读**这条指令 ⇒ 我们也改成现读（见
		//   `QueryFloraScanProgressDirect`），这样构造出的 key 与引擎写入时用的完全一致。
		constexpr std::uintptr_t kRvaFloraKeyTypePatch = 0x130A2BA;
		constexpr std::uint8_t   kPatFloraKeyType[4] = { 0x44, 0x0F, 0xB7, 0x05 };
		// 偏移与形状常量（全部来自上面那段反汇编）：
		constexpr std::size_t   kOffRefrComponentContainer = 0xC8;   // REFR → 组件容器
		constexpr std::uint8_t  kFloraScanCompId           = 0x81;   // 可扫描组件 id（物种/资源）
		constexpr std::size_t   kOffFloraCompSpecie        = 0x28;   // [组件+0x28] = key1
		constexpr std::size_t   kOffFloraMgrDb             = 0x8B0;  // 单例 + 0x8B0 = 知识库 DB
		constexpr std::size_t   kOffFloraDbTable           = 0x268;  // DB + 0x268 = 主表
		constexpr std::size_t   kOffFloraLookupBucket      = 0x10;   // 查找输出 +0x10 = 桶数据
		constexpr std::size_t   kOffFloraLookupIndex       = 0x18;   // 查找输出 +0x18 = 索引
		constexpr std::uint64_t kFloraLookupSentinel       = 0xFE0;  // +0x18 == 0xFE0 且 +0x10 == 0 = 未找到
		constexpr std::size_t   kOffFloraSubEntryOff       = 0x12;   // word：桶内 → 二级基址的偏移
		constexpr std::size_t   kOffFloraSubTable          = 0x38;   // 二级表（0x23467B0 的 this）
		constexpr std::size_t   kOffFloraSubArray          = 0x60;   // 二级元素数组
		constexpr std::size_t   kOffFloraSubCapacity       = 0x68;   // 二级容量（迭代器 == 它 = 未找到）
		constexpr std::size_t   kFloraSubElemStride        = 0x30;   // 二级元素 stride
		constexpr std::size_t   kOffFloraElemProgress      = 0x20;   // 元素 +0x20 = 进度 byte
		constexpr std::uint8_t  kFloraProgressFull         = 100;    // 0x64 = 100% = 已扫描（引擎口径）
		constexpr std::uint64_t kFloraProgressFullTtlMs    = 600000;  // 「已扫描」缓存 10 分钟（读档会清）
		constexpr std::uint64_t kFloraProgressFailTtlMs    = 10000;   // 查询失败缓存 10 秒（避免每轮重撞）
		constexpr std::size_t   kFloraProgressCacheMax     = 4096;    // 缓存条数上限（兜异常增长）
		// ★★★ 订正 R10：`flora progress probe:` 诊断条数上限（本会话）—— key 类型 word
		//   改为现读后，前几条能看清「现读值 + 停在哪一层」，一次日志就能定位。
		constexpr std::uint32_t kFloraProgProbeMax         = 8;

		// 判据缓存的有效期（毫秒）：扫描状态只会在「举着扫描仪扫到」时变化，所以这个
		// 值只影响「扫描完成 → 放下扫描仪」之后的刷新速度；放下扫描仪那一刻还有一次
		// 强制作废（见 InvalidateFloraScannedCache 的调用点）。
		// ★ v4.26：从 2000 提到 30000 —— 链的解析结果（偏移）也一起缓存，
		//   免得每 2 秒把「窗口搜索」重跑一遍（那是几十次内存读）。
		constexpr std::uint64_t kFloraScanCacheTtlMs = 30000;
		constexpr std::size_t   kFloraScanCacheMax   = 4096;  // 缓存条数上限（兜异常增长）

		using IsResourceScanned_t = bool (*)(RE::TESForm*);
		IsResourceScanned_t g_isResourceScanned      = nullptr;
		bool                g_isResourceScannedReady = false;

		// ★★★ v4.28：引擎自己的 `GetOutlineState(ref)`（1 = 未扫描 / 2 = 已扫描）
		using GetOutlineState_t = std::uint8_t (*)(RE::TESObjectREFR*);
		GetOutlineState_t g_scannableOutlineState      = nullptr;
		bool              g_scannableOutlineStateReady = false;

		// ★★★ v5.2：直读引擎扫描进度表用的 5 个**纯查询**函数（证据链见常量区 v5.2 段）。
		//   全部「签名 + 形状 + SEH」三重保护；任何一项不过 ⇒ g_floraProgressReady=false
		//   ⇒ 新判据整体不生效（自动回退到原判据链）。
		using FloraKnowledgeId_t  = std::uint32_t (*)(RE::TESObjectREFR*);           // ref → 知识 ID
		using FloraGetComponent_t = void* (*)(void*, std::uint8_t);                  // (容器, id) → 组件
		using FloraHashFind_t     = void* (*)(void*, void*, const void*);            // (表, &out, &key) → out
		using FloraSubFind_t      = std::uint64_t (*)(void*, const void*);           // (子表, &key) → 迭代器
		using FloraSingleton_t    = void* (*)();                                     // TLS 单例
		// ★★★ 订正 R11：key1 兜底（组件缺失时引擎自己的路径，见常量区 kRvaFloraKey1Fallback）：
		//   rcx = out1（uint32*）、rdx = ctx（16 字节，**会被写**）、r8 = ref、r9 = 0；返回 out1。
		using FloraKey1Fallback_t = std::uint32_t* (*)(std::uint32_t*, void*, RE::TESObjectREFR*, void*);
		FloraKnowledgeId_t  g_floraKnowledgeId   = nullptr;
		FloraGetComponent_t g_floraGetComponent  = nullptr;
		FloraHashFind_t     g_floraHashFind      = nullptr;
		FloraSubFind_t      g_floraSubFind       = nullptr;
		FloraSingleton_t    g_floraSingleton     = nullptr;
		FloraKey1Fallback_t g_floraKey1Fallback  = nullptr;  // ★ 订正 R11（nullptr = 该兜底不可用）
		bool                g_floraProgressReady = false;  // 5 个签名 + key 类型提取点签名全过才置 1
		// ★★★ 订正 R10：key 高 16 位的「类型 word」—— 存**地址**、每次查询**现读**。
		//   该 word 在 `.data` 未初始化段（运行时才被写）⇒ 启动时读必为 0；
		//   引擎自己也是每次查询现读 ⇒ 现读才能和引擎写入时用的 key 完全一致。
		const std::uint16_t* g_floraKeyTypeWord  = nullptr;  // 现读地址（0x61E1D94）
		std::uint16_t        g_floraKeyTypeAtLoad = 0;       // 启动时读到什么（诊断：通常是 0）
		std::uint16_t        g_floraKeyTypeLast   = 0;       // 最近一次现读到的值（诊断）

		// 前置声明（定义在 ResolveNativeOutline 之后）
		std::uintptr_t OutlineManagerFor(std::uint32_t a_state);
		std::uint32_t  PrimaryState();
		// ★★★ v4.25：星球目标「已扫描」判据（定义见 SigMatches 之后）
		std::uint32_t  FloraScannedState();
		void           InvalidateFloraScannedCache(const char* a_reason);
		// ★ v4.28：现在需要**引用**（主判据 `GetOutlineState(ref)` 是按引用回答的）
		bool           FloraTargetScanned(const RE::TESObjectREFR* a_ref, const RE::TESForm* a_base);
		void           LogManagerDiagnostics(const char* a_tag);
		void           LogManagerMapDiag(const char* a_tag, std::uint32_t a_state);
		void           LogOutlineColors(const char* a_tag);
		bool           EnsureManagerFor(std::uint32_t a_state);
		bool           OutlineUnhighlightRef(RE::TESObjectREFR* a_ref, std::uint32_t a_state);
		std::uint32_t  WriteColorOverrides(const char* a_tag);
		void           ApplyColorOverrides(const char* a_tag);
		std::uint32_t  CountLiveManagers();

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

		// ================================================================
		// ★★★ v4.25 / v4.26 / v4.28：星球目标（FLOR）「已扫描」判据
		// ----------------------------------------------------------------
		// 常量与完整证据链（反汇编原文 + v4.26 的离线核对）见文件上方
		// 「kRvaIsResourceScanned」「kRvaScannableOutlineState」那两段。
		//
		// ★ v4.28 的三条证据（按可信度）：
		//   ① **`GetOutlineState(ref) == 2`**（引擎自己的 IsScanned，见常量区）
		//      —— 按引用回答，植物 / 矿脉 / 气泉 / 液池通用；
		//   ② 「引擎亲手画过绿」（`ProbeFloraEngineState` 学到的，单向、按引用）；
		//   ③ **资源链**（下面这段）—— ★ 只在「产出物品是 **LVLI**」时才走：
		//      base(FLOR) → 「产出物品」字段（某个偏移上的 form）：
		//         formType == **LVLI(0x3F)** ⇒ 取首条目（count@0x13A / 首条目@0x120）→ 必须是 MISC
		//           （**引擎 0x159ED90 就是这么判的**：`cmp byte [rax+0x2E],0x3F / jne <跳过>`；
		//            产出物品直接是 MISC 的 = **植物** ⇒ 引擎那一段根本不适用，
		//            v4.25~v4.27 在这里放行 ⇒ 「没扫描的植物也判已扫描」= 用户实测的 bug）
		//      → MISC+0x238 的资源数组 {size@0, data@8}（元素 stride 0x18，首个字段 = TESForm*）
		//      → 元素里只要出现 formType == IRES(0x9F)（= BGSResource），就调引擎自己的
		//        0x1597A50 问「这个资源已经扫描过吗」。
		//   ★ 偏移不写死：先试引擎/文档记录过的值，再窗口搜索，**标定结果会话级缓存**
		//     （与 v4.23 修「资源关键词数组」用的是同一套自校准思路）。
		// ================================================================

		// 会话级「链偏移标定」结果（0 = 还没定；见上方常量区的说明）
		std::size_t g_floraProduceOff  = 0;
		std::size_t g_floraResArrayOff = 0;

		// ① 配置里「已扫描」用的状态（默认 5 = 原版「近处已扫描」= 绿 #27C684）
		std::uint32_t FloraScannedState()
		{
			return static_cast<std::uint32_t>(std::clamp(g_cfg.stateFloraScanned, 0, 11));
		}

		// 链上各层的中间结果（诊断用；也是「形状校验过没过」的载体）
		struct FloraChain
		{
			std::size_t   produceOff = 0;  // 「产出物品」字段用的偏移
			std::size_t   arrOff     = 0;  // 资源数组字段用的偏移
			std::uint64_t produce    = 0;  // 该字段指向的 form（LVLI，或植物那种 MISC）
			std::uint64_t misc       = 0;  // 产出物品（MISC；produceIsMisc 时 == produce）
			std::uint64_t arr        = 0;  // 资源数组对象
			std::uint32_t resSize    = 0;
			std::uint32_t irES[kFloraResMax]{};  // 数组里的资源（诊断）
			std::uint32_t irESCount  = 0;
			bool          anyScanned = false;
			bool          shapeOk    = false;  // 整条链的形状校验都过了
			// ★ v4.28：产出物品字段直接就是 **MISC**（= 植物，引擎那一段要求 LVLI）
			//   ⇒ 这条链**不适用**；shapeOk 会置 true、并且**不算「链失败」**。
			bool          produceIsMisc = false;
		};

		// 读 8 字节（读不到就返回 false，并把结果清零）
		bool ReadU64At(std::uint64_t a_addr, std::uint64_t* a_out)
		{
			*a_out = 0;
			return SafeReadMem(reinterpret_cast<const void*>(a_addr), a_out, sizeof(*a_out));
		}

		// 取一个 form 指针的 formType / formID（不像 form 就返回 false）
		bool FormTypeIdOf(std::uint64_t a_form, std::uint8_t* a_outFt, std::uint32_t* a_outId)
		{
			if (!IsPlausibleFormPtr(a_form)) {
				return false;
			}
			const auto* raw = reinterpret_cast<const std::uint8_t*>(a_form);
			*a_outFt = raw[kOffFormType];
			*a_outId = *reinterpret_cast<const std::uint32_t*>(raw + kOffFormID);
			return true;
		}

		// LVLI → 首个条目 → MISC（引擎的读法：条目数 byte@0x13A、首条目指针@0x120）。
		// 返回 0 表示「这个 LVLI 没有可用的 MISC 首条目」。
		std::uint64_t LvliFirstMisc(std::uint64_t a_lvli)
		{
			const auto* raw = reinterpret_cast<const std::uint8_t*>(a_lvli);
			if (raw[kOffLvliCount] == 0) {
				return 0;  // 空 LVLI（合法，只是没有产出）
			}
			std::uint64_t entryPtr = 0;
			std::uint64_t first    = 0;
			std::uint8_t  ft       = 0;
			std::uint32_t id       = 0;
			if (!ReadU64At(a_lvli + kOffLvliFirstEntry, &entryPtr) || !IsPlausiblePointer(entryPtr) ||
				!ReadU64At(entryPtr, &first) || !FormTypeIdOf(first, &ft, &id) ||
				ft != static_cast<std::uint8_t>(RE::FormType::kMISC)) {
				return 0;
			}
			return first;
		}

		// 「产出物品」字段 → MISC。★ v4.28：**只认 LVLI**（引擎 0x159ED90 的口径）。
		//   v4.25~v4.27 这里还放行了「字段直接是 MISC」，理由是引擎另一处代码
		//   （0x1479549 / 0x1479AC9）把同一个 +0x260 当 MISC 用 —— 但**那处不是
		//   判「已扫描」的那一段**；扫描求值函数 0x159ED90 明确要求 LVLI：
		//       0159F4F1  cmp byte [rax+0x2e], 0x3f   ; formType == kLVLI ?
		//       0159F4F5  jne 0x159f57f              ; ★ 不是 LVLI ⇒ 整段跳过
		//   而产出物品直接是 MISC 的 FLOR 恰好就是**植物**（如
		//   `FloraBiomeForestConiferous08` → MISC `OrgCommonFiber`）⇒ 放行等于
		//   「拿植物产出的资源去判植物扫没扫过」= 只要那个资源（有机纤维）进过勘测
		//   数据，**没扫描过的植物也被判成「已扫描」**（用户 v4.27 实测：
		//   「植物扫描前也显示成扫描后的颜色」，而矿脉 / 气泉 / 液池是对的 ——
		//   后者的产出物品正是 LVLI）。⇒ v4.28 起这里只认 LVLI，植物的「扫没扫过」
		//   交给主判据 `GetOutlineState(ref)`（见常量区）。
		std::uint64_t MiscFromProduceLvli(std::uint64_t a_produce)
		{
			std::uint8_t  ft = 0;
			std::uint32_t id = 0;
			if (!FormTypeIdOf(a_produce, &ft, &id)) {
				return 0;
			}
			if (ft != static_cast<std::uint8_t>(RE::FormType::kLVLI)) {
				return 0;  // ★ MISC（植物）/ 其它类型 ⇒ 这条链不适用
			}
			return LvliFirstMisc(a_produce);
		}

		// 资源数组 {size@+0, data@+8} 的形状校验：元素 stride 0x18，首个字段必须是
		// 一个 form 指针，且**至少有一个元素是 IRES(0x9F)**（引擎两处代码都是这么筛的）。
		// 这一步极强：垃圾指针不可能同时满足「size 合理 + data 可读 + 元素是 IRES」。
		bool ReadResourceArray(std::uint64_t a_arr, std::uint64_t* a_outData, std::uint32_t* a_outSize)
		{
			*a_outData = 0;
			*a_outSize = 0;
			if (!IsPlausiblePointer(a_arr) || !IsReadable(reinterpret_cast<const void*>(a_arr), 0x10)) {
				return false;
			}
			std::uint32_t size = 0;
			std::uint64_t data = 0;
			if (!SafeReadMem(reinterpret_cast<const void*>(a_arr), &size, sizeof(size)) ||
				!SafeReadMem(reinterpret_cast<const void*>(a_arr + 8), &data, sizeof(data))) {
				return false;
			}
			if (size == 0 || size > kFloraResElemMax || !IsPlausiblePointer(data) ||
				!IsReadable(reinterpret_cast<const void*>(data),
					static_cast<std::size_t>(size) * kOffResElemStride)) {
				return false;
			}
			// ★ v4.27：逐元素的口径**与引擎完全对齐**（0x159F540 那段循环）：
			//   `mov rcx,[rbx]`（元素首个字段 = form）/ `test rcx,rcx / je next`（**空元素跳过**）/
			//   `cmp byte [rcx+0x2e],0x9f`（**只认 IRES**，其余类型也跳过）。
			//   ★ 注意：这里**不能**再用「ft ≤ 0x60」那种旧判据 —— IRES(0x9F) 会全被否掉（见 IsPlausibleFormPtr 的说明）。
			bool anyIrES = false;
			for (std::uint32_t i = 0; i < size && !anyIrES; ++i) {
				std::uint64_t elem = 0;
				if (!ReadU64At(data + i * kOffResElemStride, &elem)) {
					return false;  // 数组读不动 ⇒ 整个候选作废
				}
				if (elem == 0) {
					continue;  // 引擎同款：空元素跳过
				}
				std::uint8_t  ft = 0;
				std::uint32_t id = 0;
				if (!FormTypeIdOf(elem, &ft, &id)) {
					return false;  // 不是 form ⇒ 垃圾指针，候选作废
				}
				anyIrES = ft == static_cast<std::uint8_t>(RE::FormType::kIRES);
			}
			if (!anyIrES) {
				return false;
			}
			*a_outData = data;
			*a_outSize = size;
			return true;
		}

		// ② 把整条链走通，并逐个 IRES 问引擎「已扫描？」。
		//   ★ 全部只读（只有末端的 g_isResourceScanned 是引擎调用）；
		//     任何一层不通 ⇒ false（= 按「未扫描」处理），并把 chain.shapeOk 置 false
		//     让上层打取证日志。
		bool QueryFloraResourceScanned(const RE::TESForm* a_base, FloraChain* a_out)
		{
			FloraChain out{};
			if (a_out) {
				*a_out = out;
			}
			if (!a_base) {
				return false;
			}
			const auto* raw = reinterpret_cast<const std::uint8_t*>(a_base);
			if (raw[kOffFormType] != static_cast<std::uint8_t>(RE::FormType::kFLOR)) {
				return false;
			}

			// ---- ① 找「产出物品」字段 ----
			std::uint64_t produce = 0;
			std::uint64_t misc    = 0;
			std::size_t   produceOff = 0;
			// ★ v4.28：`a_miscDetect` = 允许把「字段直接是 MISC」记成「植物」
			//   （只有**已知偏移**才允许：`0x260` / `0x258` 是引擎 / 文档验证过的位置；
			//    窗口搜索里的 MISC 可能只是同一条记录上的别的指针，不能当产出物品）。
			auto tryProduce = [&](std::size_t a_off, bool a_miscDetect) -> bool {
				std::uint64_t p = 0;
				if (!SafeReadMem(raw + a_off, &p, sizeof(p)) || !IsPlausiblePointer(p)) {
					return false;
				}
				std::uint8_t  ft = 0;
				std::uint32_t id = 0;
				if (!FormTypeIdOf(p, &ft, &id)) {
					return false;
				}
				if (ft == static_cast<std::uint8_t>(RE::FormType::kMISC)) {
					// ★ 产出物品直接是 MISC = **植物**（引擎 0x159ED90 要求 LVLI）
					//   ⇒ 这条链不适用；只记下来给探针 / 日志看（v4.28 的 bug 现场）。
					if (a_miscDetect) {
						out.produceOff    = a_off;
						out.produce       = p;
						out.misc          = p;
						out.produceIsMisc = true;
					}
					return false;
				}
				const auto m = MiscFromProduceLvli(p);
				if (m == 0) {
					return false;
				}
				produce    = p;
				misc       = m;
				produceOff = a_off;
				return true;
			};
			bool found = false;
			if (g_floraProduceOff != 0 && tryProduce(g_floraProduceOff, false)) {
				found = true;  // 会话内已标定：热路径只读一次
			}
			if (!found) {
				for (const auto off : kOffFloraProduceCandidates) {
					if (tryProduce(off, true)) {
						found = true;
						break;
					}
				}
			}
			if (!found && !out.produceIsMisc) {
				for (std::size_t off = kFloraPtrScanLo; off <= kFloraPtrScanHi; off += 8) {
					if (tryProduce(off, false)) {
						found = true;
						break;
					}
				}
			}
			if (!found) {
				if (out.produceIsMisc) {
					// 植物：引擎那一段（要求 LVLI）本来就不适用 ⇒ 不算「链失败」，
					//   也不打窗口取证（shapeOk = true = 链已「走通并判定」）。
					out.shapeOk = true;
					if (a_out) {
						*a_out = out;
					}
					return false;
				}
				if (a_out) {
					*a_out = out;
				}
				return false;  // shapeOk = false ⇒ 上层打窗口探针
			}
			out.produceOff = produceOff;
			out.produce    = produce;
			out.misc       = misc;

			// ---- ② 资源数组（先试已知偏移 0x238，再窗口搜索）----
			std::uint64_t data   = 0;
			std::uint32_t resSize = 0;
			std::size_t   arrOff  = 0;
			bool          arrFound = false;
			auto tryArr = [&](std::size_t a_off) -> bool {
				std::uint64_t a = 0;
				if (!SafeReadMem(reinterpret_cast<const void*>(misc + a_off), &a, sizeof(a))) {
					return false;
				}
				std::uint64_t d = 0;
				std::uint32_t n = 0;
				if (!ReadResourceArray(a, &d, &n)) {
					return false;
				}
				data     = d;
				resSize  = n;
				arrOff   = a_off;
				arrFound = true;
				return true;
			};
			if (g_floraResArrayOff != 0) {
				tryArr(g_floraResArrayOff);  // 会话内已标定：失败就下面重新找
			}
			if (!arrFound) {
				tryArr(kOffMiscResArray);
			}
			if (!arrFound) {
				for (std::size_t off = kFloraPtrScanLo; off <= kFloraPtrScanHi && !arrFound; off += 8) {
					tryArr(off);
				}
			}
			if (!arrFound) {
				if (a_out) {
					*a_out = out;
				}
				return false;  // 链走到 MISC 就断了 ⇒ shapeOk = false（上层取证）
			}
			out.arrOff  = arrOff;
			out.resSize = resSize;

			// ---- ③ 标定成功 ⇒ 会话级记住（并打一行可核对的日志）----
			if (g_floraProduceOff == 0) {
				g_floraProduceOff = produceOff;
				REX::INFO("flora scanned: 「产出物品」字段标定 = base+0x{:X}（形状校验通过：formType 为 MISC/LVLI"
						  " 且能走到资源数组）",
					produceOff);
			}
			if (g_floraResArrayOff == 0) {
				g_floraResArrayOff = arrOff;
				REX::INFO("flora scanned: 资源数组字段标定 = MISC+0x{:X}（元素 stride 0x{:X}，含 IRES）",
					arrOff, kOffResElemStride);
			}

			// ---- ④ 逐个 IRES 问引擎 ----
			bool hit = false;
			for (std::uint32_t i = 0; i < resSize; ++i) {
				std::uint64_t elem = 0;
				std::uint8_t  ft   = 0;
				std::uint32_t id   = 0;
				if (!ReadU64At(data + i * kOffResElemStride, &elem) || !FormTypeIdOf(elem, &ft, &id)) {
					break;
				}
				if (ft != static_cast<std::uint8_t>(RE::FormType::kIRES)) {
					continue;  // ★ 引擎两处代码都只认 IRES 的元素
				}
				if (out.irESCount < kFloraResMax) {
					out.irES[out.irESCount++] = id;
				}
				// ★★ 这一步才是「问引擎」：0x1597A50 =「这个资源已扫描（进了勘测数据）？」
				if (g_isResourceScannedReady &&
					g_isResourceScanned(reinterpret_cast<RE::TESForm*>(elem))) {
					hit = true;
					break;
				}
			}
			out.anyScanned = hit;
			out.shapeOk    = true;
			if (a_out) {
				*a_out = out;
			}
			return hit;
		}

		// 链走不通时的取证（前几个 base）：把 base 记录 0x180~0x380 里所有「像 form 指针」
		// 的值逐条打出来，外加 base 的 vtable RVA 与 produceChance 交叉校验
		// （水矿 `MineralDepositCommonWaterLiquid01` 的 PFPC = 100/100/100/100 ⇒ 某处应有
		//  `64 64 64 64`）。有了这一行，偏移到底在哪、对象对不对，一眼就能看出来。
		void FloraDumpWindow(const RE::TESForm* a_base)
		{
			if (!a_base) {
				return;
			}
			const auto* raw = reinterpret_cast<const std::uint8_t*>(a_base);
			std::uint64_t vt = 0;
			ReadU64At(reinterpret_cast<std::uint64_t>(a_base), &vt);
			const auto vtblRva = (vt >= ModuleBase()) ? vt - ModuleBase() : vt;

			std::string s;
			for (std::size_t off = kFloraPtrScanLo; off <= kFloraPtrScanHi; off += 8) {
				std::uint64_t p = 0;
				std::uint8_t  ft = 0;
				std::uint32_t id = 0;
				if (!ReadU64At(reinterpret_cast<std::uint64_t>(raw + off), &p) ||
					!FormTypeIdOf(p, &ft, &id)) {
					continue;
				}
				if (s.size() > 700) {
					s += " …";
					break;
				}
				char buf[48];
				std::snprintf(buf, sizeof(buf), "%s+0x%zX:%02X/%08X", s.empty() ? "" : " ", off, ft, id);
				s += buf;
			}
			std::uint32_t pcA = 0;
			std::uint32_t pcB = 0;
			SafeReadMem(raw + 0x298, &pcA, sizeof(pcA));
			SafeReadMem(raw + 0x2A0, &pcB, sizeof(pcB));
			REX::WARN("flora probe (链没走通): base=0x{:X} vtblRva=0x{:X} | 窗口 0x{:X}~0x{:X} 里的 form 指针: {} "
					  "| 0x298=0x{:08X} 0x2A0=0x{:08X}（produceChance 交叉校验：水矿应为 64646464）",
				a_base->GetFormID(), vtblRva, kFloraPtrScanLo, kFloraPtrScanHi,
				s.empty() ? "（一个都没有）" : s, pcA, pcB);
		}

		// ================================================================
		// ★★★ v5.2：引擎扫描进度直读 —— 解析 / 查询 / 缓存
		// （常量与完整反汇编证据链见文件上方 v5.2 段；红线：只调只读函数）
		// ================================================================

		// ① 启动时解析：5 个函数的签名校验 + key 类型常量提取。
		//   任何一项不过 ⇒ g_floraProgressReady 保持 false（新判据整体不生效）。
		void ResolveFloraProgressFunctions()
		{
			g_floraProgressReady = false;
			const auto base = ModuleBase();
			if (!base) {
				REX::WARN("flora progress: module base unavailable");
				return;
			}
			const auto addrKid  = base + kRvaFloraKnowledgeId;
			const auto addrComp = base + kRvaFloraGetComponent;
			const auto addrFind = base + kRvaFloraHashFind;
			const auto addrSub  = base + kRvaFloraSubFind;
			const auto addrSing = base + kRvaFloraSingleton;
			const auto addrK1fb = base + kRvaFloraKey1Fallback;
			if (!SigMatches(addrKid, kSigFloraKnowledgeId) ||
				!SigMatches(addrComp, kSigFloraGetComponent) ||
				!SigMatches(addrFind, kSigFloraHashFind) ||
				!SigMatches(addrSub, kSigFloraSubFind) ||
				!SigMatches(addrSing, kSigFloraSingleton)) {
				REX::WARN("flora progress: 签名不匹配（游戏版本变了？）-> 引擎扫描进度直读 DISABLED"
						  "（植物退回旧判据链）。 kid=+0x{:X} comp=+0x{:X} find=+0x{:X} sub=+0x{:X} sing=+0x{:X}，"
						  "请重新核对 RVA（证据链见本文件 v5.2 常量段）。",
					kRvaFloraKnowledgeId, kRvaFloraGetComponent, kRvaFloraHashFind,
					kRvaFloraSubFind, kRvaFloraSingleton);
				return;
			}
			// ★★★ 订正 R11：key1 兜底函数（0x910690）—— **签名不符只关掉兜底、不禁用整条判据**
			//   （R10 的教训：禁整条判据是危险动作；兜底不可用时组件主路径照常工作）。
			const bool k1fbOk = SigMatches(addrK1fb, kSigFloraKey1Fallback);
			if (!k1fbOk) {
				REX::WARN("flora progress: key1 兜底函数签名不符（+0x{:X}）-> **仅兜底不可用**"
						  "（组件主路径照常；请核对 RVA / 证据链见常量区 kRvaFloraKey1Fallback）。",
					kRvaFloraKey1Fallback);
			}
			// key 高 16 位的「类型 word」：从 0x130A270 的指令里现取（`44 0F B7 05 disp32`），
			// 这样即使 RVA 表变了，只要指令模式没变就读到正确的**地址**。
			// ★★★ 订正 R10：这里只**解析 + 记住地址**，**不读值、也不因值为 0 而禁用** ——
			//   该 word 在 `.data` 未初始化段（运行时才被写），启动时读必为 0；
			//   读值改到每次查询时现读（`QueryFloraScanProgressDirect`），与引擎口径一致。
			const auto   patch = base + kRvaFloraKeyTypePatch;
			std::uint8_t head[8]{};
			if (!SafeReadMem(reinterpret_cast<const void*>(patch), head, sizeof(head)) ||
				std::memcmp(head, kPatFloraKeyType, sizeof(kPatFloraKeyType)) != 0) {
				REX::WARN("flora progress: key 类型提取点签名不符（+0x{:X}）-> 引擎扫描进度直读 DISABLED",
					kRvaFloraKeyTypePatch);
				return;
			}
			std::int32_t disp = 0;
			std::memcpy(&disp, head + 4, 4);
			const auto    immAddr = patch + 8 + static_cast<std::intptr_t>(disp);
			std::uint16_t atLoad  = 0;
			if (!SafeReadMem(reinterpret_cast<const void*>(immAddr), &atLoad, sizeof(atLoad))) {
				REX::WARN("flora progress: key 类型 word 地址不可读（+0x{:X}）-> 引擎扫描进度直读 DISABLED",
					static_cast<std::uintptr_t>(immAddr - base));
				return;
			}
			g_floraKnowledgeId    = reinterpret_cast<FloraKnowledgeId_t>(addrKid);
			g_floraGetComponent   = reinterpret_cast<FloraGetComponent_t>(addrComp);
			g_floraHashFind       = reinterpret_cast<FloraHashFind_t>(addrFind);
			g_floraSubFind        = reinterpret_cast<FloraSubFind_t>(addrSub);
			g_floraSingleton      = reinterpret_cast<FloraSingleton_t>(addrSing);
			g_floraKey1Fallback   = k1fbOk ? reinterpret_cast<FloraKey1Fallback_t>(addrK1fb) : nullptr;
			g_floraKeyTypeWord    = reinterpret_cast<const std::uint16_t*>(immAddr);
			g_floraKeyTypeAtLoad  = atLoad;
			g_floraKeyTypeLast    = atLoad;
			g_floraProgressReady  = true;
			REX::INFO("flora progress ready: kid=+0x{:X} comp=+0x{:X} find=+0x{:X} sub=+0x{:X} "
					  "sing=+0x{:X} k1fb=+0x{:X}{} keyTypeAddr=+0x{:X} keyType@load=0x{:X} (sig verified；"
					  "★ 订正 R10：key 类型 word 改为**每次查询现读** —— 它在 .data 未初始化段、"
					  "启动时必为 0，不再因此禁用判据；★ 订正 R11：组件拿不到时走引擎自己的兜底 "
					  "0x910690 拿 key1（实测植物 ref 没有 0x81 组件 —— 引擎画绿走的也是这条兜底）"
					  ") -> 植物「已扫描」直读引擎扫描进度表",
				kRvaFloraKnowledgeId, kRvaFloraGetComponent, kRvaFloraHashFind, kRvaFloraSubFind,
				kRvaFloraSingleton, kRvaFloraKey1Fallback,
				(k1fbOk ? "" : "（★ 签名不符：仅兜底不可用）"),
				static_cast<std::uintptr_t>(immAddr - base), atLoad);
		}

		// 单次查询的结果（ok=false = 任何一层没走通 ⇒ 上层回退到旧判据，绝不外抛）
		// ★★★ 订正 R10：新增 `stage`（在哪一层停下的）与 `keyType`（本次现读到的类型 word）——
		//   两者**只用于诊断**、不参与判定。`stage` 让「链路不通」一眼定位到具体那一层
		//   （下一局若还有问题，看 `flora progress probe:` 行的 stage 就够了）。
		enum FloraProgStage : std::uint8_t
		{
			kFloraProgOk = 0,         // 链路走通
			kFloraProgNotReady,       // 判据没就绪 / 参数不合法
			kFloraProgNoContainer,    // 组件容器（ref+0xC8）读不到 / 不像指针
			kFloraProgNoComponent,    // 0x81 组件拿不到
			kFloraProgNoKey1,         // key1 == 0（这个引用没有物种 / 资源 ID）
			kFloraProgNoKeyType,      // key 类型 word 现读失败
			kFloraProgNoSingleton,    // TLS 单例 / 知识库 DB 读不到
			kFloraProgMainTableMiss,  // 主表里没有这个物种 / 资源（= 从来没扫过）
			kFloraProgSubEntryOff,    // 桶内偏移 / 二级基址形状不对（★ R12 起不再产生；被 12~15 取代）
			kFloraProgSubTableMiss,   // 二级表里没有这个「知识 ID」的条目
			kFloraProgProgressShape,  // 进度 byte 形状不对（>100）
			kFloraProgException,      // SEH 捕获到异常
			// ★★★ 订正 R12（诊断细化）：R11 两局里 `stage=8` 占了全部 probe，
			//   而 8 混合了三种情况（偏移读不到 / 基址不可读 / 表空或形状坏）——
			//   无法判断「没扫过（空表，正常）」与「结构真的不对（异常）」。
			//   这里拆成四档（**数字只增不改**：8 保留原义，12~15 是新细分）。
			kFloraProgSubOffUnreadable  = 12,  // 桶内 subOff（bucket+index*4+0x12）读不到
			kFloraProgSubBaseUnreadable = 13,  // 二级基址（bucket+subOff）不可读 / 其 cap/arr 字段读不到
			kFloraProgSubEmptyTable     = 14,  // ★ 二级表 cap == 0（= 这个物种在本存档里**没有记录**，多半没扫过）
			kFloraProgSubShapeBad       = 15,  // 二级表形状坏（arr 非指针 / cap 异常大）
		};

		struct FloraProgressResult
		{
			bool          ok       = false;  // 链路走通（拿到 shape 合法的进度值）
			std::uint8_t  progress = 0;      // 0..100；== 100 = 已扫描（引擎口径）
			std::uint32_t key1     = 0;      // 物种 / 资源 ID（0x81 组件 +0x28）
			std::uint32_t key2     = 0;      // 知识 ID（0x1307180(ref)）
			std::uint16_t keyType  = 0;      // ★ 订正 R10：本次现读到的类型 word（诊断）
			bool          keyTypeRead = false;             // 上面那个值是否真的读到了（诊断）
			bool          key1FromFallback = false;        // ★ 订正 R11：key1 来自兜底 0x910690（诊断）
			std::uint8_t  stage    = kFloraProgNotReady;  // ★ 订正 R10：停在哪一层（诊断）
			// ★★★ 订正 R12（诊断）：失败路径也把「已经走到的位置」带回去 ——
			//   R11 的 probe 行在失败时 `key1=0x0 key2=0x0` 是**假的**（失败分支没赋值），
			//   导致「key1 到底拿到没有」只能靠 `k1来源=` 反推。现在照实填。
			std::uint64_t subBucket = 0;  // 主表查到的桶指针（stage>=8 时）
			std::uint16_t subOff    = 0;  // 桶内偏移（stage 12~15 时）
			std::uint64_t subCap    = 0;  // 二级表容量（stage 13~15 时）
			std::uint64_t subArr    = 0;  // 二级表数组指针（stage 13~15 时）
		};

		// ★ 订正 R10：`stage` 的可读名字（只用于日志）
		const char* FloraProgStageName(std::uint8_t a_stage)
		{
			switch (a_stage) {
			case kFloraProgOk:            return "链路走通";
			case kFloraProgNotReady:      return "判据没就绪 / 参数不合法";
			case kFloraProgNoContainer:   return "组件容器（ref+0xC8）读不到";
			case kFloraProgNoComponent:   return "0x81 组件拿不到";
			case kFloraProgNoKey1:        return "key1 == 0（这个引用没有物种 / 资源 ID）";
			case kFloraProgNoKeyType:     return "★ key 类型 word 现读失败";
			case kFloraProgNoSingleton:   return "TLS 单例 / 知识库 DB 读不到";
			case kFloraProgMainTableMiss: return "主表里没有这个物种 / 资源（= 从没扫过）";
			case kFloraProgSubEntryOff:   return "桶内偏移 / 二级基址形状不对（★ R12 起不再产生）";
			case kFloraProgSubTableMiss:  return "二级表里没有这个知识 ID 的条目";
			case kFloraProgProgressShape: return "进度 byte 形状不对（>100）";
			case kFloraProgException:     return "SEH 捕获到异常（指针失效 / 表结构变化）";
			case kFloraProgSubOffUnreadable:  return "桶内 subOff 读不到（+0x12）";
			case kFloraProgSubBaseUnreadable: return "二级基址不可读 / cap·arr 字段读不到";
			case kFloraProgSubEmptyTable:     return "★ 二级表空（cap==0 —— 这个物种在本存档里没有记录，多半没扫过）";
			case kFloraProgSubShapeBad:       return "二级表形状坏（arr 非指针 / cap 异常大）";
			default:                      return "（未知 stage）";
			}
		}

		// ② 单次查询（全 SEH + 逐层形状校验）。只调只读函数：
		//   0x347170（组件，读锁）→ 读 [comp+0x28] → 0x910690（★ R11 兜底，组件缺失时）
		//   → 0x1307180（ref→知识ID）→ 0x23FF640（TLS 单例）→ 0x24105D0（主表）
		//   → 0x23467B0（二级）→ 读 [元素+0x20]。
		//   ★★★ 订正 R10（★ 关键）：key 高 16 位的「类型 word」**每次查询现读** ——
		//     它在 `.data` 未初始化段（运行时才被写入，启动时读必然是 0）；
		//     R9 在启动时读到 0 就把整条判据禁用了（`ready=0 / 问=0`，用户看到
		//     「已扫描的还是青、开关扫描仪也不自愈」）。引擎自己也是每次现读
		//     （`movzx r8d, word [rip+disp]`）⇒ 现读才能和引擎构造的 key 完全一致。
		//   ★★★ 订正 R11（★ 关键）：**组件拿不到时走引擎自己的兜底**（0x910690）拿 key1 ——
		//     实测（用户 2026-09-27 13:10 局）植物 ref 100% 没有 0x81 组件
		//     （`flora progress probe: … stage=3`），而引擎画绿用的就是这条兜底；
		//     R10 只复刻组件主路径 ⇒ 整条判据一次都没跑起来。
		FloraProgressResult QueryFloraScanProgressDirect(const RE::TESObjectREFR* a_ref)
		{
			FloraProgressResult r{};
			if (!g_floraProgressReady || !g_floraKeyTypeWord || !a_ref) {
				return r;
			}
			__try {
				const auto* raw = reinterpret_cast<const std::uint8_t*>(a_ref);
				// ① key1（物种/资源 ID）：**先组件、后兜底**（与引擎 0x7BCBD0 的顺序一致）：
				//   组件路径 = [ref+0xC8] 容器 → 0x347170(容器, 0x81) → [comp+0x28]；
				//   兜底路径 = 0x910690(&key1, ctx16, ref, 0)（组件缺失 / [comp+0x28]==0 时）。
				//   实测：植物 ref 上**没有** 0x81 组件（stage=3）⇒ 兜底是常态路径。
				std::uint32_t key1 = 0;
				{
					std::uint64_t container = 0;
					if (SafeReadMem(raw + kOffRefrComponentContainer, &container, sizeof(container)) &&
						IsPlausiblePointer(container)) {
						void* comp = g_floraGetComponent(reinterpret_cast<void*>(container), kFloraScanCompId);
						if (comp && IsReadable(comp, kOffFloraCompSpecie + 4)) {
							std::uint32_t k = 0;
							if (SafeReadMem(reinterpret_cast<const std::uint8_t*>(comp) + kOffFloraCompSpecie,
									&k, sizeof(k))) {
								key1 = k;
							}
						}
					}
				}
				if (key1 == 0 && g_floraKey1Fallback) {
					// ★ 引擎口径的兜底（只读查询 + 引用计数配平；见常量区 kRvaFloraKey1Fallback）：
					std::uint32_t                    k1f = 0;
					alignas(16) std::uint8_t         ctx[16] = {};
					const std::uint32_t*             out1 =
						g_floraKey1Fallback(&k1f, ctx, const_cast<RE::TESObjectREFR*>(a_ref), nullptr);
					const std::uint32_t k1fVal = out1 ? *out1 : k1f;
					if (k1fVal != 0) {
						key1              = k1fVal;
						r.key1FromFallback = true;
						++g_state.floraProgK1Fallback;
					}
				}
				if (key1 == 0) {
					// 组件与兜底都没拿到 ⇒ 这个引用真的没有物种/资源 ID（引擎也会跳过）
					r.stage = g_floraKey1Fallback ? kFloraProgNoKey1 : kFloraProgNoComponent;
					return r;
				}
				// ①.5 ★★★ 订正 R10：**现读** key 类型 word（与引擎那条指令同一时刻取值）
				std::uint16_t keyType = 0;
				if (!SafeReadMem(reinterpret_cast<const void*>(g_floraKeyTypeWord),
						&keyType, sizeof(keyType))) {
					r.stage = kFloraProgNoKeyType;
					return r;
				}
				r.keyType     = keyType;
				r.keyTypeRead = true;
				g_floraKeyTypeLast = keyType;  // 诊断：最近一次现读到什么
				if (keyType == 0) {
					++g_state.floraProgKeyZero;  // 诊断：现读到 0 的次数（0 本身也可能是合法类型）
				}
				// ★★★ 订正 R12：从这里开始照实回填 —— 即使后面失败，probe 行也能看到
				//   key1（已拿到）/ key2 的真实值（R11 的失败行里它们恒为 0x0，会误导排障）。
				r.key1 = key1;
				// ② key2 = 0x1307180(ref)（纯查询：有 0x2A 组件 ⇒ FormID；否则查表映射）
				const std::uint32_t key2 = g_floraKnowledgeId(const_cast<RE::TESObjectREFR*>(a_ref));
				r.key2 = key2;
				// ③ mgr = [0x23FF640() + 0x8B0]（TLS 单例 + 知识库 DB）
				void*         sing = g_floraSingleton();
				std::uint64_t mgr  = 0;
				if (!sing ||
					!SafeReadMem(reinterpret_cast<const std::uint8_t*>(sing) + kOffFloraMgrDb, &mgr, sizeof(mgr)) ||
					!IsPlausiblePointer(mgr)) {
					r.stage = kFloraProgNoSingleton;
					return r;
				}
				// ④ 主表查找（0x24105D0，只读）；key = (类型 word << 48) | (key1 << 16)
				std::uint64_t       out[4]{};
				const std::uint64_t keyA = (static_cast<std::uint64_t>(keyType) << 48) |
										   (static_cast<std::uint64_t>(key1) << 16);
				g_floraHashFind(reinterpret_cast<void*>(mgr + kOffFloraDbTable), out, &keyA);
				const std::uint64_t bucket = out[kOffFloraLookupBucket / 8];
				const std::uint64_t index  = out[kOffFloraLookupIndex / 8];
				if (index == kFloraLookupSentinel && bucket == 0) {
					r.stage = kFloraProgMainTableMiss;
					return r;  // 表里没有这个物种 / 资源 ⇒ 无进度记录（= 从没扫过）
				}
				if (!IsPlausiblePointer(bucket)) {
					r.stage = kFloraProgMainTableMiss;
					return r;
				}
				r.subBucket = bucket;  // ★ R12 诊断：主表查到的桶
				// ⑤ 二级基址 = bucket + word[bucket + index*4 + 0x12]
				std::uint16_t subOff = 0;
				if (!SafeReadMem(reinterpret_cast<const std::uint8_t*>(bucket) + index * 4 + kOffFloraSubEntryOff,
						&subOff, sizeof(subOff))) {
					r.stage = kFloraProgSubOffUnreadable;  // ★ R12：8 拆成 12（偏移读不到）
					return r;
				}
				r.subOff = subOff;
				const auto* base2 = reinterpret_cast<const std::uint8_t*>(bucket + subOff);
				if (!IsReadable(base2, kOffFloraSubCapacity + 8)) {
					r.stage = kFloraProgSubBaseUnreadable;  // ★ R12：8 拆成 13（基址不可读）
					return r;
				}
				// ⑥ 二级查找（0x23467B0，只读）；返回迭代器（索引），== 容量 ⇒ 未找到
				const std::uint64_t iter =
					g_floraSubFind(const_cast<std::uint8_t*>(base2) + kOffFloraSubTable, &key2);
				std::uint64_t cap = 0;
				std::uint64_t arr = 0;
				if (!SafeReadMem(base2 + kOffFloraSubCapacity, &cap, sizeof(cap)) ||
					!SafeReadMem(base2 + kOffFloraSubArray, &arr, sizeof(arr))) {
					r.stage = kFloraProgSubBaseUnreadable;  // ★ R12：13（字段读不到）
					return r;
				}
				r.subCap = cap;
				r.subArr = arr;
				if (cap == 0) {
					// ★★★ 订正 R12：**空表**单列一档 —— 这就是「这个物种在本存档里
					//   没有记录（多半没扫过）」的正常表现，不该和「形状坏」混在一起
					//   （R11 两局里全部 probe 都停在 stage=8，正是分不清这两种）。
					r.stage = kFloraProgSubEmptyTable;
					return r;
				}
				if (!IsPlausiblePointer(arr) || cap > (1u << 24)) {
					r.stage = kFloraProgSubShapeBad;  // ★ R12：15（形状坏）
					return r;
				}
				if (iter >= cap) {
					r.stage = kFloraProgSubTableMiss;
					return r;  // 这个「知识 ID」在物种记录里没有条目
				}
				// ⑦ 进度 = byte[元素 + 0x20]（元素 stride 0x30）
				const auto* elem = reinterpret_cast<const std::uint8_t*>(arr) + iter * kFloraSubElemStride;
				std::uint8_t prog = 0;
				if (!SafeReadMem(elem + kOffFloraElemProgress, &prog, sizeof(prog)) || prog > 100) {
					r.stage = kFloraProgProgressShape;
					return r;  // 形状不对 ⇒ 不采信
				}
				r.ok       = true;
				r.stage    = kFloraProgOk;
				r.progress = prog;
				r.key1     = key1;
				r.key2     = key2;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				r.ok    = false;  // 指针失效 / 表结构变化等任何异常 ⇒ 当作「查不到」，绝不外抛
				r.stage = kFloraProgException;
			}
			return r;
		}

		// ③ 带缓存：按 **base（物种）** 缓存（同 species 共享 ⇒ 天然「物种级扩散」）。
		//   · 「已扫描」（100）：10 分钟 TTL（读档 / 换场景随判据缓存一起清）；
		//   · 「未满」：FloraUnscannedTtlMs（默认 5 秒 —— 扫到 100 后最多这么久翻绿）；
		//   · 「失败」：10 秒（避免每轮重撞；失败方向永远是「继续走旧判据」）。
		FloraProgressResult QueryFloraScanProgressCached(const RE::TESObjectREFR* a_ref,
			std::uint32_t a_baseFid, std::uint64_t a_now)
		{
			FloraProgressResult r{};
			if (!g_cfg.floraEngineProgress || !g_floraProgressReady || !a_ref || a_baseFid == 0) {
				return r;
			}
			const auto it = g_state.floraProgressByBase.find(a_baseFid);
			if (it != g_state.floraProgressByBase.end()) {
				const auto&         rec = it->second;
				const std::uint64_t ttl = rec.ok
					? (rec.progress == kFloraProgressFull
							? kFloraProgressFullTtlMs
							: (g_cfg.floraUnscannedTtlMs > 0
									? static_cast<std::uint64_t>(g_cfg.floraUnscannedTtlMs)
									: kFloraProgressFailTtlMs))
					: kFloraProgressFailTtlMs;
				if (a_now - rec.atMs < ttl) {
					++g_state.floraProgCacheHits;
					r.ok       = rec.ok;
					r.progress = rec.progress;
					return r;
				}
				g_state.floraProgressByBase.erase(it);
			}
			r = QueryFloraScanProgressDirect(a_ref);
			if (r.ok) {
				++g_state.floraProgQueries;
				if (r.progress == kFloraProgressFull) {
					++g_state.floraProgFull;
				} else {
					++g_state.floraProgPartial;
				}
			} else {
				++g_state.floraProgFails;
			}
			// ★★★ 订正 R10：诊断（只前几条）—— key 类型 word 是**现读**的，第一次拿到
			//   非 0 值 / 首次失败都在这里可见：`stage` 说明停在哪一层、`keyType` 是现读值
			//   （扫描到第几步就断在 `keyType=未读` 之前；见 `QueryFloraScanProgressDirect`）。
			//   ★ 订正 R11：加 `k1来源=`（组件 / 兜底 0x910690）—— 实测植物 ref 没有 0x81
			//   组件，正常工作时这里应大量出现 `k1来源=兜底`。
			//   ★★★ 订正 R12：额度语义升级 —— 除「前 kFloraProgProbeMax 条（默认 8）」外，
			//     **每个 stage 的第一条**与**第一条成功**都保证打出（R11 两局实测：
			//     前 8 条全是同一个 stage=8，成功样例与其它 stage 一条都没留下）。
			//     行数上限 ≈ 8 + 16（stage 位图）+ 1，仍不刷屏。
			{
				const std::uint32_t stage     = r.stage;
				const bool          stageNew  = stage < 32 &&
                                    (g_state.floraProgStageSeen & (1u << stage)) == 0;
				const bool          okFirst   = r.ok && !g_state.floraProgSawOk;
				const bool          baseQuota = g_state.floraProgProbes < kFloraProgProbeMax;
				if (baseQuota || stageNew || okFirst) {
					if (baseQuota) {
						++g_state.floraProgProbes;
					}
					const char* k1src = (r.key1FromFallback ? "兜底(0x910690)" : "组件(0x81)");
					// ★ R12：stage 12~15 时附「桶 / subOff / cap / arr」——直接区分
					//   「空表（没扫过，正常）」与真正的形状异常（一次日志就能定性）。
					char extra[128]{};
					if (stage >= kFloraProgSubOffUnreadable && stage <= kFloraProgSubShapeBad) {
						std::snprintf(extra, sizeof(extra),
							" 桶=0x%zX subOff=0x%X cap=%zu arr=0x%zX",
							static_cast<std::size_t>(r.subBucket), static_cast<unsigned>(r.subOff),
							static_cast<std::size_t>(r.subCap), static_cast<std::size_t>(r.subArr));
					}
					if (r.keyTypeRead) {
						REX::INFO("flora progress probe: base=0x{:X} key1=0x{:X} k1来源={} key2=0x{:X} "
								  "keyType=0x{:X}（现读） percent={} stage={} -> {}{}",
							a_baseFid, r.key1, k1src, r.key2, r.keyType, r.progress,
							static_cast<std::uint32_t>(stage),
							r.ok ? (r.progress == kFloraProgressFull ? "★ 已扫描（引擎进度 100）"
																	 : "查到但**未满**（继续走旧判据）")
								 : FloraProgStageName(r.stage),
							extra);
					} else {
						REX::INFO("flora progress probe: base=0x{:X} key1=0x{:X} k1来源={} key2=0x{:X} "
								  "keyType=（未读到） stage={} -> {}{}",
							a_baseFid, r.key1, k1src, r.key2, static_cast<std::uint32_t>(stage),
							FloraProgStageName(r.stage), extra);
					}
				}
				if (stage < 32) {
					g_state.floraProgStageSeen |= (1u << stage);
				}
				if (r.ok) {
					g_state.floraProgSawOk = true;
				}
			}
			if (g_state.floraProgressByBase.size() >= kFloraProgressCacheMax) {
				g_state.floraProgressByBase.clear();  // 兜异常增长（正常情况下只有几十条）
			}
			g_state.floraProgressByBase.emplace(a_baseFid, State::FloraProgressRec{ r.progress, a_now, r.ok });
			return r;
		}

		// ④ 缓存作废（读档 / 换场景 / 放下扫描仪时随判据缓存一起调 —— 见 InvalidateFloraScannedCache）。
		void InvalidateFloraProgressCache(const char* a_reason)
		{
			if (g_state.floraProgressByBase.empty()) {
				return;
			}
			const auto n = g_state.floraProgressByBase.size();
			g_state.floraProgressByBase.clear();
			REX::INFO("flora progress: 进度缓存作废（{}，清了 {} 条 base）-> 下一轮重新问引擎", a_reason, n);
		}

		// ★★★ v4.33 / v5.1 / v5.1.7：学习表落盘（定义在下面；这里先声明，因为
		//   `RememberFloraRef` 的「绿」分支也要落盘）。★ v5.1 起一行 = 两个 FormID；
		//   ★ v5.1.7 起追加第 3 个字段 = 见证时刻（游戏时间，天）。
		void AppendFloraLearnRecord(std::uint32_t a_refFid, std::uint32_t a_baseFid, float a_days);

		// ================================================================
		// ★★★ v5.1：**只读**读一眼「引擎给这个引用写的 outline 状态」
		// ----------------------------------------------------------------
		// 条件与节点布局的完整证据见常量区 `kRvaOutlineStateTree` 那段。
		// 返回 nullptr = 引擎的状态表里**没有这个引用**（= 引擎从没给它写过状态，
		// 没举过扫描仪时这是最常见的情况）；返回非空 = 引擎写过的状态 dword
		// （4/5 = 绿、7/8 = 青、0..3/6/9..12 = 别的）。
		// 安全性：树头 / 每个节点都先过 IsReadable（带已验证区间缓存，命中时零 syscall），
		//   深度上限 kOutlineStateTreeDepthMax（远大于红黑树实际深度）。
		// ★ 与 0x17D5BE0（LookupOrAdd）的区别只有一个、但至关重要：**绝不插入**。
		// ================================================================
		const std::uint32_t* LookupOutlineStateReadOnly(const RE::TESObjectREFR* a_ref)
		{
			const auto base = ModuleBase();
			if (!base || !a_ref || !g_state.nativeReady) {
				return nullptr;
			}
			const auto* hdr = reinterpret_cast<const std::uint8_t*>(base + kRvaOutlineStateTree);
			if (!IsReadable(hdr, 0x10)) {
				return nullptr;
			}
			const auto sentinel = *reinterpret_cast<const std::uintptr_t*>(hdr);
			if (!sentinel ||
				!IsReadable(reinterpret_cast<const void*>(sentinel), kOffStateTreeSentinelRoot + 8)) {
				return nullptr;
			}
			// 哨兵 +0x08 = 根（MSVC _Tree：_Myhead->_Parent）；空树的根就是哨兵本身。
			auto       node = *reinterpret_cast<const std::uintptr_t*>(sentinel + kOffStateTreeSentinelRoot);
			const auto key  = reinterpret_cast<std::uintptr_t>(a_ref);
			for (std::uint32_t depth = 0; node && depth < kOutlineStateTreeDepthMax; ++depth) {
				const auto* n = reinterpret_cast<const std::uint8_t*>(node);
				if (!IsReadable(n, kOffStateTreeNodeSize)) {
					return nullptr;
				}
				if (n[kOffStateTreeNodeIsNil] != 0) {
					return nullptr;  // 走到哨兵 = 树里没有这个键
				}
				const auto nodeKey = *reinterpret_cast<const std::uintptr_t*>(n + kOffStateTreeNodeKey);
				if (nodeKey == key) {
					const auto* v = n + kOffStateTreeNodeValue;
					return IsReadable(v, sizeof(std::uint32_t))
					         ? reinterpret_cast<const std::uint32_t*>(v)
					         : nullptr;
				}
				node = *reinterpret_cast<const std::uintptr_t*>(
					n + ((nodeKey < key) ? kOffStateTreeNodeRight : kOffStateTreeNodeLeft));
			}
			return nullptr;
		}

		// 引擎「引用 → 状态」表当前的条目数（树头 +0x08；★ v5.1 诊断）。
		//   用途：**长时间游玩它应该基本稳定**。持续单调增长 = 有代码在往里面插条目
		//   （老版本就是本 MOD 自己插的）；v5.1 起本 MOD 一个都不插。
		std::uint64_t OutlineStateTableCount()
		{
			const auto base = ModuleBase();
			if (!base) {
				return 0;
			}
			const auto* hdr = reinterpret_cast<const std::uint8_t*>(base + kRvaOutlineStateTree);
			if (!IsReadable(hdr, 0x10)) {
				return 0;
			}
			return *reinterpret_cast<const std::uint64_t*>(hdr + kOffStateTreeCount);
		}

		// 按引用记忆的容量保护（兜异常增长）：先丢「只见过青」的条目
		//   （它们只省一次引擎调用，丢了完全无害），仍满才整体清空。
		constexpr std::size_t kFloraRefKnowMax = 8192;
		void EnsureFloraRefKnowRoom()
		{
			if (g_state.floraRefKnow.size() < kFloraRefKnowMax) {
				return;
			}
			std::size_t dropped = 0;
			for (auto it = g_state.floraRefKnow.begin(); it != g_state.floraRefKnow.end();) {
				if (!it->second.green) {
					it = g_state.floraRefKnow.erase(it);
					++dropped;
				} else {
					++it;
				}
			}
			if (dropped > 0) {
				REX::INFO("flora learn: 按引用记忆达上限 {}，淘汰了 {} 条「青」条目（「绿」条目全部保留）",
					kFloraRefKnowMax, dropped);
			}
			if (g_state.floraRefKnow.size() >= kFloraRefKnowMax) {
				REX::WARN("flora learn: 「绿」条目也达上限 {}（异常）—— 整体清空重学", kFloraRefKnowMax);
				g_state.floraRefKnow.clear();
			}
		}

		// 按物种（base）扩散表的容量兜底：物种 / 资源总量正常只有几十~几百条。
		//   真到上限说明判据异常 ⇒ 整体清空重学（不丢正确性：旧条目的引用级
		//   记忆还在，下轮遇到时会**动态激活**重建）。
		constexpr std::size_t kFloraBaseKnowMax = 4096;

		// （`FloraBaseScope` 定义在 State 之前 —— State 里有它的容器：见文件上方。）

		// `cell 指针 → worldspace 指针` 的缓存（同一 cell 的成千上万次询问 = 一次引擎调用）。
		//   换场景 / 读档时清空（cell 可能被释放 / 重排）。
		constexpr std::size_t kFloraCellWsCacheMax = 256;

		// ★ v5.1.5：这个引用所在的 worldspace（星球 / 内景；读不到 ⇒ 0 = 未知）。
		//   用引擎自己的 `TESObjectREFR::GetParentWorldSpace()`；按 cell 指针缓存。
		std::uintptr_t WorldspaceOfRef(const RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return 0;
			}
			const auto* cell = a_ref->parentCell;
			if (!cell) {
				return 0;
			}
			const auto cellKey = reinterpret_cast<std::uintptr_t>(cell);
			if (const auto it = g_state.floraCellWsCache.find(cellKey); it != g_state.floraCellWsCache.end()) {
				return it->second;
			}
			std::uintptr_t ws = 0;
			if (auto* w = const_cast<RE::TESObjectREFR*>(a_ref)->GetParentWorldSpace()) {
				ws = reinterpret_cast<std::uintptr_t>(w);
			}
			if (g_state.floraCellWsCache.size() >= kFloraCellWsCacheMax) {
				g_state.floraCellWsCache.clear();
			}
			g_state.floraCellWsCache.emplace(cellKey, ws);
			return ws;
		}

		// ★★★ v5.1.6（订正 R6）：**玩家当前所在的世界空间**（星球 / 内景）。
		//   读档复核时用它做「这个 base 的作用域跟当前上下文是不是同一处」的过滤
		//   （见 ScopeSkipBaseInThisWorldspace）。读不到 ⇒ 0（= 不收紧）。
		std::uintptr_t CurrentWorldspace()
		{
			return WorldspaceOfRef(RE::PlayerCharacter::GetSingleton());
		}

		// ★★★ v5.1.2（订正 R2）：把「这个 **base（物种 / 资源）** 已扫描」记进会话表。
		//   权威来源（调用点）：`RememberFloraRef(绿)` 成功时、⓪/⓪.5 的引用级绿记忆
		//   命中时（= 跨会话**动态激活**）。返回 true = 本次首次记下。
		//   ★ 依据：引擎知识库是 species（资源）级的 —— 引擎自己的行为就是「学过一个
		//     实例 ⇒ 同 species 的全部实例画绿」（v5.1 日志实证：举一下扫描仪，
		//     同 base 的一串引用被逐个画成 4/5）。所以「权威确认过任一个体」⇒
		//     「同 base 全绿」不是外推，而是**把引擎的行为补全**。
		//   ★★★ v5.1.5（订正 R5）：扩散**限制在同一颗星球**（见 FloraBaseScope）——
		//     跨星球外溢正是用户这一轮报的「未扫描星球资源直接显示绿色」。
		bool RememberFloraBase(std::uint32_t a_baseFid, std::uintptr_t a_ws, const char* a_reason)
		{
			// ★★★ v5.2：记忆层总开关（默认 0 = 抛弃记忆）—— 不写物种表。
			//   「同 species 全绿」现在由 ⓠ 的引擎进度数据（按 base 缓存）保证。
			if (!g_cfg.floraUseMemory) {
				return false;
			}
			if (!a_baseFid || a_baseFid == 0xFFFFFF) {
				return false;
			}
			if (g_state.floraBaseKnow.size() >= kFloraBaseKnowMax) {
				REX::WARN("flora learn: 按物种表达上限 {}（异常）—— 整体清空重学（会随引用级记忆动态重建）",
					kFloraBaseKnowMax);
				g_state.floraBaseKnow.clear();
			}
			const auto it = g_state.floraBaseKnow.find(a_baseFid);
			if (it == g_state.floraBaseKnow.end()) {
				FloraBaseScope s{};
				s.days = CurrentGameDays();  // ★ v5.1.7：见证时刻（读档剪枝用）
				if (a_ws) {
					s.worldspaces[0] = a_ws;
					s.count          = 1;
				}
				g_state.floraBaseKnow.emplace(a_baseFid, s);
				++g_state.floraBaseNew;
				const auto n = g_state.floraBaseNew;
				if (n <= 24 || n % 64 == 0) {
					REX::INFO("flora scan: 按物种记下「已扫描」—— base=0x{:X}（{}）；"
							  "★ v5.1.2：同 species / 同资源的**所有**实例一起变绿；"
							  "★ v5.1.5：**限制在同一颗星球**（记录 worldspace=0x{:X}；"
							  "0 = 读不到 ⇒ 不收紧；不落盘 ⇒ 不会外溢到别的存档）",
						a_baseFid, a_reason, a_ws);
				}
				return true;
			}
			// 已在表里：只补作用域（同一物种在别的星球也被见证 ⇒ 那颗星球也放行）
			auto& e = it->second;
			// ★ v5.1.7：保留**最早**一次见证的游戏时间（最能证明「这个绿在本存档的过去」）
			const float d = CurrentGameDays();
			if (d > 0.0f && (e.days <= 0.0f || d < e.days)) {
				e.days = d;
			}
			if (a_ws && !e.Seen(a_ws) && e.count < e.worldspaces.size()) {
				e.worldspaces[e.count++] = a_ws;
			}
			return false;
		}

		// 查「这个物种 / 资源」是不是已被权威确认过（会话级；见上面长注释）。
		//   ★★★ v5.1.8（订正 R8）：**默认不再按星球硬拒绝**（回引擎口径）——
		//     用户 2026-09-27 10:50 那一局实证：在星球 A 扫过的物种（本会话当场见证），
		//     星图快速旅行到 B 之后，**引擎自己**（一举扫描仪）把同 species 的实例
		//     画成绿 4/5 ⇒ 引擎的物种知识**跨星球生效**；而我们按「不在同一颗星球」
		//     把它们挡成青色 = 用户第三次报的「传送后已扫描物品变青」。
		//     ⇒ 物种表本来就有**存档作用域**（会话级、不落盘、读档时按锚点剪枝 +
		//     落盘条目按见证时刻播种），跨星球扩散不会外溢到别的存档；
		//     `FloraSpeciesPlanetScope=1` 仍保留 R5 的严格口径（只为对照 / 回退）。
		//     `a_ws == 0`（当前引用读不到 worldspace）⇒ 不收紧（保守）。
		bool FloraBaseKnown(std::uint32_t a_baseFid, std::uintptr_t a_ws)
		{
			if (!a_baseFid) {
				return false;
			}
			const auto it = g_state.floraBaseKnow.find(a_baseFid);
			if (it == g_state.floraBaseKnow.end()) {
				return false;
			}
			const auto& e = it->second;
			if (!g_cfg.floraSpeciesPlanetScope) {
				if (e.count != 0 && a_ws != 0 && !e.Seen(a_ws)) {
					++g_state.floraBaseCrossPlanet;  // ★ v5.1.8：这次扩散跨了星球（旧口径会拒绝）
				}
				return true;  // ★ 默认口径 = 引擎口径（物种知识跨星球；作用域由存档边界管）
			}
			if (e.count == 0 || a_ws == 0) {
				return true;  // 拿不到作用域 / 当前世界空间不知道 ⇒ 不收紧
			}
			if (e.Seen(a_ws)) {
				return true;
			}
			++g_state.floraBasePlanetDenied;
			return false;
		}

		// ================================================================
		// ★★★ v5.1.7（订正 R7）：**存档时间指纹** —— 「这个存档是哪个时间线」的硬证据
		// ----------------------------------------------------------------
		// 为什么需要（完整证据链见 docs/40）：v5.1.6 把「资源链复核有证伪」当作
		//   「读回了更早的存档」的证据 —— 但**载入刚结束时资源链的否定答案是错的**：
		//   用户 2026-09-27 10:15 那一局，载入画面关闭 + 4s 的复核把 9 个 base 全判成
		//   「本存档没扫描」并删掉 132 条记忆（= 用户看到的「已扫描物品变青」）；
		//   而 10s 后引擎自己（拿着扫描仪）把**同一批** base 画成绿（state 4/5）、
		//   80s 后资源链对其中一个（base=0x270033）也答「已扫描」——
		//   **同一个函数、同一个资源，80 秒后答案相反**。
		//   ⇒ 「没扫描」这个否定答案在载入刚结束的那段时间**没有区分度**
		//     （链路上下文没就绪时同样答「没扫描」），不能拿它做不可逆的作废。
		//
		// 新口径 = **游戏时间**（`Calendar::gameDaysPassed`；随存档一起保存 / 读回）：
		//   · 每条记忆自带「学习时刻」（落盘行第 3 字段；旧格式行 = 0 = 未知）；
		//   · 读档时把「本存档的游戏时间」记成**锚点**（`floraSaveDaysFloor`）：
		//       学习时刻 ≤ 锚点 + eps ⇒ 这个扫描发生在本存档的过去 ⇒ 记忆有效；
		//       学习时刻 >  锚点 + eps ⇒ 属于更新的时间线 ⇒ 不在作用域（**不删**）；
		//   · 传送 / 继续同一存档 ⇒ 锚点几乎不动 ⇒ 什么都不作废（本次修复的核心）。
		//
		// 离线核对（本机 1.16.244.0，docs/40 §三）：
		//   Calendar 单例 = REL::ID 937673 -> RVA 0x5FDCDF8（`Calendar**`）；
		//   字段 gameDaysPassed @ Calendar+0x30、TESGlobal::value @ +0x48 —— 两处交叉验证：
		//     · 0x5A73EC: mov rcx,[单例] / mov rax,[rcx+0x30] / vmovss xmm0,[rax+0x48]
		//     · 0x9519FC: mov rax,[单例] / mov rcx,[rax+0x10]（gameYear）/ vcvttss2si r8,[rcx+0x48]
		//       （且 year 读不到时的兜底常量 0x4D = 77 与 commonlibsf `Calendar::GetYear()`
		//        的默认值完全一致 —— 布局对上了）
		//   运行时再叠一层形状校验：单例非空 → 全局非空可读 → formType == kGLOB → 值有限且 > 0。
		// ================================================================
		constexpr float kFloraSaveTimeEpsDays = 0.005f;  // ≈ 7.2 游戏分钟（时钟量化 + 处理延迟余量）

		bool ReadGameDays(float* a_out)
		{
			*a_out = 0.0f;
			// 直接走 `REL::Relocation` 而不是 `Calendar::GetSingleton()`：先核对
			//   「地址库解析出来的地址非空」再去取单例指针（万一 ID 解析失败，
			//   commonlibsf 的写法会直接解引用一个空地址）。
			static ::REL::Relocation<RE::Calendar**> calVar{ RE::ID::Calendar::Singleton };
			if (!calVar.address()) {
				return false;
			}
			auto* cal = *calVar;
			if (!cal || !IsReadable(cal, 0x40)) {
				return false;
			}
			auto* glob = cal->gameDaysPassed;
			if (!glob || !IsReadable(glob, 0x50)) {
				return false;
			}
			if (reinterpret_cast<const std::uint8_t*>(glob)[kOffFormType] !=
				static_cast<std::uint8_t>(RE::FormType::kGLOB)) {
				return false;
			}
			const float v = *reinterpret_cast<const float*>(
				reinterpret_cast<const std::uint8_t*>(glob) + 0x48);
			if (!(v > 0.0f) || !std::isfinite(v)) {
				return false;
			}
			*a_out = v;
			return true;
		}

		// 见证时刻（游戏时间，天；读不到 ⇒ 0 = 未知）。调用点只在「写新结论」时（很少），
		//   再叠一层 1 秒缓存兜住同一帧里的多次写。
		float CurrentGameDays()
		{
			const auto now = NowMs();
			if (g_state.floraDaysCacheAtMs && now - g_state.floraDaysCacheAtMs < 1000) {
				return g_state.floraDaysCache;
			}
			float d = 0.0f;
			if (!ReadGameDays(&d)) {
				d = 0.0f;
			}
			g_state.floraDaysCacheAtMs = now ? now : 1;
			g_state.floraDaysCache     = d;
			return d;
		}

		// 诊断（**只用于日志**）：最近一次排队载入的存档名 + 当前存档编号。
		//   偏移取自 commonlibsf `RE/B/BGSSaveLoad.h`（BGSSaveLoadManager：Singleton = REL::ID 883588、
		//   currentSaveGameNumber @ +0x38、queuedEntryToLoad @ +0x58；BGSSaveLoadFileEntry::fileName @ +0x00）。
		//   ★ 全部走 SafeReadMem（SEH 直读）+ 可打印字符校验 ⇒ 偏移万一不对，最坏也只是
		//     日志里少一段信息，绝不参与任何判定、绝不崩。
		void ReadSaveNameDiag()
		{
			g_state.floraFpSaveName[0] = '\0';
			g_state.floraFpSaveNo      = 0;
			static ::REL::Relocation<void**> mgrVar{ RE::ID::BGSSaveLoadManager::Singleton };
			std::uintptr_t                   mgr = 0;
			if (!mgrVar.address() ||
				!SafeReadMem(reinterpret_cast<const void*>(mgrVar.address()), &mgr, sizeof(mgr)) || !mgr) {
				return;
			}
			std::uint32_t no = 0;
			if (SafeReadMem(reinterpret_cast<const void*>(mgr + 0x38), &no, sizeof(no)) && no < 10000000u) {
				g_state.floraFpSaveNo = no;
			}
			std::uintptr_t entry = 0;
			if (!SafeReadMem(reinterpret_cast<const void*>(mgr + 0x58), &entry, sizeof(entry)) || !entry) {
				return;
			}
			std::uintptr_t namePtr = 0;
			if (!SafeReadMem(reinterpret_cast<const void*>(entry), &namePtr, sizeof(namePtr)) || !namePtr) {
				return;
			}
			char buf[40]{};
			if (!SafeReadMem(reinterpret_cast<const void*>(namePtr), buf, sizeof(buf) - 1)) {
				return;
			}
			buf[sizeof(buf) - 1] = '\0';
			std::size_t n = 0;
			for (; n + 1 < sizeof(g_state.floraFpSaveName) && buf[n]; ++n) {
				const auto c = static_cast<unsigned char>(buf[n]);
				if (c < 0x20 || c > 0x7E) {
					break;  // 非可打印 ⇒ 不像文件名，丢弃
				}
				g_state.floraFpSaveName[n] = static_cast<char>(c);
			}
			g_state.floraFpSaveName[n] = '\0';
		}

		// ★★★ v5.1.5（订正 R5）/ v5.1.7（订正 R7）：这条记忆**还在「本存档的作用域」里**吗？
		//   为什么需要它：用户实测「未扫描星球资源直接显示绿色，重新读档（未扫描时的存档）
		//   后问题非常严重」—— 「已扫描」是**存档里勘测数据**的事实，而记忆（含落盘文件）
		//   不区分存档 ⇒ 在「后玩的存档」里学到的绿会漏到「更早的存档」里。
		//   ★★★ v5.1.7（订正 R7）**新口径 = 存档时间指纹**（默认；见上面长注释 / docs/40）：
		//     · `FloraMemoryScope=0`（旧行为，对照用）⇒ 永远算数；
		//     · 本时间线内当场见证（epoch == 当前）⇒ 永远算数（不受锚点限制）；
		//     · 其余条目按**学习时刻**判定：`days ≤ 锚点 + eps` ⇒ 算数；
		//       没有时间信息（旧格式行）⇒ 用 `floraLegacyStampDays`（首次读档的锚）；
		//       连锚都没有（还没处理过读档）⇒ 不收紧（保守）。
		//   ★ 旧口径（v5.1.5 的 epoch；`FloraSaveFingerprint=0` 或读不到时间时回退）：
		//     · 条目 epoch != 0 ⇒ **只有当前读档编号相同**才算数（= 本存档内当场见证）；
		//     · 条目 epoch == 0（本会话启动时从落盘文件读回，属于**上一个游戏会话**）⇒
		//       `FloraMemoryScope=1`（默认）只在本会话**第一次读档**时算数
		//       （= 重开游戏继续玩同一个存档：扫过的目标不用再开一遍扫描仪）；
		//       之后的每次读档都不算数（那时玩家可能读了更早的存档）。
		bool FloraEntryInScope(const State::FloraRefKnow& a_e)
		{
			if (g_cfg.floraMemoryScope == 0) {
				return true;
			}
			if (g_cfg.floraSaveFingerprint && g_cfg.floraMemoryScope == 1 &&
				g_state.floraSaveDaysFloor >= 0.0f) {
				if (a_e.epoch != 0 && a_e.epoch == g_state.floraSaveEpoch) {
					return true;  // ★ 本时间线内当场见证 ⇒ 永远有效
				}
				float d = a_e.days;
				if (!(d > 0.0f)) {
					d = g_state.floraLegacyStampDays;  // 旧格式行 / 读不到时间 ⇒ 用首次读档的锚
				}
				if (!(d > 0.0f)) {
					return true;  // 实在没有时间信息 ⇒ 不收紧（保守）
				}
				return d <= g_state.floraSaveDaysFloor + kFloraSaveTimeEpsDays;
			}
			if (a_e.epoch != 0) {
				return a_e.epoch == g_state.floraSaveEpoch;
			}
			return g_cfg.floraMemoryScope == 1 && g_state.floraSaveEpoch <= 1;
		}

		// ================================================================
		// ★★★ v5.1.8（订正 R8）：用落盘记忆**播种**按物种扩散表
		// ----------------------------------------------------------------
		// 为什么需要（用户 2026-09-27 10:48 那一局日志实证）：
		//   外景里的星球目标引用是**运行时临时引用**（FormID `0xFF……`），
		//   同一个物种的实例在**新会话 / 换场景后换了 FormID**（不是「同一个引用」）⇒
		//   按引用的落盘记忆**命中不了**（那一局载入后 `记忆: 命中=2`，
		//   271 个本已扫描过的植物全在青色）——而引擎自己一举扫描仪就把它们画成绿
		//   ⇒ 那些绿是**本存档的事实**，我们只是「忘了」。
		//   ⇒ 只靠引用级记忆，「继续同一存档 / 传送」必然先青后绿（用户三次报的现象）。
		// 做法：把落盘记忆里**仍在作用域内**的绿条目按 **base（物种）**播种进物种表 ——
		//   之后同 species 的实例（无论 FormID 是否变过）直接走 ⓪.2 ⇒ 一开始就是绿的，
		//   不必等玩家举一次扫描仪。
		//   ③ 播种只**新建**条目（不覆盖已有见证；已有条目只把 `days` 往早收）；
		//   ④ `days` 沿用条目自己的值（0 = 旧格式 ⇒ 读档剪枝时用 legacy 锚，与条目同口径）；
		//   ⑤ 不落盘（物种表本来就是会话级表；落盘记忆才是它的来源）。
		// 安全性：只播种 `FloraEntryInScope()` 判为**本存档作用域内**的条目 ——
		//   读回更早的存档时，属于「更新的时间线」的条目不算数（R5/R7 的规则不变）。
		std::size_t SeedFloraBaseFromRefMemory(const char* a_reason)
		{
			// ★★★ v5.2：记忆层总开关（默认 0 = 抛弃记忆）—— 不播种。
			//   用户报的「所有星球资源固定显示已扫描绿」的主嫌就是
			//   「R8 跨星球放行 + 落盘播种」把物种表大面积填充；新口径下这整条路关闭。
			if (!g_cfg.floraUseMemory) {
				return 0;
			}
			if (!g_cfg.floraLearnPersist || g_cfg.floraMemoryScope != 1) {
				return 0;  // 最严格口径（2）/ 旧行为（0）不播种
			}
			std::size_t seeded = 0;
			std::size_t early  = 0;  // 已有条目被「往早收」的个数（诊断）
			for (const auto& kv : g_state.floraRefKnow) {
				const auto& e = kv.second;
				if (!e.green || !e.baseFid || e.baseFid == 0xFFFFFF) {
					continue;
				}
				if (!FloraEntryInScope(e)) {
					continue;
				}
				const auto it = g_state.floraBaseKnow.find(e.baseFid);
				if (it == g_state.floraBaseKnow.end()) {
					if (g_state.floraBaseKnow.size() >= kFloraBaseKnowMax) {
						REX::WARN("flora learn: 播种物种表时达上限 {}（异常）—— 停止播种", kFloraBaseKnowMax);
						break;
					}
					FloraBaseScope s{};
					s.days = e.days;  // 0 = 旧格式（剪枝时用 legacy 锚）
					g_state.floraBaseKnow.emplace(e.baseFid, s);
					++seeded;
					++g_state.floraBaseSeeded;
				} else if (e.days > 0.0f &&
						   (it->second.days <= 0.0f || e.days < it->second.days)) {
					it->second.days = e.days;  // 保留**最早**见证时刻（与 RememberFloraBase 一致）
					++early;
				}
			}
			if (seeded > 0 || early > 0) {
				REX::INFO("flora learn: 按物种扩散表**播种** {} 个 base（{}；落盘记忆里共 {} 条，"
						  "作用域内才会播种）⇒ 同 species 的实例**不必等举扫描仪**就是绿的"
						  "（★ v5.1.8：引用会换 FormID，物种级才稳）",
					seeded, a_reason, g_state.floraRefKnow.size());
			}
			return seeded;
		}

		// ★★★ v5.1：把「这个**引用**是否已扫描」写进按引用的单向记忆。
		//   三个**权威**来源都走这里：引擎状态 == 2、引擎亲手画过 4/5、资源链命中。
		//   返回 true = 本次**首次**写入（调用方可据此打一条日志 / 落盘）。
		//   规则（与 v4.31 的「单向」一致，只是粒度收窄到引用）：
		//     · 「绿」不会被「青」翻案（这就是「低概率变青」的根治）；
		//     · ★★★ v5.1.5：「单向」与「作用域」绑定 —— 引擎在**本存档里**当场画出
		//       「青」（7/8）时，可以把一条**过期**（不在作用域内）的绿翻案掉
		//       （那正是「读档后假绿」：引擎已经明确说这个存档里它没被扫描）；
		//       作用域内的绿仍然单向（「低概率变青」不回归）。
		//     · 同一个 FormID 换了 base（运行时引用的 FormID 会被回收）⇒ 当新条目；
		//     · 「绿」条目落盘（`SAS_AlwaysScan.flora-learn.txt`，跨会话保留）。
		bool RememberFloraRef(const RE::TESObjectREFR* a_ref, const RE::TESForm* a_base,
			bool a_green, const char* a_reason)
		{
			// ★★★ v5.2：记忆层总开关（默认 0 = 抛弃记忆）—— 不学、不落盘。
			if (!g_cfg.floraUseMemory) {
				return false;
			}
			if (!a_ref || !a_base) {
				return false;
			}
			const std::uint32_t refFid = a_ref->GetFormID();
			if (refFid == 0 || refFid == 0xFFFFFF) {
				return false;
			}
			const std::uint32_t baseFid = a_base->GetFormID();
			auto&               e       = g_state.floraRefKnow[refFid];
			if (e.baseFid != baseFid) {
				e = State::FloraRefKnow{};  // FormID 被回收去装别的东西 ⇒ 当新条目
			}
			if (e.baseFid == baseFid) {
				const bool inScope = FloraEntryInScope(e);
				if (e.green == a_green) {
					// ★ v5.1.5：结论相同但**已经过期** ⇒ 只把作用域盖成当前这一档
					//   （= 本存档内重新见证；不重复落盘、不重复计数）。
					if (!inScope) {
						e.epoch = g_state.floraSaveEpoch;
						++g_state.floraScopeRescoped;
					}
					return false;  // 已有同样的结论
				}
				if (e.green && !a_green) {
					if (inScope) {
						return false;  // ★ 单向：**本存档内**确认过「已扫描」的不被「未扫描」翻案
					}
					// ★ v5.1.5：过期的绿 ⇒ 允许被引擎**当场**画的青翻案（见上面长注释）
					++g_state.floraScopeDemoted;
				}
			}
			e.baseFid = baseFid;
			e.green   = a_green;
			e.epoch   = g_state.floraSaveEpoch;  // ★ v5.1.5：盖「见证于第几次读档之后」
			const float seenDays = CurrentGameDays();  // ★ v5.1.7：见证时刻（游戏时间，天）
			e.days = seenDays;
			EnsureFloraRefKnowRoom();
			if (a_green) {
				++g_state.floraRefGreenNew;
				AppendFloraLearnRecord(refFid, baseFid, seenDays);  // ★ 落盘（跨会话保留）
				// ★★★ v5.1.2（订正 R2）：顺手把 base 记进会话级「物种表」——
				//   同 base 的其它实例（本轮引擎没画到的那些）靠它一起变绿。
				//   ★ v5.1.5：物种表每次读档都被清空（只由「本存档内见证」重建），
				//     而且**带见证时的 worldspace**（同 species 只在**同一颗星球**上扩散）。
				RememberFloraBase(baseFid, WorldspaceOfRef(a_ref), a_reason);
			} else {
				++g_state.floraRefCyanNew;
			}
			const auto n = g_state.floraRefGreenNew + g_state.floraRefCyanNew;
			if (n <= 24 || n % 64 == 0) {
				REX::INFO("flora scan: 按引用记下 {} —— ref=0x{:X} base=0x{:X}（{}）"
						  "（★ v5.1：记忆粒度 = 引用；★ v5.1.5：结论带读档作用域 epoch={}；"
						  "★ v5.1.7：带见证时刻 = {} 天）",
					a_green ? "「已扫描」（绿）" : "「未扫描」（青）", refFid, baseFid, a_reason,
					g_state.floraSaveEpoch, seenDays);
			}
			return true;
		}

		// 查「这个引用」的记忆（base 必须对得上）。命中 ⇒ 调用方不必再问引擎。
		//   ★★★ v5.1.5：**不在本存档作用域内的条目一律不算命中**（否则旧存档的绿会
		//     漏到刚读进来的档里 —— 用户实测的假绿就是这么来的）。
		bool FloraRefKnown(const RE::TESObjectREFR* a_ref, std::uint32_t a_baseFid, bool* a_outGreen)
		{
			// ★★★ v5.2：记忆层总开关（默认 0 = 抛弃记忆）—— 记忆表不参与判定。
			if (!g_cfg.floraUseMemory) {
				return false;
			}
			if (!a_ref || !a_baseFid) {
				return false;
			}
			const std::uint32_t refFid = a_ref->GetFormID();
			if (!refFid) {
				return false;
			}
			const auto it = g_state.floraRefKnow.find(refFid);
			if (it == g_state.floraRefKnow.end() || it->second.baseFid != a_baseFid) {
				return false;
			}
			if (!FloraEntryInScope(it->second)) {
				++g_state.floraScopeSkipped;
				return false;
			}
			if (a_outGreen) {
				*a_outGreen = it->second.green;
			}
			return true;
		}

		// ★★★ v4.26 / v5.1：读一眼**引擎自己给这个引用写的 outline 状态**并学下来。
		//   状态表 = 引擎的「引用 → 状态」红黑树；引擎扫描时会把「已扫描的星球目标」
		//   写成 **4/5**（原生绿）、「未扫描的」写成 **7/8**（青色）—— 见常量区反汇编。
		//   ⇒ 不猜偏移、不碰资源链，直接看引擎画了什么；「已扫描」是**单向**的
		//     （勘测数据不会退回），所以学到的「绿」一直有效。
		//
		//   ★★★ v4.33：调用点从「只在举着扫描仪（让位）时」扩展到**每次判据未命中
		//     也会读一眼**（见 FloraTargetScanned ⓪.5）—— 放下扫描仪后引擎留在
		//     表里的 4/5 也能被捡到。
		//   ★★★ v5.1（本次修复）：读法从 `0x17D5BE0`（Lookup**OrAdd**）改成
		//     **只读走树**（LookupOutlineStateReadOnly）——
		//       · 0x17D5BE0 查不到就**插入**（new 0x30 字节节点 + 对该 REFR 引用计数
		//         `lock xadd` ++），而这一路是「每个 flora 引用 × 每次判据未命中」都要
		//         跑的 ⇒ 老版本玩得越久，引擎状态表里条目越多、放不掉的引用越多
		//         —— 用户报告的「1.8.1 随着游戏进行帧数持续降低」最可能的来源；
		//       · 现在一个条目都不插、引用计数一个都不动 ⇒ 「捡漏」可以一直开着；
		//       · 记录粒度也从 base 改成**引用**（治「同 species / 同资源全变绿」）。
		// ★★★ v5.2：返回值 = **结论**（0 = 引擎表里没有这个引用 / 1 = 引擎画的是青 /
		//   2 = 引擎画的是绿）——「抛弃记忆」后调用方（FloraTargetScanned ⓪.5）要能**直接用**
		//   这个结论，不再依赖 `floraRefKnow` 记忆表；下面写记忆的两行只在记忆开关
		//   打开时才会生效（RememberFloraRef 自己判断）。
		//   ★ 仍然**只读**（LookupOutlineStateReadOnly 自己走红黑树，不插条目、不动引用计数）。
		std::uint8_t ProbeFloraEngineState(const RE::TESObjectREFR* a_ref, const RE::TESForm* a_base)
		{
			if (!g_state.nativeReady || !a_ref || !a_base) {
				return 0;
			}
			const auto* p = LookupOutlineStateReadOnly(a_ref);
			if (!p) {
				++g_state.floraTableNoEntry;
				return 0;  // 引擎没给这个引用写过状态（没举过扫描仪时最常见）
			}
			++g_state.floraTableProbes;
			const std::uint32_t st = *p;
			if (st == kFloraStateEngineGreenA || st == kFloraStateEngineGreenB) {
				// 引擎亲手画的绿 =「这个引用已经扫描过」——最硬的证据
				++g_state.floraTableGreen;
				if (RememberFloraRef(a_ref, a_base, true, "引擎亲手把它画成了绿（outline state 4/5）")) {
					REX::INFO("flora scan: 引擎把 ref=0x{:X}（base=0x{:X}）画成了绿色（outline state {}）"
							  "-> 记下这个**引用**已扫描（★ v5.1：只读探针，不往状态表里插条目、跨会话落盘）",
						a_ref->GetFormID(), a_base->GetFormID(), st);
				}
				return 2;
			}
			if (st == kFloraStateEngineCyanA || st == kFloraStateEngineCyanB) {
				// 目前是青色（未扫描）——只在还没有「绿」记录时记一笔
				++g_state.floraTableCyan;
				RememberFloraRef(a_ref, a_base, false, "引擎亲手把它画成了青（outline state 7/8）");
				return 1;
			}
			return 0;
		}

		// ★★★ v4.33 / v5.1：**按引用**记忆的落盘（`<esm 同级>\SAS_AlwaysScan.flora-learn.txt`）。
		//   格式（★ v5.1）：每行 `<引用 FormID> <base FormID>`（十六进制，空格分隔）；
		//   `#` 开头 / 空行忽略。
		//   ★ v4.33~v5.0 的旧格式是「每行一个 base」—— 那正是「扫过一次，同 species /
		//     同资源的**所有**实例（含跨星球、跨存档）都变绿」的成因，这里会识别出来
		//     并**整体忽略**（日志给出条数；想彻底清掉可以直接删掉这个文件）。
		//   为什么还要落盘（用户实测：「低概率变青，开一遍扫描仪才会变回来」）：
		//     引擎 `GetOutlineState` 有时答不出「已扫描」⇒ 绿色只能靠「举过扫描仪时
		//     引擎亲手画过 4/5」学到；那条记忆不落盘的话**每次重开游戏都要再开一遍
		//     扫描仪**（v4.33 的原始动机）。现在落盘的是「哪个**引用**已扫描」。
		std::string FloraLearnTablePath()
		{
			if (!g_state.floraPersistPath.empty()) {
				return g_state.floraPersistPath;
			}
			// 与 INI 同目录（= esm 同级；老安装回退到 DLL 旁时也保持一致）
			std::string dir;
			if (const auto cut = g_iniPath.find_last_of('\\'); cut != std::string::npos) {
				dir = g_iniPath.substr(0, cut);
			} else {
				dir = ModuleDir();
			}
			g_state.floraPersistPath = dir.empty()
				? std::string{ "SAS_AlwaysScan.flora-learn.txt" }
				: dir + "\\SAS_AlwaysScan.flora-learn.txt";
			return g_state.floraPersistPath;
		}

		// 启动时读回（文件不存在 = 首次运行，属正常）
		void LoadFloraLearnTable()
		{
			// ★★★ v5.2：记忆层总开关（默认 0 = 抛弃记忆）—— 连落盘也不读；
			//   旧文件原样保留（把 FloraUseMemory 设回 1 就恢复）。
			if (!g_cfg.floraUseMemory) {
				g_state.floraPersistReady = true;
				return;
			}
			const auto path = FloraLearnTablePath();
			if (!g_cfg.floraLearnPersist) {
				g_state.floraPersistReady = true;
				REX::INFO("flora learn: 落盘已关（FloraLearnPersist=0）—— 按引用记忆只在内存里生效（{}）", path);
				return;
			}
			std::FILE* f = nullptr;
			if (::fopen_s(&f, path.c_str(), "r") != 0 || !f) {
				REX::INFO("flora learn: 落盘文件不存在（{}）—— 首次运行属正常；"
						  "以后每学到一条「这个引用已扫描」都会追加写进去（行格式：`引用 base`）",
					path);
				g_state.floraPersistReady = true;
				return;
			}
			// 读一行 = 两个十六进制 FormID（★ v5.1 起）+ 可选第 3 个字段（★ v5.1.7 起 =
			//   **见证时刻**（游戏时间，天））。只认「两个 token」的行；
			//   v4.33~v5.0 的「一个 base」旧格式会被数出来并忽略（成因见上面那段注释）。
			const auto readHex = [](const char* a_in, const char** a_outEnd) -> std::uint32_t {
				const char* h   = (a_in[0] == '0' && (a_in[1] == 'x' || a_in[1] == 'X')) ? a_in + 2 : a_in;
				char*       end = nullptr;
				const auto  v   = std::strtoul(h, &end, 16);
				if (a_outEnd) {
					*a_outEnd = (end && end != h) ? end : nullptr;
				}
				return static_cast<std::uint32_t>(v);
			};
			char line[192]{};
			while (std::fgets(line, sizeof(line), f)) {
				const char* p = line;
				while (*p == ' ' || *p == '\t') {
					++p;
				}
				if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') {
					continue;
				}
				const char* e1  = nullptr;
				const auto  ref = readHex(p, &e1);
				if (!e1 || ref == 0) {
					++g_state.floraPersistIgnored;
					continue;
				}
				const char* q = e1;
				while (*q == ' ' || *q == '\t') {
					++q;
				}
				if (*q == '\r' || *q == '\n' || *q == '\0') {
					++g_state.floraPersistIgnored;  // ★ 旧格式（只有一个 base）⇒ 忽略
					continue;
				}
				const char* e2   = nullptr;
				const auto  base = readHex(q, &e2);
				if (!e2 || base == 0) {
					++g_state.floraPersistIgnored;
					continue;
				}
				// ★★★ v5.1.7（订正 R7）：可选第 3 个字段 = 见证时刻（游戏时间，天）。
				//   v5.1.6 及更早写的行没有它 ⇒ days = 0 = 未知（读档时用
				//   `floraLegacyStampDays` 兜 —— 等价于 R5 的「第一次读档仍然有效」）。
				float days = 0.0f;
				{
					const char* r = e2;
					while (*r == ' ' || *r == '\t') {
						++r;
					}
					if (*r != '\r' && *r != '\n' && *r != '\0') {
						char*        de = nullptr;
						const double dv = std::strtod(r, &de);
						if (de && de != r && dv > 0.0 && dv < 1.0e7) {
							days = static_cast<float>(dv);
						}
					}
				}
				auto& e = g_state.floraRefKnow[ref];
				if (e.baseFid != base || !e.green) {
					e.baseFid = base;
					e.green   = true;
					// ★★★ v5.1.5（订正 R5）：落盘条目 =「**上一个游戏会话**见证的结论」，
					//   盖 epoch = 0 ⇒ 只有在本会话**第一次读档**时才算数
					//   （见 FloraEntryInScope；之后每次读档只认本存档内见证过的绿）。
					e.epoch = 0;
					e.days  = days;  // ★ v5.1.7
					++g_state.floraPersistLoaded;
					if (days <= 0.0f) {
						++g_state.floraPersistNoTime;
					}
				}
			}
			std::fclose(f);
			g_state.floraPersistReady = true;
			REX::INFO("flora learn: 按引用记忆已从落盘文件载入 {} 条（{}）-> 读档时按**见证时刻**"
					  "（★ v5.1.7：行第 3 字段 = 游戏时间）判定是否还在本存档作用域内"
					  "（时间 ≤ 本存档的游戏时间 ⇒ 直接判「已扫描」，不需要再开一遍扫描仪）；"
					  "★ 没有时间字段的旧行（{} 条）用「首次读档的锚」兜底 = 本会话第一次读档时仍然有效{}",
				g_state.floraPersistLoaded, path, g_state.floraPersistNoTime,
				g_state.floraPersistIgnored
					? "；★ 旧格式（base 级）行被忽略 " + std::to_string(g_state.floraPersistIgnored) +
						  " 条 —— 那种粒度会把同 species / 同资源的**所有**实例一起涂绿、"
						  "且跨存档生效，正是 v5.1 修掉的问题（想清空直接删这个文件）"
					: std::string{});
			// ★★★ v5.1.8（订正 R8）：载入完就用**作用域内**的绿条目播种物种表 ——
			//   「继续同一存档 / 传送」时同 species 的实例（FormID 变了也算）直接是绿的。
			SeedFloraBaseFromRefMemory("启动时载入落盘记忆");
		}

		// 新学到一条「这个引用已扫描」就追加写一行（文件不存在时创建；去重靠内存记忆）。
		//   ★ v5.1.7：第 3 个字段 = 见证时刻（游戏时间，天）—— 读档时用它判定
		//     「这个扫描发生在哪个存档的过去」（见 FloraEntryInScope / docs/40）。
		void AppendFloraLearnRecord(std::uint32_t a_refFid, std::uint32_t a_baseFid, float a_days)
		{
			// ★★★ v5.2：记忆层总开关（默认 0 = 抛弃记忆）—— 不再落盘。
			if (!g_cfg.floraUseMemory || !g_cfg.floraLearnPersist) {
				return;
			}
			const auto path = FloraLearnTablePath();
			std::FILE* f = nullptr;
			if (::fopen_s(&f, path.c_str(), "a") != 0 || !f) {
				if (g_state.floraPersistWrites == 0) {
					REX::WARN("flora learn: 落盘文件建不出来（{}）—— 只在内存里生效", path);
				}
				return;
			}
			std::fprintf(f, "0x%08X 0x%08X %.5f\n", a_refFid, a_baseFid,
				a_days > 0.0f ? a_days : 0.0f);
			std::fclose(f);
			++g_state.floraPersistWrites;
			if (g_state.floraPersistWrites == 1) {
				REX::INFO("flora learn: 开始落盘（{}）—— 以后学到的「这个引用已扫描」都追加到这里"
						  "（行格式：`引用 FormID base FormID 游戏时间(天)`），重开游戏直接读回"
						  "（想重置就删掉这个文件）",
					path);
			}
		}

		// ================================================================
		// ★★★ v5.1.5（订正 R5）：**读档 = 存档边界**（「已扫描」记忆按存档隔离）
		// ----------------------------------------------------------------
		// 用户报告：「未扫描星球资源直接显示绿色问题还存在，特别是重新读档后
		//   （未扫描时的存档）问题非常严重」。
		// 为什么必须用**引擎自己的读档事件**（`RE::TESLoadGameEvent`）：本项目
		//   v4.0 起用的「载入画面由开变关」这条信号**读档与换场景都会发**
		//   （快速旅行 / 进门都会经过载入画面），而「记忆还能不能用」只取决于
		//   **换没换存档** —— 换场景不会让勘测数据倒退，读档会。
		//
		// 事件类型：commonlibsf 里 `TESLoadGameEvent` 定义在 `RE/E/Events.h`
		//   （单独 include 会因缺类型编译不过 —— v4.0 / v4.11 都踩过）⇒ 按既有做法
		//   **本地定义一个同布局（空负载）的类型**；事件源走 commonlibsf 给的
		//   REL::ID（`TESLoadGameEvent::GetEventSource` = 64149，versionlib 里对得上）。
		//   sink 只做一件事：置一个**原子标志**（事件不保证在主线程），
		//   真正的处理放在主线程（载入画面关闭时，见 ProcessFloraSaveLoad）。
		// ================================================================
		struct SasLoadGameEvent
		{
		};

		class SasLoadGameSink final : public RE::BSTEventSink<SasLoadGameEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SasLoadGameEvent&,
				RE::BSTEventSource<SasLoadGameEvent>*) override
			{
				g_state.saveLoadPendingAtMs.store(NowMs(), std::memory_order_relaxed);
				g_state.saveLoadPending.store(true, std::memory_order_relaxed);
				g_state.loadEvtTotal.fetch_add(1, std::memory_order_relaxed);
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		SasLoadGameSink g_loadGameSink;

		// 主线程：确保读档事件 sink 挂着（失败每 4 秒重试；挂上后每 60 秒核对一次）。
		//   形状校验与既有的事件通道同一套（★ v4.11/v4.12/v4.13 的教训：vtable / 形状
		//   对不上就**绝不注册**，最坏结果只是少一条信号）。
		void EnsureLoadGameSink(std::uint64_t a_nowMs)
		{
			if (g_cfg.floraMemoryScope == 0) {
				return;  // 旧行为（对照）：不需要存档边界
			}
			if (g_state.loadSinkRegistered) {
				if (a_nowMs - g_state.loadSinkCheckMs < kLoadSinkCheckMs) {
					return;
				}
				g_state.loadSinkCheckMs = a_nowMs;
				if (!SinkStillInArray(g_state.loadSinkSource, &g_loadGameSink)) {
					g_state.loadSinkRegistered = false;
					g_state.loadSinkRetryAtMs  = 0;
					REX::WARN("load event: 读档事件的 sink 从事件源数组里掉出去了 -> 重挂它");
				} else {
					return;
				}
			}
			if (g_state.loadSinkRetryAtMs > a_nowMs) {
				return;
			}
			g_state.loadSinkRetryAtMs = a_nowMs + 4000;

			// 事件源：`TESLoadGameEvent::GetEventSource()`（返回 BSTEventSource 的地址）
			using func_t = void* (*)();
			static ::REL::Relocation<func_t> getSource{ RE::ID::TESLoadGameEvent::GetEventSource };
			auto* src = static_cast<std::uint8_t*>(getSource());
			if (!src || !IsReadable(src, 0x20)) {
				++g_state.loadSinkFailures;
				if (g_state.loadSinkFailures <= 3) {
					REX::WARN("load event: 读档事件源拿不到（{}）-> 这次不注册"
							  "（FloraMemoryScope=1 的「按存档隔离」会退化：读档不会作废过期记忆）",
						src ? "返回的地址不可读" : "GetEventSource() 返回空");
				}
				return;
			}
			const auto sz = *reinterpret_cast<const std::uint32_t*>(src + 0x08);
			const auto cp = *reinterpret_cast<const std::uint32_t*>(src + 0x0C);
			const auto dp = *reinterpret_cast<const std::uint64_t*>(src + 0x10);
			if (sz > cp || cp > 4096 || (sz != 0 && !IsReadable(reinterpret_cast<const void*>(dp), 8))) {
				++g_state.loadSinkFailures;
				if (g_state.loadSinkFailures <= 3) {
					REX::WARN("load event: 读档事件源的形状不像 BSTEventSource（size={} cap={} data=0x{:X}）-> 不注册",
						sz, cp, dp);
				}
				return;
			}
			reinterpret_cast<RE::BSTEventSource<SasLoadGameEvent>*>(src)->RegisterSink(&g_loadGameSink);
			g_state.loadSinkRegistered = true;
			g_state.loadSinkSource     = src;
			g_state.loadSinkCheckMs    = a_nowMs;
			REX::INFO("load event: 读档事件 sink 已注册（源 0x{:X}，sinks size={} cap={}）-> 每次读档都会把"
					  "「已扫描」记忆的**存档作用域** +1，并清空按物种扩散表（★ v5.1.5）",
				reinterpret_cast<std::uintptr_t>(src), sz, cp);
		}

		// ★★★ v5.1.5（订正 R5）/ v5.1.6（订正 R6）：这个 base（物种 / 资源）在本**存档**里的
		//   资源链结论 —— 三态（v5.1.6 从「能证伪吗」升级成三态：**「已扫描」这一档是反证**，
		//   见 ProcessFloraSaveLoad 的判定）：
		//     0 = 链不适用 / 拿不到结论（植物产出 MISC、无产出字段、形状校验没过、判据没就绪）
		//     1 = 引擎答「在勘测数据里」（已扫描）
		//     2 = 引擎答「不在勘测数据里」（可证伪 = 本存档里必然没扫描）
		//   判据函数 = 引擎自己的 `0x1597A50`（live、跟随存档）。
		constexpr std::uint8_t kFloraChainNoVerdict = 0;
		constexpr std::uint8_t kFloraChainScanned   = 1;
		constexpr std::uint8_t kFloraChainUnscanned = 2;

		std::uint8_t FloraBaseChainVerdictInThisSave(std::uint32_t a_baseFid)
		{
			if (!g_isResourceScannedReady || !g_cfg.floraScannedByResource || !a_baseFid) {
				return kFloraChainNoVerdict;
			}
			auto* base = RE::TESForm::LookupByID(a_baseFid);
			if (!base) {
				return kFloraChainNoVerdict;
			}
			FloraChain chain{};
			const bool scanned = QueryFloraResourceScanned(base, &chain);
			if (!chain.shapeOk || chain.produceIsMisc) {
				return kFloraChainNoVerdict;  // 链不适用 / 走不通 ⇒ 拿不到结论
			}
			return scanned ? kFloraChainScanned : kFloraChainUnscanned;
		}

		// ★★★ v5.1.6（订正 R6）：复核时的**作用域过滤** —— 这个 base 该跳过吗？
		//   为什么需要：资源链查询的上下文含**当前世界空间（星球 / 内景）**——
		//   记忆里「见证于别的世界空间」的 base 在当前上下文上查，很可能答「没扫描」
		//   （= 假证伪）。用户实测（2026-09-27 09:55 那一局的传送）：8 个
		//   **本会话当场见证过、引擎亲手画过绿**的矿脉 base 被 v5.1.5 证伪并丢掉，
		//   而用户随后一挙扫描仪，引擎又把同一批目标画成绿 = 它们在本存档里
		//   明明是已扫描的 ⇒ 那次「证伪」正是假证伪。
		//   规则：物种表里有这个 base 的见证记录、且记录里**只有别的世界空间**
		//   ⇒ 跳过复核（不当作证伪）；读不到当前世界空间 / 没有见证记录 ⇒ 不跳过
		//   （落盘记忆没有作用域信息，只能按「本存档」严格复核）。
		bool ScopeSkipBaseInThisWorldspace(std::uint32_t a_baseFid, std::uintptr_t a_curWs)
		{
			if (!g_cfg.floraSpeciesPlanetScope || a_curWs == 0) {
				return false;
			}
			const auto it = g_state.floraBaseKnow.find(a_baseFid);
			if (it == g_state.floraBaseKnow.end()) {
				return false;
			}
			const auto& e = it->second;
			if (e.count == 0) {
				return false;  // 见证时也读不到 worldspace ⇒ 不收紧
			}
			return !e.Seen(a_curWs);
		}

		// ================================================================
		// ★★★ v5.1.5（订正 R5）/ v5.1.6（订正 R6）：处理一次**读档候选**
		// ----------------------------------------------------------------
		// v5.1.5：把引擎的 `TESLoadGameEvent` 当**存档边界**（读档 = 勘测数据可能倒退）：
		//   ① `floraSaveEpoch + 1` —— 之后只有「本存档内见证过」的结论才算数
		//      （见 FloraEntryInScope；落盘条目的 epoch = 0）；
		//   ② **清空按物种（base）扩散表** —— 它是「同 species 全绿」的放大器，
		//      必须跟随存档（本存档内的见证会重新把它填起来）；
		//   ③ **资源链复核**：对记忆里出现过的 base 逐个问引擎 `0x1597A50`
		//      （这个资源在不在**本存档**的勘测数据里）—— 链走通且答「没有」
		//      ⇒ 该 base 的**全部条目当场丢掉**。
		// ------------------------------------------------------------------
		// v5.1.6（本次修复）：**事件不再无条件当作存档边界**（用户实测「传送切换
		//   地图后，已扫描物品变成青色」—— 引擎这个事件在部分传送 / 换世界空间时
		//   也会发）。改为**证据驱动**：
		//     · 先复核（且复核已延后到世界稳定，见 MaybeProcessFloraSaveLoad /
		//       ScopeSkipBaseInThisWorldspace）；
		//     · **一个 base 都证伪不了** ⇒ 判定「传送 / 读同一存档」（勘测数据
		//       没有倒退）⇒ 记忆**保持有效**（作用域不推进、物种表不清、条目不丢），
		//       只把链**确认**（本存档里已扫描）的绿条目「重见证」到当前作用域；
		//     · 有证伪 ⇒ 才是真「读回了更早的存档」⇒ 走 v5.1.5 的作废路径。
		//   回退：`FloraLoadBoundaryEvidence=0` 回到 v5.1.5 行为；`FloraMemoryScope=2`
		//   仍按「每次事件都推进」的最严格口径。
		// ================================================================
		constexpr std::size_t    kFloraScopeValidateMaxBase = 64;  // 每次读档最多复核多少个 base
		// （复核延后时长 `kFloraScopeEvalDelayMs` 定义在文件上方常量区 —— 启动日志也要引用它。）

		// ================================================================
		// ★★★ v5.1.7（订正 R7）：用**游戏时间指纹**处理一次读档候选（默认口径）
		// ----------------------------------------------------------------
		// 返回 true = 已按新口径处理完（调用方直接 return）；false = 读不到时间 / 开关关掉
		//   / `FloraMemoryScope` 不是 1 ⇒ 交给 v5.1.6 的链证据路径（fallback，原样保留）。
		//
		// 与 v5.1.6 的三点区别（全部来自用户 2026-09-27 10:15 那一局的实证，见 docs/40）：
		//   ① **不做资源链复核** —— 那个时刻的否定答案没有区分度（同一资源 80 秒后答相反）；
		//   ② **不删任何条目** —— 作废 = 「不在本存档作用域」（可逆：换更新的存档就自动回来）；
		//   ③ 传送 / 继续同一存档 ⇒ 锚点几乎不动 ⇒ 什么都不作废（不再依赖链的自证）。
		//
		// 「作废」的语义 = `FloraEntryInScope` 用锚点把「属于更新时间线」的条目挡在作用域外
		//   （它们不再产生绿）；同一次读档里，**本时间线内当场见证**的条目（epoch == 当前）
		//   不受锚点限制。物种表则按每个 base 的**最早见证时刻**剪枝。
		// ================================================================
		bool TryProcessFloraSaveLoadByTime(const char* a_when, std::uint64_t a_loadNo)
		{
			if (g_cfg.floraMemoryScope != 1 || !g_cfg.floraSaveFingerprint) {
				return false;
			}
			float days = 0.0f;
			if (!ReadGameDays(&days)) {
				++g_state.floraFpFailures;
				if (g_state.floraFpFailures <= 3) {
					REX::WARN("flora memory: 读档候选 #{}：**游戏时间读不到**"
							  "（Calendar / GameDaysPassed 形状校验没过）⇒ 本次回退 v5.1.6 的链证据路径"
							  "（那一套的否定答案在载入刚结束时可能不准，FloraSaveFingerprint 想关就设 0）",
						a_loadNo);
				}
				return false;
			}
			const float floorOld = g_state.floraSaveDaysFloor;
			if (g_state.floraLegacyStampDays < 0.0f) {
				g_state.floraLegacyStampDays = days;  // 旧格式行的锚（= R5 的「第一次读档仍有效」）
			}
			g_state.floraSaveDaysFloor = days;  // ★ 锚点 = 本存档的游戏时间
			++g_state.floraSaveEpoch;           // 「本时间线」标记（读档后当场见证的条目不受锚点限制）
			++g_state.floraFpLoads;
			ReadSaveNameDiag();

			const float limit = days + kFloraSaveTimeEpsDays;
			std::size_t voided = 0;
			std::size_t kept   = 0;
			for (const auto& kv : g_state.floraRefKnow) {
				const auto& e = kv.second;
				if (e.epoch == g_state.floraSaveEpoch) {
					++kept;  // 刚 bump，正常不会有；留着兜异常
					continue;
				}
				float d = e.days;
				if (!(d > 0.0f)) {
					d = g_state.floraLegacyStampDays;
				}
				if (d > 0.0f && d > limit) {
					++voided;
				} else {
					++kept;
				}
			}
			std::size_t pruned = 0;
			for (auto it = g_state.floraBaseKnow.begin(); it != g_state.floraBaseKnow.end();) {
				float d = it->second.days;
				if (!(d > 0.0f)) {
					d = g_state.floraLegacyStampDays;
				}
				if (d > 0.0f && d > limit) {
					it = g_state.floraBaseKnow.erase(it);  // 这个物种的绿属于更新的时间线 ⇒ 剪掉
					++pruned;
				} else {
					++it;
				}
			}
			g_state.floraFpVoided += voided;
			g_state.floraFpClearedBase += pruned;

			// ★★★ v5.1.8（订正 R8）：剪枝之后再播种一次 —— 属于「更新的时间线」的物种
			//   已经被剪掉，剩下的（含落盘条目）都在本存档作用域内 ⇒ 用它们播种，
			//   同 species 的实例（含 FormID 换过的）**不必等举扫描仪**就是绿的。
			const auto seeded = SeedFloraBaseFromRefMemory(a_when);

			REX::INFO("flora memory: 读档候选 #{}（{}）-> ★ **存档时间指纹**：本存档的游戏时间 = {:.5f} 天"
					  "（上次锚点 = {}；存档 = {} / #{}）⇒ **不在本存档作用域**的条目 {} 条 / 仍有效 {} 条"
					  "（物种表剪掉 {} 个 base；播种后物种表 {} 个 base，本次新增 {} 个）"
					  "—— ★ **不删任何条目**（作用域可逆：换一个更新的存档就会自动回来）"
					  "；★ v5.1.8（订正 R8：物种表用落盘记忆播种 + 扩散不再按星球硬拒绝 —— "
					  "传送 / 换星球后已扫描的 species 直接是绿的）",
				a_loadNo, a_when, days,
				floorOld >= 0.0f ? std::to_string(floorOld) : std::string("无"),
				g_state.floraFpSaveName[0] ? g_state.floraFpSaveName : "?",
				g_state.floraFpSaveNo,
				voided, kept, pruned,
				g_state.floraBaseKnow.size(), seeded);
			return true;
		}

		void ProcessFloraSaveLoad(const char* a_when)
		{
			++g_state.floraSaveLoads;
			const auto loadNo = g_state.floraSaveLoads;

			if (g_cfg.floraMemoryScope == 0) {
				REX::INFO("flora memory: 读档候选 #{}（{}）—— FloraMemoryScope=0 ⇒ 旧行为"
						  "（记忆不按存档隔离、物种表不清）",
					loadNo, a_when);
				return;
			}

			// ★★★ v5.1.7（订正 R7）：**优先按「游戏时间指纹」处理**（见上面的长注释 / docs/40）。
			//   读不到时间 / 开关关掉 / scope != 1 ⇒ 落到下面 v5.1.6 的链证据路径（回退）。
			if (TryProcessFloraSaveLoadByTime(a_when, loadNo)) {
				return;
			}

			// ① 资源链复核（★ v5.1.6：**先复核、后决策** —— v5.1.5 是先作废再复核）
			//   ★ 注意（v5.1.7）：这条路只在「读不到游戏时间」时才会走到 —— 那时链的
			//     否定答案依旧不可靠（见上方长注释），所以它只是**兜底**，不是默认口径。
			const auto                        curWs = CurrentWorldspace();
			std::size_t                       checked = 0;
			std::size_t                       skipped = 0;
			std::size_t                       noVerdict = 0;
			std::unordered_set<std::uint32_t> dropBases;
			std::unordered_set<std::uint32_t> confirmedBases;
			{
				std::unordered_set<std::uint32_t> bases;
				for (const auto& kv : g_state.floraRefKnow) {
					if (kv.second.baseFid) {
						bases.insert(kv.second.baseFid);
					}
				}
				for (const auto base : bases) {
					if (checked >= kFloraScopeValidateMaxBase) {
						++skipped;
						continue;
					}
					if (ScopeSkipBaseInThisWorldspace(base, curWs)) {
						++skipped;
						continue;
					}
					++checked;
					const auto verdict = FloraBaseChainVerdictInThisSave(base);
					if (verdict == kFloraChainUnscanned) {
						dropBases.insert(base);
					} else if (verdict == kFloraChainScanned) {
						confirmedBases.insert(base);
					} else {
						++noVerdict;
					}
				}
			}
			g_state.floraScopeConfirmedBase += confirmedBases.size();
			g_state.floraScopeChainDisproved += dropBases.size();
			g_state.floraScopeChainSkip += skipped;

			// ② ★★★ v5.1.6（订正 R6）：**没有证伪 ⇒ 这不是「更早的存档」**（= 传送 /
			//   读同一存档）⇒ 记忆保持有效，不做任何作废。
			//   为什么可信：`0x1597A50` 已经在这个上下文里答了「有 / 没有」——
			//   勘测数据倒退（读回更早存档）时，记忆里的绿**必然**会被大规模证伪；
			//   而一个都证伪不了，就说明这些绿在本存档里确实存在（本会话学到的绿
			//   本来就是「引擎亲手画 4/5」确认过的）。
			const bool evidenceMode = g_cfg.floraLoadBoundaryEvidence && g_cfg.floraMemoryScope == 1;
			if (evidenceMode && dropBases.empty()) {
				++g_state.floraScopeKept;
				for (auto& kv : g_state.floraRefKnow) {
					if (kv.second.green && confirmedBases.count(kv.second.baseFid) &&
						kv.second.epoch != g_state.floraSaveEpoch) {
						kv.second.epoch = g_state.floraSaveEpoch;  // 「重见证」（不改结论、不重复落盘）
						++g_state.floraScopeRescoped;
					}
				}
				REX::INFO("flora memory: 读档候选 #{}（{}）-> 资源链复核 {} 个 base：**没有一个被证伪**"
						  "（确认已扫描 {} / 无结论 {} / 跳过 {}；当前世界空间 0x{:X}）⇒ 判定为**传送 / 读同一存档**"
						  "（勘测数据没有倒退）⇒ 「已扫描」记忆**保持有效**（作用域不推进、物种表不清、条目不丢；"
						  "重见证 {} 条）—— ★ v5.1.6（订正 R6：不再把传送误当作读档）",
					loadNo, a_when, checked, confirmedBases.size(), noVerdict, skipped, curWs,
					g_state.floraScopeRescoped);
				// ★★★ v5.1.8（订正 R8）：这条路径也是「记忆保持有效」⇒ 顺手播种一次
				//   （作用域内的落盘条目 ⇒ 物种表；FormID 换过的实例也能直接是绿的）。
				SeedFloraBaseFromRefMemory(a_when);
				return;
			}

			// ③ 真·存档边界：勘测数据确实倒退（有 base 被证伪）⇒ 走 v5.1.5 的作废路径。
			//   `FloraMemoryScope=2`（最严格）也走这里（不依赖证据，每次事件都推进）。
			//   ★★★ v5.1.7（订正 R7）：这里**不再删条目**（和主线的新口径一致）——
			//     「没扫描」这个否定答案在载入刚结束时不可靠（见上面 TryProcessFloraLoadSaveByTime
			//     的长注释），所以只用**作用域**表达作废（epoch 推到 >= 2 ⇒ 落盘条目一律
			//     不在作用域，需要重新见证）；条目本身留着，换个更新的存档就会回来。
			++g_state.floraSaveEpoch;
			if (g_state.floraSaveEpoch < 2) {
				g_state.floraSaveEpoch = 2;  // 第一次读档也能作废落盘条目（v5.1.5 是靠上面的删除达到同样效果）
			}
			const auto clearedBases    = g_state.floraBaseKnow.size();
			g_state.floraScopeClearedBase += clearedBases;
			g_state.floraBaseKnow.clear();

			g_state.floraScopeDroppedBase += dropBases.size();  // 诊断：被证伪的 base 数（不再真删）

			REX::INFO("flora memory: 读档 #{} 处理完（{}）-> 资源链**证伪 {} 个 base**"
					  "⇒ 判定为**读回了更早的存档** ⇒ 「已扫描」记忆的存档作用域 = 第 {} 次读档；"
					  "按物种扩散表已清空 {} 个 base{}（复核 {} 个：确认已扫描 {} / 无结论 {} / 跳过 {}；"
					  "当前世界空间 0x{:X}）；★ v5.1.7：被证伪的条目**不再删除**（只按作用域失效，"
					  "换更新的存档就会回来）；之后这些目标只会由「本存档内当场见证」"
					  "（引擎画 4/5 / 状态==2 / 链命中）重新变绿 —— ★ 落盘记忆仍在"
					  "（重开游戏继续玩同一个存档时有效，FloraMemoryScope={}）",
				loadNo, a_when, dropBases.size(), g_state.floraSaveEpoch, clearedBases,
				skipped ? ("（另有 " + std::to_string(skipped) + " 个 base 被跳过：超预算 / 只见证于别的世界空间）")
						: std::string{},
				checked, confirmedBases.size(), noVerdict, skipped, curWs,
				g_cfg.floraMemoryScope);
		}

		// ★★★ v5.1.6（订正 R6）：收到读档事件 ⇒ **只记「待复核」**（不立刻处理）。
		//   为什么延后：v5.1.5 在「载入画面关闭」那一瞬间就跑资源链复核 —— 实测在
		//   传送 / 换世界空间时拿到**假证伪**（勘测数据 / 上下文可能还没就绪）。
		//   延后到「载入画面关闭 + kFloraScopeEvalDelayMs 且没有新的载入」后再做
		//   （见 MaybeProcessFloraSaveLoad），那一刻世界已经稳定。
		//   `saveLoadPending` 的消费从 ProcessFloraSaveLoad 挪到这里 ——
		//   「不是读档事件的载入画面关闭」（纯快速旅行 / 进门）仍然什么都不做。
		void NoteFloraSaveLoad(const char* a_when, std::uint64_t a_nowMs)
		{
			if (!g_state.saveLoadPending.exchange(false)) {
				return;  // 这次「载入画面关闭」不是读档事件（= 快速旅行 / 进门）⇒ 什么都不做
			}
			g_state.floraScopeEvalWhen = a_when;
			g_state.floraScopeEvalAtMs = a_nowMs + kFloraScopeEvalDelayMs;
		}

		// 每帧：到点（且世界稳定）⇒ 真正处理（Tick 里在早退之前调用）。
		void MaybeProcessFloraSaveLoad(std::uint64_t a_nowMs)
		{
			if (!g_state.floraScopeEvalAtMs || a_nowMs < g_state.floraScopeEvalAtMs) {
				return;
			}
			if (IsLoadingScreenUp()) {
				return;  // 又弹了载入画面 ⇒ 继续等（复核必须在世界稳定后做）
			}
			g_state.floraScopeEvalAtMs = 0;
			ProcessFloraSaveLoad(g_state.floraScopeEvalWhen ? g_state.floraScopeEvalWhen : "读档事件");
		}

		// ③ 热路径入口：**每个引用**按 TTL（已扫描 30 秒 / 未扫描见
		//   `FloraUnscannedTtlMs`）最多问引擎一次。
		//   返回「这个星球目标已经扫描过（原版绿）」。
		//   ★ v4.28：判据顺序 = ① 引擎状态（`GetOutlineState(ref)`，主判据）
		//     → ② 引擎亲手画过的绿（会话内学习）→ ③ 资源链（**只对 LVLI 产出**）。
		//   ★★★ v4.31：在最前面加 **⓪ 单向学习表**，并把「引擎亲手画过的绿」并入它
		//     —— 权威确认过「已扫描」的条目直接返回；另外「重问拿不到权威答案」时
		//     **沿用旧结论**（不再一律降成未扫描）。
		//   ★★★ v4.33：
		//     ① ⓪ 与 ① 之间加 **⓪.5 引擎状态表捡漏**（放下扫描仪后也读一眼 4/5）；
		//     ② 学习表**跨场景 / 跨会话保留**（落盘，见 LoadFloraLearnTable）；
		//     ③ 「沿用」判据订正：`chain.produceIsMisc || !chain.shapeOk`（见下）。
		//   ★★★ v5.1（本次修复）：
		//     ① ⓪ 的粒度从 **base** 改成 **引用**（`floraRefKnow`）—— 治用户实测的
		//        「矿石、植物扫描前后颜色都是绿色」：扫过一个实例不再让同 species /
		//        同资源的所有实例、跨星球、跨存档一起变绿；
		//     ② ⓪.5 的读取改成**只读走树**（不再调 LookupOrAdd：它插入条目 +
		//        加引用计数 ⇒ 玩得越久越卡，见常量区 kRvaOutlineStateTree）。
		bool FloraTargetScanned(const RE::TESObjectREFR* a_ref, const RE::TESForm* a_base)
		{
			if (!a_base) {
				return false;
			}
			const bool engineStateOn = g_cfg.floraScannedByEngineState && g_scannableOutlineStateReady;
			const bool chainOn       = g_cfg.floraScannedByResource && g_isResourceScannedReady;
			// ★★★ v5.2：新判据（引擎扫描进度直读）也算「一条可用判据」——
			//   旧版这里只看「引擎状态」与「资源链」两条；少了这一条，
			//   GetOutlineState 签名不匹配时会把新判据也一起挡掉。
			const bool progressOn = g_cfg.floraEngineProgress && g_floraProgressReady;
			if (!progressOn && !engineStateOn && !chainOn) {
				return false;  // 所有判据都关了 / 都不可用 ⇒ 退回「扫没扫过看起来一样」
			}
			const std::uint32_t fid = a_base->GetFormID();
			const auto          now = NowMs();
			// ★★★ v4.31：缓存过期时先把**旧结论**留着 —— 末尾「沿用」要用它
			//   （见「低概率变青」的修复说明）。旧记录必须先核对 base（指针复用）。
			bool prevScanned = false;
			bool hasPrev     = false;
			const auto it    = g_state.floraScannedCache.find(a_ref);
			if (it != g_state.floraScannedCache.end()) {
				if (it->second.baseFid == fid) {
					// 缓存命中还要**核对 base**：引用指针可能已被回收去装别的东西
					// ★★★ v4.33：**判成「已扫描」的按 30 秒 TTL；判成「未扫描」的按
					//   FloraUnscannedTtlMs（默认 5 秒）** —— 引擎状态对同一个引用会
					//   一会儿答 2、一会儿答 1（组件 / 登记未就绪），旧版一律缓存 30 秒
					//   ⇒ 引擎刚恢复「已扫描」也要等 30 秒才变绿（用户看到的「变回青色」
					//   的窗口期就是这么来的）。短 TTL 只影响重问频率（引擎调用很便宜）。
					const auto ttl = it->second.scanned
						? kFloraScanCacheTtlMs
						: (g_cfg.floraUnscannedTtlMs > 0
								? std::min<std::uint64_t>(kFloraScanCacheTtlMs,
									  static_cast<std::uint64_t>(g_cfg.floraUnscannedTtlMs))
								: kFloraScanCacheTtlMs);
					if (now - it->second.atMs < ttl) {
						return it->second.scanned;
					}
					prevScanned = it->second.scanned;
					hasPrev     = true;
				}
				g_state.floraScannedCache.erase(it);
			}
			if (g_state.floraScannedCache.size() >= kFloraScanCacheMax) {
				g_state.floraScannedCache.clear();  // 兜异常增长（正常情况下几十条）
			}

			// ⓠ ★★★ v5.2：**直读引擎的扫描进度表**（= 本次「抛弃记忆」后的植物主判据）
			// ----------------------------------------------------------------
			// 用户指令（原文）：「不能直接读取游戏自己的物件状态来判断吗？……我们还是走
			//   植物也直接读游戏本身物件状态的方式，抛弃自己记忆的方法」。
			// 这条判据就是那份「游戏自己的物件状态」：引擎给**已扫描**植物写 state 4/5
			//   用的就是它（`PlayerKnowledge` 物种槽的 `percent`，== 100 = 已扫描；
			//   完整反汇编证据链见常量区 v5.2 段）—— **随时可问、只读**，
			//   与「引擎在可见窗口里画过 / 我们记下来」无关。
			// 语义：
			//   · `percent == 100` ⇒ 已扫描（绿，短路）；
			//   · `percent < 100`（查到了但未满）⇒ **不短路**（继续走 ⓪.5 / ① / ②）——
			//     宁可青，也不赌「部分进度算不算已扫描」；
			//   · 查询失败 ⇒ 不短路（自动回退旧判据链；`floraProgFails=` 计数可见）。
			//   · 结果按 **base（物种）** 缓存 ⇒ 同 species 的实例共享一条结论
			//     （引擎数据本来就是物种级的）——「扫一个实例 ⇒ 同 species 全绿」
			//     由**引擎数据**保证，不再需要 v5.1.2 的物种扩散表。
			//   ★★★ 订正 R12：`progSaidPartial` = 本轮直读**查到了但未满**（percent < 100）——
			//     供 ⓪.5 / ① 的「冲突诊断」用（直读说未满、引擎却画了绿 4/5）。
			bool progSaidPartial = false;
			if (a_ref) {
				const auto pr = QueryFloraScanProgressCached(a_ref, fid, now);
				if (pr.ok && pr.progress < kFloraProgressFull) {
					progSaidPartial = true;
				}
				if (pr.ok && pr.progress == kFloraProgressFull) {
					++g_state.floraScanHits;
					g_state.floraScannedCache.emplace(a_ref, State::FloraScanRec{ true, now, fid, 7 });
					// ★★★ 订正 R12：判绿行走**独立额度**（R11 两局里 8 条额度全被会话
					//   开头的判青最终行吃光 ⇒ 这一行在日志里结构上不可能出现）。
					if (g_cfg.floraScanProbeMax > 0 &&
						g_state.floraScanGreenProbes < static_cast<std::uint32_t>(g_cfg.floraScanProbeMax)) {
						++g_state.floraScanGreenProbes;
						REX::INFO("flora scan: ref=0x{:X} base=0x{:X} 判据 = 引擎扫描进度直读（★ v5.2）"
								  "：percent={}/100（key1=0x{:X} key2=0x{:X}）-> 已扫描 ⇒ 状态 {}（原版「已扫描」绿）",
							a_ref->GetFormID(), fid, pr.progress, pr.key1, pr.key2, FloraScannedState());
					}
					return true;
				}
			}

			// ⓪ ★★★ v4.31 / v5.1：**按引用**的单向记忆（最早、最省）——
			//   1) 「已扫描」：权威确认过 ⇒ 直接返回，不再重问引擎、也不再走链
			//      （「低概率变青」的根治：引擎有时会「拿不到 / 答未扫描」，
			//       而重问会把上次的结论推翻）；
			//   2) 「未扫描」（青）：**只是提示，不再短路**（★★ v5.1.1 订正）。
			//      v5.1 原本在这里直接 `return false` —— 而这条「青」记忆只有
			//      「引擎亲手画过 7/8」一个来源（= 举着扫描仪时被我们探针看到），
			//      一旦记下就**再也不会被主判据 / 资源链翻案** ⇒ 玩家随后真的扫了它，
			//      只要探针没赶上引擎写 4/5 的那一小段窗口，它就**永远停在青色**
			//      （用户报告：「少部分扫描后未变色」）。现在「青」只当提示：
			//      继续往下走 ⓪.5（引擎状态表只读探针）→ ① 主判据 → ② 资源链，
			//      任何一条给出「已扫描」都会把它升级成绿（绿仍然单向、不会被翻回）。
			//   ★ 粒度 = **引用**（+ base 校验）：同 base 的其它实例**不受影响** ——
			//     v4.31~v5.0 记的是 base，那会让整片同 species / 同资源一起变绿。
			//   ★★★ v5.2：整段由 **`FloraUseMemory`** 控制 —— 默认 **0 = 抛弃记忆**
			//     （用户指令），这段判据（以及下面的 ⓪.2 / 播种 / 落盘）全部不参与；
			//     想回退到 v5.1.8 行为才把它设成 1。
			if (g_cfg.floraUseMemory && a_ref) {
				bool knownGreen = false;
				if (FloraRefKnown(a_ref, fid, &knownGreen) && knownGreen) {
					++g_state.floraRefHits;
					++g_state.floraScanHits;
					++g_state.floraLearnedHits;
					// ★★★ v5.1.2（订正 R2）：引用级绿记忆命中 ⇒ 顺手**动态激活**物种表。
					//   场景：重开游戏（引用级记忆从落盘读回、物种表却是空的）——
					//   本会话第一次遇到这条记忆时把它的 base 也放行 ⇒ 同 species 的
					//   其它实例**立刻**一起变绿，不必等玩家再举一次扫描仪。
					//   ★ 影响面与「引用级记忆」本身完全相同（记忆是绿的 ⇒ 才激活），
					//     不会额外外溢到别的存档。
					//   ★ v5.1.5：带上这个引用所在的 worldspace ⇒ 只放行**同一颗星球**。
					RememberFloraBase(fid, WorldspaceOfRef(a_ref), "引用级记忆命中（跨会话动态激活）");
					g_state.floraScannedCache.emplace(a_ref,
						State::FloraScanRec{ true, now, fid, 4 });
					if (g_cfg.floraScanProbeMax > 0 &&
						g_state.floraScanGreenProbes < static_cast<std::uint32_t>(g_cfg.floraScanProbeMax)) {
						++g_state.floraScanGreenProbes;
						REX::INFO("flora scan: ref=0x{:X} base=0x{:X} 判据 = 按引用记忆（★ v5.1）"
								  "-> 已扫描 ⇒ 状态 {}（原版「已扫描」绿）",
							a_ref->GetFormID(), fid, FloraScannedState());
					}
					return true;
				}
			}

			// ⓪.2 ★★★ v5.1.2（订正 R2）：**按物种（base）扩散** —— 纯内存查表，零引擎调用。
			//   ★★★ v5.1.5（订正 R5）：**扩散只在同一颗星球内**（FloraBaseKnown 第二参
			//     = 该引用的 worldspace）—— 用户报的「未扫描星球资源直接显示绿色」
			//     有一半来自 v5.1.2 的**全局**扩散（在星球 A 学到的物种会让星球 B 上
			//     同种但没扫过的资源也变绿，正是 v5.1 之前那类假绿，docs/33）。
			//   引擎知识库是 species（资源）级的：只要这个物种被权威确认过（任一实例
			//   被引擎画成 4/5、或引擎状态 == 2、或资源链命中），**同 base 的所有实例**
			//   都该是原版绿 —— 这正是「打开扫描仪再关闭就变绿」的那批目标
			//   （它们在引擎的画里本来就是绿的，只是之前没被我们学到）。
			//   判定放在 ⓪ 之后、⓪.5（探针）之前：省掉一次状态表读取。
			//   ★★★ v5.1.5（订正 R5）：**限制在同一颗星球**（worldspace）—— 用户报的
			//     「未扫描星球资源直接显示绿色」有一半就是这么来的（跨星球外溢）。
			//   ★★★ v5.2：同样由 **`FloraUseMemory`** 控制（默认 0 = 抛弃记忆）——
			//     用户报的「所有星球资源固定显示已扫描绿」的主嫌就是这个物种表
			//     （R8 的「跨星球放行 + 落盘播种」让它被大面积填充）；新口径下
			//     「同 species 全绿」改由 ⓠ 的**引擎进度数据**保证。
			if (g_cfg.floraUseMemory && FloraBaseKnown(fid, WorldspaceOfRef(a_ref))) {
				++g_state.floraBaseHits;
				++g_state.floraScanHits;
				++g_state.floraLearnedHits;
				g_state.floraScannedCache.emplace(a_ref,
					State::FloraScanRec{ true, now, fid, 6 });
				if (g_cfg.floraScanProbeMax > 0 &&
					g_state.floraScanGreenProbes < static_cast<std::uint32_t>(g_cfg.floraScanProbeMax)) {
					++g_state.floraScanGreenProbes;
					REX::INFO("flora scan: ref=0x{:X} base=0x{:X} 判据 = 按物种扩散（★ v5.1.2："
							  "同 species / 同资源已被权威确认；★ v5.1.5：同一颗星球）"
							  "-> 已扫描 ⇒ 状态 {}（原版「已扫描」绿）",
						a_ref->GetFormID(), fid, FloraScannedState());
				}
				return true;
			}

			// ⓪.5 ★★★ v4.33：学习表没命中时**顺手读一眼引擎的状态表**（捡漏）——
			//   引擎在玩家「举过扫描仪」之后会把已扫描目标的状态留在表里（4/5 绿），
			//   而 v4.26~v4.32 只在「举着扫描仪（让位）」那一刻读它 ⇒ 放下扫描仪之后
			//   就不再捡。现在改成判据未命中时读一眼（纯内存读 + 引用级缓存限流，
			//   同一个引用最多每 FloraUnscannedTtlMs 一次）⇒ 引擎留过的绿不会白丢。
			if (a_ref) {
				// ★★★ v5.2：探针现在**返回结论**（2 = 引擎画的是绿）—— 记忆层关闭时
				//   直接消费这个结论（不再要求"先记进记忆表再查"）；记忆层打开时再
				//   回查记忆表（补上"以前学到过绿、此刻引擎表里已经没条目"的情况）。
				const std::uint8_t engState   = ProbeFloraEngineState(a_ref, a_base);
				bool               greenKnown = (engState == 2);
				// ★★ v5.1.1：这里同样**只让「绿」短路**。v5.1 的写法是
				//   「只要记忆里有结论（含「青」）就 return」—— 而记忆里的「青」
				//   可能是很久以前探针记下的（此刻引擎表里早就没有它了），
				//   于是每一轮都在这里被它挡住，① 主判据 / ② 资源链永远跑不到。
				if (!greenKnown && g_cfg.floraUseMemory) {
					bool memGreen = false;
					greenKnown = FloraRefKnown(a_ref, fid, &memGreen) && memGreen;
				}
				if (greenKnown) {
					// ★★★ 订正 R12：**冲突计数**（`冲突=` 定义后第一次真正自增）——
					//   「直读答『查到但未满』」而「引擎状态表说 4/5 绿」两者矛盾
					//   （按引擎口径 percent 到 100 才会写 4/5）。只记诊断 + 首条 WARN，
					//   **不影响行为**（绿仍然单向）。
					if (progSaidPartial) {
						++g_state.floraProgConflict;
						if (!g_state.floraProgConflictWarned) {
							g_state.floraProgConflictWarned = true;
							REX::WARN("flora progress: ★ 冲突 —— 直读答「未满」而引擎状态表画的是绿"
									  "（ref=0x{:X} base=0x{:X}）-> 诊断计数 `冲突=`；不影响行为",
								a_ref->GetFormID(), fid);
						}
					}
					++g_state.floraStatusTableHits;
					++g_state.floraScanHits;
					++g_state.floraLearnedHits;
					// ★★★ v5.1.2：同 ⓪ —— 命中即动态激活物种表（见那里的长注释）
					//   ★ v5.1.5：带上 worldspace（只在同一颗星球扩散）
					//   ★ v5.2：只在记忆层打开时做（默认关闭）
					if (g_cfg.floraUseMemory) {
						RememberFloraBase(fid, WorldspaceOfRef(a_ref), "状态表捡漏命中（动态激活）");
					}
					if (g_cfg.floraScanProbeMax > 0 &&
						g_state.floraScanGreenProbes < static_cast<std::uint32_t>(g_cfg.floraScanProbeMax)) {
						++g_state.floraScanGreenProbes;
						REX::INFO("flora scan: ref=0x{:X} base=0x{:X} 判据 = 引擎状态表捡漏"
								  "（引擎亲手画过 state 4/5）-> 已扫描 ⇒ 状态 {}（原版「已扫描」绿）",
							a_ref->GetFormID(), fid, FloraScannedState());
					}
					g_state.floraScannedCache.emplace(a_ref,
						State::FloraScanRec{ true, now, fid, 5 });
					return true;
				}
			}

			// ① ★★★ v4.28 主判据：引擎自己的「这个引用扫没扫过」
			//   `GetOutlineState(ref)`（1 = 未扫描 / 2 = 已扫描；引擎的 `IsScanned`
			//   就是它 == 2，证据链见常量区）。按引用回答 ⇒ 植物也准。
			std::uint8_t engineState = 0;  // 0 = 没问 / 拿不到
			if (engineStateOn && a_ref) {
				engineState = g_scannableOutlineState(const_cast<RE::TESObjectREFR*>(a_ref));
				++g_state.floraEngineStateQueries;
				if (engineState == kScannableStateScanned) {
					++g_state.floraEngineStateScanned;
				} else if (engineState == 1) {
					++g_state.floraEngineStateUnscanned;
				} else {
					// ★★★ v5.1.1：引擎「不知道」（反汇编 0x1306E80 里有这条路径：
					//   组件缺失 + 玩家知识库查不到 ⇒ 返回 **0**）。实测占约 20%，
					//   老日志里这一档是「问=833 已扫描=7 未扫描=655」那 171 个差额。
					//   它只能当「没有权威答案」，绝不能被当成「已扫描」反推。
					++g_state.floraEngineStateUnknown;
				}
				if (engineState == kScannableStateScanned) {
					// ★ R12：同 ⓪.5 的冲突诊断（直读说未满、引擎答 2）
					if (progSaidPartial) {
						++g_state.floraProgConflict;
						if (!g_state.floraProgConflictWarned) {
							g_state.floraProgConflictWarned = true;
							REX::WARN("flora progress: ★ 冲突 —— 直读答「未满」而 GetOutlineState(ref)=2"
									  "（ref=0x{:X} base=0x{:X}）-> 诊断计数 `冲突=`；不影响行为",
								a_ref->GetFormID(), fid);
						}
					}
					++g_state.floraScanHits;
					// ★★★ v4.31 / v5.1：写进**按引用的单向记忆** —— 之后**这个引用**
					//   都直接走 ⓪（不再重问引擎）。这是「低概率变青」的根治：
					//   30 秒后 TTL 过期重问时，即使引擎这次答 0/1，也不会翻案。
					//   ★ v5.1：只记这个引用，同 base 的其它实例照常按各自的判据走。
					RememberFloraRef(a_ref, a_base, true, "引擎状态 GetOutlineState(ref)=2（原生 IsScanned）");
					g_state.floraScannedCache.emplace(a_ref, State::FloraScanRec{ true, now, fid, 1 });
					if (g_cfg.floraScanProbeMax > 0 &&
						g_state.floraScanGreenProbes < static_cast<std::uint32_t>(g_cfg.floraScanProbeMax)) {
						++g_state.floraScanGreenProbes;
						REX::INFO("flora scan: ref=0x{:X} base=0x{:X} 判据 = 引擎自己的扫描状态 "
								  "`GetOutlineState(ref)`=2（= 原生 IsScanned）-> 已扫描 ⇒ 状态 {}（原版「已扫描」绿）",
							a_ref->GetFormID(), fid, FloraScannedState());
					}
					return true;
				}
			}

			// ② 资源链（★ 只对「产出物品是 LVLI」的 FLOR 成立；植物不会走这里）
			FloraChain chain{};
			const bool scanned = chainOn && QueryFloraResourceScanned(a_base, &chain);
			if (chainOn) {
				++g_state.floraScanQueries;
				if (scanned) {
					++g_state.floraScanHits;
					// ★★★ v4.31 / v5.1：同样写进**按引用的**单向记忆
					//   （资源进了勘测数据 = 单向；粒度收窄到这一次问的那个引用）
					RememberFloraRef(a_ref, a_base, true,
						"资源链命中（产出物品是 LVLI 的矿脉 / 气泉 / 液池）");
				}
				if (!chain.shapeOk) {
					++g_state.floraScanShapeFails;
					// 前几个失败的 base 打「指针窗口」（这是定位偏移的唯一硬证据）
					if (g_state.floraScanDumps < kFloraProbeBases) {
						++g_state.floraScanDumps;
						FloraDumpWindow(a_base);
					}
				}
			}

			// ★★★ v4.31 / v4.33：**没有权威答案就沿用旧结论**（不是一律降成「未扫描」）。
			//   权威答案 = 引擎状态 == 2（上面已 return）∨ 资源链**走通且适用**
			//   （shapeOk 且**不是**「产出是 MISC 的植物」—— 那一段链对植物根本不用，
			//   它的 shapeOk 恰好 = true 但给不出任何结论）。
			//   ★★★ v4.33 修正：v4.31 写的判据是 `!chain.shapeOk`，而植物
			//     （produceIsMisc）恰好会把 shapeOk 置 true ⇒ 这个条件对植物**永远
			//     不成立** ⇒ 「沿用」从未保护过植物 —— 这正是「低概率变青」在
			//     v4.31 之后依然复现的原因之一。
			bool              result          = scanned;
			const bool        noAuthoritative = chain.produceIsMisc || !chain.shapeOk;
			//   ★★★ v5.2：「沿用旧结论」也归记忆层管（默认关闭）—— 新口径下每一轮
			//     都由引擎数据（ⓠ / ⓪.5 / ① / ②）当场回答；TTL 内（30s / 5s）的
			//     `floraScannedCache` 缓存仍然生效（那是性能缓存，不是语义记忆）。
			if (g_cfg.floraUseMemory && noAuthoritative && hasPrev && prevScanned) {
				result = true;
				++g_state.floraStickyKeeps;
			}
			g_state.floraScannedCache.emplace(a_ref, State::FloraScanRec{ result, now, fid, 0 });

			if (g_cfg.floraScanProbeMax > 0 &&
				g_state.floraScanProbes < static_cast<std::uint32_t>(g_cfg.floraScanProbeMax)) {
				++g_state.floraScanProbes;
				char res[96];
				std::size_t p = 0;
				for (std::uint32_t i = 0; i < chain.irESCount && p + 16 < sizeof(res); ++i) {
					p += static_cast<std::size_t>(std::snprintf(res + p, sizeof(res) - p, "%s0x%08X",
						i ? "," : "", chain.irES[i]));
				}
				const char* const how = !chainOn
					? "资源链判据已关 / 不可用（本行只剩引擎状态）"
					: (chain.produceIsMisc
							? "产出物品是 **MISC**（= 植物）⇒ 引擎那一段要求 LVLI，资源链不适用（判据交主判据）"
							: (chain.shapeOk ? "资源链（产出物品是 LVLI）"
											 : "资源链（链没走通，已打指针窗口取证）"));
				REX::INFO("flora scan: base=0x{:X} 产出字段[+0x{:X}]=0x{:X} misc=0x{:X} 资源数组[+0x{:X}]={}个"
						  " irES=[{}] | 引擎状态={} | {} -> 已扫描={} ⇒ 状态 {}（{}）",
					fid, chain.produceOff, chain.produce, chain.misc, chain.arrOff, chain.resSize,
					p ? res : "无", engineState, how, result ? 1 : 0,
					result ? FloraScannedState() : g_cfg.stateByCategory[static_cast<std::size_t>(Category::kFlora)],
					result ? "原版「已扫描」绿" : "原版「未扫描」青色脉冲");
			}
			return result;
		}

		// 判据缓存作废。调用点：放下扫描仪（那一局扫描的结果刚刚变了）、换场景 / 读档
		// （换了一个存档 = 勘测数据可能完全不同）。
		void InvalidateFloraScannedCache(const char* a_reason)
		{
			// ★★★ v5.2：引擎扫描进度缓存也在这里一起清（进度是**存档级**数据：
			//   读档 / 切场景后可能完全不同；放下扫描仪时清掉只是多查几次，无副作用）。
			InvalidateFloraProgressCache(a_reason);
			if (g_state.floraScannedCache.empty()) {
				return;
			}
			const auto n = g_state.floraScannedCache.size();
			g_state.floraScannedCache.clear();
			REX::INFO("flora scanned: 判据缓存作废（{}，清了 {} 条）-> 下一轮重新问引擎", a_reason, n);
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

			// ★★★ v5.0：自建颜色通道的两个前置（见 docs/32）——
			//   ① 管理器 ctor `0x6532F0`：按首 16 字节签名核对；
			//   ② 「挂上」visitor vtable `0x4B2FA10`：+0x08 / +0x10 两个槽必须都指向
			//      回调 `0x653FC0`（= `add rcx,8; jmp 0x653DD0`）。
			//   （摘除 visitor vtable `0x4B2F9F0` 已在上面校验过。）
			//   两者任一不可用 ⇒ ChannelMode 自动回退旧路径（只少一种模式，不影响功能）。
			const auto addrMgrCtor      = base + kRvaChannelMgrCtor;
			const auto addrMountVisorVt = base + kRvaChannelMountVisor;
			bool       mountVisorOk     = false;
			if (IsReadable(reinterpret_cast<const void*>(addrMountVisorVt), 0x18)) {
				const auto* vt = reinterpret_cast<const std::uintptr_t*>(addrMountVisorVt);
				mountVisorOk = (vt[1] == base + kRvaChannelMountCb) &&
				               (vt[2] == base + kRvaChannelMountCb);
			}
			const bool ctorOk = SigMatches(addrMgrCtor, kSigChannelMgrCtor);
			if (ctorOk && mountVisorOk) {
				g_channelMgrCtor      = reinterpret_cast<ChannelMgrCtor_t>(addrMgrCtor);
				g_channelMountVisorVt = addrMountVisorVt;
				REX::INFO("channel: 自建颜色通道就绪 —— mgrCtor=+0x{:X} mountVtbl=+0x{:X}（sig verified）",
					kRvaChannelMgrCtor, kRvaChannelMountVisor);
			} else {
				g_channelMgrCtor = nullptr;
				REX::WARN("channel: 自建颜色通道不可用（mgrCtor={} mountVtbl={}）"
						  "-> ChannelMode 自动回退旧路径（state 覆盖）",
					ctorOk ? "ok" : "sig-mismatch",
					mountVisorOk ? "ok" : "bad-vtbl");
			}
			// ★★★ v4.25：星球目标「已扫描」判据用的那个引擎函数
			//   （0x1597A50 =「这个 BGSResource 已经扫描过（进了勘测数据）？」）。
			//   它被引擎自己的扫描求值函数 `0x159ED90` 调用（`0x159F54E`），因此语义
			//   与原版**逐字节一致**——比我们自己猜「扫描状态存在哪」可靠得多。
			//   ★ 拿不到只是「星球目标不再区分扫描前后」（退回 v4.24 行为），
			//     不该影响高亮本身，所以单独校验、单独降级。
			const auto addrScanned = base + kRvaIsResourceScanned;
			if (SigMatches(addrScanned, kSigIsResourceScanned)) {
				g_isResourceScanned      = reinterpret_cast<IsResourceScanned_t>(addrScanned);
				g_isResourceScannedReady = true;
				REX::INFO("flora scanned: 引擎判据就绪（RVA 0x{:X}，签名核对通过）-> 放下扫描仪后，"
						  "**已扫描过**的矿石 / 气体 / 液体 / 植物用状态 {}（原版绿 #27C684），"
						  "没扫描过的用状态 {}（原版青色脉冲）",
					kRvaIsResourceScanned,
					g_cfg.stateFloraScanned,
					static_cast<int>(g_cfg.stateByCategory[static_cast<std::size_t>(Category::kFlora)]));
			} else {
				g_isResourceScannedReady = false;
				REX::WARN("flora scanned: RVA 0x{:X} 签名不匹配（游戏版本变了？）-> "
						  "资源链判据不可用（「已扫描的星球目标」改用主判据 / 退回与未扫描同色）",
					kRvaIsResourceScanned);
			}

			// ★★★ v4.28：**主判据** —— 引擎自己的 `ScannableComponent::GetOutlineState(ref)`
			//   （1 = 未扫描 / 2 = 已扫描；引擎的原生 `IsScanned` 就是它 == 2，
			//    证据链见常量区 kRvaScannableOutlineState 那一段）。
			//   它是**按引用**回答的（内部解析 species / resource），所以植物
			//   （产出物品是 MISC、资源链不适用）也准 —— v4.27 的「植物没扫就绿」
			//   就是只用了资源链造成的。
			const auto addrOutlineState = base + kRvaScannableOutlineState;
			if (SigMatches(addrOutlineState, kSigScannableOutlineState)) {
				g_scannableOutlineState      = reinterpret_cast<GetOutlineState_t>(addrOutlineState);
				g_scannableOutlineStateReady = true;
				REX::INFO("flora scanned: 主判据就绪 —— ScannableComponent::GetOutlineState(ref)"
						  "（RVA 0x{:X}，签名核对通过；1 = 未扫描 / 2 = 已扫描 = 引擎原生 IsScanned）"
						  "-> 放下扫描仪后：已扫描的星球目标（植物 / 矿石 / 气体 / 液体）用状态 {}"
						  "（原版绿 #27C684），没扫描过的用状态 {}（原版青色脉冲）",
					kRvaScannableOutlineState,
					g_cfg.stateFloraScanned,
					static_cast<int>(g_cfg.stateByCategory[static_cast<std::size_t>(Category::kFlora)]));
			} else {
				g_scannableOutlineStateReady = false;
				REX::WARN("flora scanned: 主判据 RVA 0x{:X} 签名不匹配（游戏版本变了？）-> "
						  "退回只用资源链（植物会偏绿，= v4.27 行为，不影响其它功能）",
					kRvaScannableOutlineState);
			}

			LogManagerDiagnostics("install");
			LogManagerMapDiag("install", PrimaryState());
			// ★ v4.0.1：把 11 个状态的实际配色打出来（分类分色出问题时的第一手证据）
			LogOutlineColors("install");

			// ★★★ v4.18：**配色覆盖必须在任何 HighlightManager 被创建之前写进表** ——
			//   反汇编 `0x17D47B0` 实证：管理器只在**创建**那一刻从 mgr 表算出颜色参数
			//   交给渲染侧（`0x6532F0` + `0x653850`）；对**已存在**的管理器，
			//   `mov eax,[mgr+0x28]; cmp eax,[mgr+0x30]; je <跳过>` —— 两个版本字段相等
			//   就整段跳过，永远不重算颜色。
			//   install 阶段管理器是 0/11（上面那行日志可证）⇒ 在这里把表写好，
			//   之后无论引擎（举扫描仪）还是我们（autoEnsure）创建管理器，用的都是
			//   覆盖后的颜色 —— 这是「颜色没区别」问题的正解。
			//   （world-ready 还会再写一次，兜住「引擎中途改表」；两次都是幂等写。）
			{
				const auto applied = WriteColorOverrides("install");
				REX::INFO("outline colors[install]: 已在管理器创建之前写入 {} 个状态的覆盖色"
						  "（管理器 alive={}/{} —— 若为 0 则后续创建的管理器直接用这些颜色）",
					applied, CountLiveManagers(), kOutlineManagerUsed);
			}
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
		// ★ v4.0.1 / ★★ v4.19 订正：每状态「配色块」（诊断 + 覆盖）
		// --------------------------------------------------------------------
		// 每状态一个 0xA0 的块，块基址 = `kRvaOutlineParams`（0x5919A88），字段见常量区：
		//   +0x00 / +0x20 = 脉冲 High / Low —— `0x17D47B0` 建/刷管理器时读；
		//   +0x40 = float 插值除数、+0x60 = float（都原样透传给渲染侧）；
		//   +0x80 = ★★ 描边基色 —— 挂引用（`0x17D4CD0`）时读的就是它，
		//           引擎交给渲染侧的 32 字节参数块的 +0x00 也是它 ⇒ **画出来的是它**。
		//   所以「每个状态本来就是不同颜色」，本 MOD 只需要把类别映射到不同的状态
		//   （或进一步用 INI 的 `ColorXxx` 覆盖成想要的颜色）。
		// ★ 老版本写/读的 0x591E088 = 块基址 + 112×0xA0，那是**另一片 float 常量区**
		//   （v4.19 订正：以前是「写错表」，既没效果又污染常量；见 docs/18）。
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
			auto* tab = OutlineParamTable(kRvaOutlineParams);
			if (!tab) {
				REX::WARN("outline colors[{}]: 配色块不可读（RVA 0x{:X} 在这个版本上变了？）",
					a_tag, kRvaOutlineParams);
				return;
			}
			// dword 的布局是 **`0xAARRGGBB`**（★ v4.17 静态初始化函数实证：
			// state 0 写进去的就是 0xFF3EADF2 = 青 + 不透明；截图也对得上）。
			const auto rgbOf = [](std::uint32_t v) {
				return static_cast<unsigned>(((v >> 16) & 0xFFu) << 16 | ((v >> 8) & 0xFFu) << 8 | (v & 0xFFu));
			};
			// ★★ v4.20：加打 `a=`（alpha 字节）—— 覆盖不透明度到底写没写进去，
			//   看这一列即可（用户反馈「覆盖太深」时就是靠它核对）。
			for (std::uint32_t i = 0; i < kOutlineManagerUsed; ++i) {
				const auto rd = [tab, i](std::size_t a_off) {
					return *reinterpret_cast<const std::uint32_t*>(tab + i * kOutlineParamStride + a_off);
				};
				const auto rf = rd(kOffStateBaseColor);
				const auto hi = rd(kOffStatePulseHigh);
				const auto lo = rd(kOffStatePulseLow);
				char buf[224];
				std::snprintf(buf, sizeof(buf),
					"  state=%2u %-18s 基色=#%06X a=%02X (%3u,%3u,%3u) 脉冲High=#%06X a=%02X 脉冲Low=#%06X a=%02X",
					i, kOutlineStateName[i], rgbOf(rf), (rf >> 24) & 0xFFu,
					(rf >> 16) & 0xFFu, (rf >> 8) & 0xFFu, rf & 0xFFu,
					rgbOf(hi), (hi >> 24) & 0xFFu, rgbOf(lo), (lo >> 24) & 0xFFu);
				REX::INFO("outline colors[{}]: {}", a_tag, buf);
			}
		}

		// ====================================================================
		// ★★ v4.19 诊断：读「渲染侧」每状态实际收到的 32 字节参数块
		// --------------------------------------------------------------------
		// 依据（反汇编 `0x653850`「把参数块交给渲染侧」，见 docs/18）：
		//     rax = [rip+0x5321979]（RVA 0x59751E8）= 高亮宿主对象
		//     rsi = [rax+0xE8]           = 高亮管理器集合
		//     [rsi+0x2C8] = u32 数组：managerID → 槽号
		//     [rsi+0x3C8] = 槽数组：**每槽 32 字节**（= `0x17D47B0` 算出来的参数块）
		//     managerID   = `[mgr+0x40] & 0xFFFFFF`
		// 参数块布局（`0x17D47B0` 里那段打包）：
		//     +0x00 dword = **描边基色**（= 配色块 +0x80，也就是画出来的颜色）
		//     +0x04 dword = 0
		//     +0x08 dword = 脉冲色（High/Low 按相位插值后打包）
		//     +0x0C float = 配色块 +0x60（原样透传）
		//     +0x10 float = 引擎常量
		// 用途：`基色` 就是渲染器实际用的颜色 —— 覆盖生效时它应等于我们写的值；
		//       若仍是原生值 ⇒ 管理器是在写表之前建的（颜色不会变）。
		// ★ 全程只读、每一步都过 IsReadable；拿不到就记一行 WARN 并放弃（不重试）。
		// ====================================================================
		constexpr std::uintptr_t kRvaRenderHost       = 0x59751E8;
		constexpr std::size_t    kOffHostHighlightSet = 0xE8;
		constexpr std::size_t    kOffHlIdToSlot       = 0x2C8;
		constexpr std::size_t    kOffHlSlotData       = 0x3C8;
		constexpr std::size_t    kOffManagerId        = 0x40;
		constexpr std::size_t    kRenderSlotStride    = 32;
		constexpr int            kRendererProbeMax    = 3;  // 每个会话最多跑几次（限流）
		// ★★★ v4.24：管理器占用探针（见 LogManagerOccupancy）；每会话最多打几次
		constexpr std::uint32_t  kManagerOccupancyMax = 6;

		void LogRendererParams(const char* a_tag)
		{
			if (!g_cfg.rendererProbe || g_state.rendererProbeRuns >= kRendererProbeMax) {
				return;
			}
			++g_state.rendererProbeRuns;
			const auto derefPtr = [](std::uintptr_t a_addr) -> std::uintptr_t {
				if (!IsReadable(reinterpret_cast<const void*>(a_addr), sizeof(std::uintptr_t))) {
					return 0;
				}
				return *reinterpret_cast<const std::uintptr_t*>(a_addr);
			};
			const auto host  = derefPtr(ModuleBase() + kRvaRenderHost);
			const auto hl    = host ? derefPtr(host + kOffHostHighlightSet) : 0;
			const auto idTab = hl ? derefPtr(hl + kOffHlIdToSlot) : 0;
			const auto slots = hl ? derefPtr(hl + kOffHlSlotData) : 0;
			if (!host || !hl || !idTab || !slots) {
				REX::WARN("renderer params[{}]: 渲染侧指针不可读（host=0x{:X} hl=0x{:X} idTab=0x{:X} slots=0x{:X}）"
						  "-> 跳过（只是诊断，不影响高亮）",
					a_tag, host, hl, idTab, slots);
				return;
			}
			std::uint32_t logged = 0;
			for (std::uint32_t st = 0; st < kOutlineManagerUsed; ++st) {
				const auto mgr = OutlineManagerFor(st);
				if (!mgr) {
					continue;
				}
				const auto idAddr = mgr + kOffManagerId;
				if (!IsReadable(reinterpret_cast<const void*>(idAddr), 4)) {
					continue;
				}
				const auto id = *reinterpret_cast<const std::uint32_t*>(idAddr) & 0xFFFFFFu;
				if (id > 0xFFFFu) {
					continue;  // 还没分配 ID（0xFFFFFF = 未分配）
				}
				const auto idxAddr = idTab + id * 4;
				if (!IsReadable(reinterpret_cast<const void*>(idxAddr), 4)) {
					continue;
				}
				const auto idx = *reinterpret_cast<const std::uint32_t*>(idxAddr);
				if (idx > 0xFFFFFu) {
					continue;
				}
				const auto slot = slots + idx * kRenderSlotStride;
				if (!IsReadable(reinterpret_cast<const void*>(slot), kRenderSlotStride)) {
					continue;
				}
				const auto* d     = reinterpret_cast<const std::uint32_t*>(slot);
				const auto  base  = d[0];
				const auto  pulse = d[2];
				const auto  f0    = *reinterpret_cast<const float*>(&d[3]);
				const auto  f1    = *reinterpret_cast<const float*>(&d[4]);
				// ★★ v4.20：`a=` = 渲染侧实际收到的 alpha 字节（覆盖不透明度生效与否的直接证据）
				REX::INFO("renderer params[{}]: state={:2} id=0x{:X} 槽={:<4} 基色=#{:06X} a={:02X} 脉冲=#{:06X} a={:02X} f0={:.3f} f1={:.3f}",
					a_tag, st, id, idx,
					base & 0xFFFFFFu, (base >> 24) & 0xFFu,
					pulse & 0xFFFFFFu, (pulse >> 24) & 0xFFu, f0, f1);
				++logged;
			}
			if (logged == 0) {
				REX::INFO("renderer params[{}]: 管理器还没建（0/{}）-> 渲染侧暂无每状态参数",
					a_tag, kOutlineManagerUsed);
			}
		}

		// 把每个类别的颜色写进引擎的**每状态配色块**（`kRvaOutlineParams`）。
		//   ★★★ v4.19 订正（写表地址 + 覆盖字段；证据链见常量区与 docs/18）：
		//     · 以前写的是 **0x591E088** —— 那是块基址 + 112×0xA0 的 **float 常量区**
		//       ⇒ **写错表**：对描边零影响，还污染了那张常量表（v4.19 彻底改掉）。
		//     · 真正要写的是同一块里的三处：
		//         +0x00 脉冲 High ┐ 只换 RGB、alpha 原样（实测 alpha=0）
		//         +0x20 脉冲 Low  ┘ 一起改 ⇒ 脉冲相位切到 Low 时也不会闪回原生色
		//         +0x80 ★ 描边基色：换 RGB，且 **alpha = 0 时补 0xFF**
		//               （只有 7/8 这种从未初始化的槽是 0，不补就永远画不出来）
		//     · dword 布局 = `0xAARRGGBB`（静态初始化函数实证：state 0 = 0xFF3EADF2 青）。
		//   ★ 生效时机（v4.18 的教训仍然成立）：管理器**只在创建那一刻**读这块表
		//     （`0x17D47B0` 对已存在的管理器是 `cmp [mgr+0x28],[mgr+0x30]; je 跳过`），
		//     所以覆盖必须写在任何管理器创建之前 —— install 阶段（0/11）先写一次，
		//     world-ready 再写一次兜底，引擎销毁管理器后再写一次。
		//   ★ v4.19：按**状态**归并后再写 —— 同一状态被两个类别共用（容器+尸体、
		//     弹药+植物）时只写一次；若两个类别共用同一状态却给了**不同**颜色，
		//     报警并保留先出现的那个（否则会互相覆盖，表现为「颜色随机变」）。
		//   ★★ v4.20：新增「覆盖不透明度」（INI `AlphaXxx`，0 = 保留引擎原值）——
		//     被点名的三类（武器/防具/门）默认 102（≈40%），三处（High/Low/基色）
		//     一起写 ⇒ 高亮变半透明，物品本身的材质透出来（用户要求「弄浅一点」）。
		//     其余类别 alpha=0 ⇒ 与 v4.19 逐字节相同（只有 RGB 被换掉）。
		std::uint32_t WriteColorOverrides(const char* a_tag)
		{
			auto* tab = OutlineParamTable(kRvaOutlineParams);
			if (!tab) {
				return 0;
			}
			// ① 类别 → 状态 → 颜色 + 不透明度（含「同状态撞色 / 撞透明度」检测）
			std::array<std::uint32_t, kOutlineManagerUsed> stateColor{};
			stateColor.fill(kColorUnset);
			std::array<std::uint32_t, kOutlineManagerUsed> stateAlpha{};  // ★ v4.20（0 = 保留原值）
			std::array<std::string, kOutlineManagerUsed> stateWho{};
			std::uint32_t conflicts = 0;
			for (std::size_t c = 0; c < kCategoryCount; ++c) {
				const auto rgb = g_cfg.colorOverride[c];
				if (rgb == kColorUnset) {
					continue;
				}
				const auto st = static_cast<std::uint32_t>(std::clamp(g_cfg.stateByCategory[c], 0, 11));
				if (st >= kOutlineManagerUsed) {
					continue;
				}
				const auto rgb24    = rgb & 0xFFFFFFu;
				const auto alphaCfg = static_cast<std::uint32_t>(g_cfg.colorAlpha[c]);
				if (stateColor[st] == kColorUnset) {
					stateColor[st] = rgb24;
					stateAlpha[st] = alphaCfg;
					stateWho[st]   = kCategoryName[c];
				} else if (stateColor[st] != rgb24) {
					if (++conflicts <= 4) {
						REX::WARN("outline colors: 类别 {} 与 {} 共用 state={} 但颜色不同（#{:06X} 与 #{:06X}）"
								  "-> 保留 {} 的颜色；想让两者都生效，把其中一个的 StateXxx 改到别的状态",
							kCategoryName[c], stateWho[st], st, stateColor[st], rgb24, stateWho[st]);
					}
				} else {
					if (stateAlpha[st] != alphaCfg && ++conflicts <= 4) {
						REX::WARN("outline colors: 类别 {} 与 {} 共用 state={}（颜色相同）但透明度不同（{} 与 {}）"
								  "-> 保留 {} 的 {}；想让两者都生效，把其中一个的 StateXxx 改到别的状态",
							kCategoryName[c], stateWho[st], st, stateAlpha[st], alphaCfg,
							stateWho[st], stateAlpha[st]);
					}
					stateWho[st] += ',';
					stateWho[st] += kCategoryName[c];
				}
			}
			// ② 逐状态写三处（脉冲 High / 脉冲 Low / 描边基色）
			std::uint32_t applied = 0;
			for (std::uint32_t st = 0; st < kOutlineManagerUsed; ++st) {
				if (stateColor[st] == kColorUnset) {
					continue;
				}
				const auto rgb   = stateColor[st];
				const auto alpha = stateAlpha[st];  // ★ v4.20：0 = 保留引擎原值
				auto*      blk = tab + st * kOutlineParamStride;
				// ★ v4.20：脉冲两处 —— 有配置 alpha 就写它，没配置（0）就保留引擎原 alpha
				//   （= v4.19 行为，含脉冲的呼吸 alpha）。
				const auto patchChannel = [rgb, alpha](std::uint8_t* p) {
					auto v = *reinterpret_cast<std::uint32_t*>(p);
					v = (alpha != 0) ? ((alpha << 24) | rgb) : ((v & 0xFF000000u) | rgb);
					*reinterpret_cast<std::uint32_t*>(p) = v;
				};
				const auto baseBefore = *reinterpret_cast<const std::uint32_t*>(blk + kOffStateBaseColor);
				const auto hiBefore   = *reinterpret_cast<const std::uint32_t*>(blk + kOffStatePulseHigh);
				patchChannel(blk + kOffStatePulseHigh);  // 脉冲 High
				patchChannel(blk + kOffStatePulseLow);   // 脉冲 Low
				// ★★ v4.21：描边基色 —— 默认写 `RGB + alpha=0`（**不填充**：只留脉冲轮廓，
				//   物品材质透出）。`NoFill=0` 时退回 v4.19 的「alpha=0 补 0xFF（填充）」。
				//   证据/推导见 Config::noFill 的长注释 + docs/20。
				if (g_cfg.noFill) {
					*reinterpret_cast<std::uint32_t*>(blk + kOffStateBaseColor) = rgb;  // alpha=0
				} else {
					patchChannel(blk + kOffStateBaseColor);
					auto* p = blk + kOffStateBaseColor;
					auto  v = *reinterpret_cast<std::uint32_t*>(p);
					if ((v & 0xFF000000u) == 0) {  // v4.19 兜底（实心填充）
						v |= 0xFF000000u;
						*reinterpret_cast<std::uint32_t*>(p) = v;
					}
				}
				++applied;
				char alphaNote[48];
				if (alpha != 0) {
					std::snprintf(alphaNote, sizeof(alphaNote), "脉冲 %u/255（约 %u%%）",
						alpha, (alpha * 100u + 127u) / 255u);
				} else {
					std::snprintf(alphaNote, sizeof(alphaNote), "脉冲保留引擎原值");
				}
				char buf[352];
				std::snprintf(buf, sizeof(buf),
					"outline colors: state=%u 覆盖为 #%06X <- %s"
					"（描边基色原值 0x%08X -> %s；脉冲 High 原值 0x%08X；%s）",
					st, rgb, stateWho[st].c_str(), baseBefore,
					g_cfg.noFill ? "写为 #RRGGBB + alpha=0（不填充：物品材质透出）"
								 : "写为 #RRGGBB（填充模式，alpha=0 时补 0xFF）",
					hiBefore, alphaNote);
				REX::INFO("[{}] {}", a_tag, buf);
			}
			return applied;
		}

		// 写表 + 立刻让引擎用上：建/刷管理器（0x17D47B0）+ 已挂的目标整批重申。
		//   ★ 注意顺序：**先写表、后 ensure** —— 顺序反了就会「管理器带着旧颜色出生」
		//     （v4.17 的教训）。已挂的目标也必须重新 Set 一次：挂引用时读的那份
		//     颜色参数是「挂的时候读一次」（0x17D4CD0），不重挂就还是旧色。
		void ApplyColorOverrides(const char* a_tag)
		{
			// ★★★ v5.0：通道模式下**不写引擎配色块**（颜色只进自建通道的 32B 参数）。
			//   注意：`g_channelsFailed`（模板标定失败）后自动落到旧路径 —— 那时
			//   这一层会照常覆盖（功能不丢，只是回到 v4.33 行为）。
			if (g_cfg.channelMode && !g_channelsFailed) {
				REX::INFO("outline colors[{}]: ChannelMode=1 -> 跳过引擎配色覆盖"
						  "（自建通道用自己的参数块，原版配色一个字节不动）",
					a_tag);
				return;
			}
			const auto applied = WriteColorOverrides(a_tag);
			if (applied == 0) {
				return;
			}
			if (g_outlineEnsure) {
				g_outlineEnsure(nullptr);
			}
			MarkAllForReassert("颜色覆盖后重刷");
			REX::INFO("outline colors[{}]: 已覆盖 {} 个状态的颜色（管理器 alive={}/{}；已挂的已转入重挂）",
				a_tag, applied, CountLiveManagers(), kOutlineManagerUsed);
			// ★ v4.19：把「渲染侧实际收到的参数块」打出来 —— 这是「颜色到底有没有
			//   送进渲染器」的唯一直接证据（以前只能靠肉眼看画面）。
			LogRendererParams(a_tag);
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
			// ★ v4.18：先写表、再让引擎建/刷管理器（ApplyColorOverrides 内部就是这个顺序）。
			//   v4.17 在这里是反的（先 ensure、后写表）⇒ 管理器带着旧颜色出生，
			//   且 `0x17D47B0` 对已存在的管理器不再刷新 ⇒ 覆盖永远不生效。
			//   写表本身已经在 install 阶段做过一次（管理器在 0/11 时），这里是
			//   第二道保险：万一引擎中途改过表，这里会把它改回来并重挂。
			ApplyColorOverrides("world-ready");
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

		// ★★★ v4.24：11 个 HighlightManager 的「元素数」一行打全（诊断探针）。
		//   为什么需要：**引擎自己在往哪些 state 写、写了多少**是「原版扫描色落在
		//   哪个槽位」这类问题的唯一实况证据（本轮就是靠它验证「已扫描的星球目标 =
		//   state 4/5 = 绿色」这条结论；也用来复盘「扫描前 / 扫描后 / 正在扫描」）。
		//   全部只读 + 指针校验；由 INI `ManagerOccupancyProbe`（默认 1）控制，
		//   每会话 ≤ `kManagerOccupancyMax` 次，调用点见 Tick（举起扫描仪后 1.5s）。
		void LogManagerOccupancy(const char* a_tag)
		{
			char        buf[512];
			std::size_t used = 0;
			buf[0]           = '\0';
			for (std::uint32_t i = 0; i < kOutlineManagerUsed; ++i) {
				if (used + 24 >= sizeof(buf)) {
					break;
				}
				const auto  mgr     = OutlineManagerFor(i);
				const int   written = mgr
				                        ? std::snprintf(buf + used, sizeof(buf) - used, "%s%u=%u",
											  used ? " " : "", i, ManagerMapCount(mgr))
				                        : std::snprintf(buf + used, sizeof(buf) - used, "%s%u=-",
											  used ? " " : "", i);
				if (written <= 0) {
					break;
				}
				used += static_cast<std::size_t>(written);
			}
			REX::INFO("manager occupancy[{}]: {}（11 个 outline 状态各自的元素数；"
					  "引擎在写哪些槽位看这里 —— 4/5 = 已扫描的星球目标【绿】、7/8 = 未扫描【青】）",
				a_tag, buf);
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

		// ====================================================================
		// ★★★ v5.0：完全自建颜色通道（完整逆向 / 方案 / 验收见 docs/32）★★★
		// --------------------------------------------------------------------
		// 与旧路径（状态表 + 引擎配色块覆盖）完全平行的一套挂载/摘除：
		//   · 通道 = 一个自建 HighlightManager（ctor 0x6532F0）+ 一份 32 字节参数；
		//   · 挂载 = 复刻 0x17D4CD0 的「挂上」visitor（vtable 0x4B2FA10）；
		//   · 摘除 = 复刻 0x17D4CD0 开头的「摘除」visitor（vtable 0x4B2F9F0）；
		//   · **不写状态表、不碰引擎配色块** ⇒ 原版颜色 100% 不受影响。
		// ====================================================================

		// 通道 i 的最终颜色（0xRRGGBB）：
		//   ① INI `ColorXxx`（!= kColorUnset）优先；
		//   ② 否则用内置通道默认表（= v4.32 分组配色的「观感」逐项固化）。
		constexpr std::uint32_t kChannelColorDef[kCategoryCount] = {
			0x1F8EE2u,  // kLoot        杂项 —— 原版蓝
			0xFF2E2Eu,  // kLootWeapon  武器 / 投掷物 —— 红
			0xFF2E2Eu,  // kLootApparel 太空服 / 背包 / 头盔 / 服饰 —— 红（与武器同组）
			0x27C684u,  // kLootAmmoAid 弹药 / 救援 —— 原版绿
			0xB36BFFu,  // kLootNote    笔记 —— 紫
			0xB36BFFu,  // kLootResource 资源 —— 紫（与笔记同组）
			0xFF9500u,  // kContainer   容器 —— 橙
			0x27C684u,  // kDevice      设备 —— 原版绿
			0xFFFFFFu,  // kDoor        门 —— 白
			0x72E8FFu,  // kFlora       植物 / 矿脉（未扫描）—— 原版青脉冲的脉冲色
			0x1F8EE2u,  // kOther       MSTT（默认关）—— 与杂项同蓝
			0xFF9500u,  // kCorpse      尸体 —— 橙（与容器同色）
		};

		std::uint32_t ChannelColorFor(std::size_t a_ch)
		{
			if (a_ch == kChannelFloraScanned) {
				// 「植物已扫描」：INI ColorFloraScanned 优先，否则原版绿。
				return g_cfg.colorFloraScanned != kColorUnset
				         ? (g_cfg.colorFloraScanned & 0xFFFFFFu)
				         : 0x27C684u;
			}
			if (a_ch >= kCategoryCount) {
				return 0xFFFFFFu;
			}
			const auto ov = g_cfg.colorOverride[a_ch];
			return (ov != kColorUnset) ? (ov & 0xFFFFFFu) : kChannelColorDef[a_ch];
		}

		// 从引擎 state 0 的管理器读 32 字节参数作为模板（颜色之外的系数 = 引擎值）。
		bool ReadChannelParamTemplate(std::uint8_t* a_out)
		{
			const auto base = ModuleBase();
			if (!base) {
				return false;
			}
			auto* hostSlot = reinterpret_cast<void**>(base + kRvaOutlineHost);
			if (!IsReadable(hostSlot, sizeof(void*)) || !*hostSlot) {
				return false;
			}
			auto* host = *hostSlot;
			if (!IsReadable(host, kOffHostParamStore + sizeof(void*))) {
				return false;
			}
			auto* store = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(host) + kOffHostParamStore);
			if (!store || !IsReadable(store, kOffStoreData + sizeof(void*))) {
				return false;
			}
			auto* idTab = *reinterpret_cast<std::uint32_t**>(static_cast<std::uint8_t*>(store) + kOffStoreIdTab);
			auto* data  = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(store) + kOffStoreData);
			if (!idTab || !data) {
				return false;
			}
			const auto mgrAddr = OutlineManagerFor(0);  // uintptr_t（0 = 不存在）
			if (!mgrAddr || !IsReadable(reinterpret_cast<const void*>(mgrAddr), kChannelMgrBytes)) {
				return false;
			}
			const auto id = *reinterpret_cast<const std::uint32_t*>(mgrAddr + 0x40);
			if (id == 0xFFFFFFu || (id & 0xFFFFFFu) >= 0xCBF00u) {
				return false;
			}
			if (!IsReadable(idTab + id, sizeof(std::uint32_t))) {
				return false;
			}
			const auto slot = idTab[id];
			if (slot == 0) {
				return false;  // 尚未注册参数（正常不会发生：ctor 末尾就注册）
			}
			auto* const src = data + static_cast<std::uint64_t>(slot) * kChannelParamBytes;
			if (!IsReadable(src, kChannelParamBytes)) {
				return false;
			}
			std::memcpy(a_out, src, kChannelParamBytes);
			return true;
		}

		// 通道模式是否「该走通道」：INI 开 + 引擎函数都就绪 + 没判定失败。
		bool ChannelsWanted()
		{
			return g_cfg.channelMode && g_channelMgrCtor && g_channelMountVisorVt &&
			       g_outlineVisit && g_state.nativeReady && !g_channelsFailed;
		}

		// 账本（g_state.outlined）里「这条高亮挂在哪」的比较键：
		//   通道模式（已建好）= 通道号；旧路径 = state。
		//   ★ g_channelsReady 一旦为 true 就不会回退（g_channelsFailed 只在建好前发生），
		//     所以同一会话内键的语义是稳定的。
		bool ChannelLedgerMode()
		{
			return g_channelsReady;
		}
		std::uint32_t LedgerKeyOf(const Candidate& a_c)
		{
			return ChannelLedgerMode() ? a_c.channel : a_c.state;
		}
		std::uint32_t LedgerKeyOf(const OutlineEntry& a_e)
		{
			return ChannelLedgerMode() ? a_e.channel : a_e.state;
		}

		// 懒创建 13 条通道（模板就绪后一次建满；失败有重试上限与自动回退）。
		// 常量：每 200ms 一轮扫描最多重试 1 次 ⇒ 50 轮 ≈ 10 秒。
		constexpr std::uint32_t kChannelRetryWarnAt = 50;
		constexpr std::uint32_t kChannelRetryFailAt = 100;
		bool EnsureChannels()
		{
			if (g_channelsReady) {
				return true;
			}
			if (!ChannelsWanted()) {
				return false;
			}
			// 模板来源 = 引擎 state 0 的管理器（不存在就先让引擎建满，幂等）
			if (!OutlineManagerFor(0)) {
				if (g_outlineEnsure) {
					g_outlineEnsure(nullptr);
				}
				return false;  // 下一轮再试
			}
			std::uint8_t tmpl[kChannelParamBytes]{};
			if (!ReadChannelParamTemplate(tmpl)) {
				++g_channelRetries;
				if (g_channelRetries == kChannelRetryWarnAt) {
					REX::WARN("channel: 32 字节参数模板标定失败（已重试 {} 轮）—— 形状校验没过，继续等待（不挂载）",
						g_channelRetries);
				}
				if (g_channelRetries >= kChannelRetryFailAt) {
					g_channelsFailed = true;
					REX::WARN("channel: 模板标定连续失败 {} 轮 -> 自建通道不可用，**自动回退旧路径**（state 覆盖）",
						g_channelRetries);
				}
				return false;
			}
			// 逐条建：模板 + 覆盖两个颜色 dword
			std::uint32_t built = 0;
			for (std::size_t i = 0; i < kChannelCount && built == i; ++i) {
				std::uint8_t params[kChannelParamBytes];
				std::memcpy(params, tmpl, kChannelParamBytes);

				const auto rgb   = ChannelColorFor(i);
				const auto aCat  = (i < kCategoryCount) ? i : static_cast<std::size_t>(Category::kFlora);
				const auto alpha = static_cast<std::uint32_t>(g_cfg.colorAlpha[aCat]);

				// 基色：NoFill=1 写 alpha=0（不填充，物品材质透出）；否则填充（alpha=0 补 0xFF）
				auto* const baseDword = reinterpret_cast<std::uint32_t*>(params + kOffChannelBaseColor);
				*baseDword = g_cfg.noFill
				               ? rgb
				               : ((alpha != 0) ? ((alpha << 24) | rgb) : (0xFF000000u | rgb));
				// 脉冲色：alpha 配了就用配置，否则保留模板 alpha（= 引擎的呼吸透明度）
				auto* const pulseDword = reinterpret_cast<std::uint32_t*>(params + kOffChannelPulseColor);
				*pulseDword = (alpha != 0)
				                ? ((alpha << 24) | rgb)
				                : ((*pulseDword & 0xFF000000u) | rgb);

				auto* const mgr = g_channelMgrCtor(g_channelMgrMem[i], params);
				if (!mgr) {
					REX::WARN("channel: ctor 返回空（第 {} 条）-> 放弃自建通道，回退旧路径", i);
					g_channelsFailed = true;
					return false;
				}
				g_channels[i].mgr   = mgr;
				g_channels[i].mgrId = *reinterpret_cast<const std::uint32_t*>(
					static_cast<const std::uint8_t*>(mgr) + 0x40);
				++built;
			}
			g_channelsReady = true;
			// 汇总日志：一行汇总 + 一行逐通道颜色（便于 grep 核对）
			char buf[512];
			std::size_t used = 0;
			buf[0]           = '\0';
			for (std::size_t i = 0; i < kChannelCount; ++i) {
				const int n = std::snprintf(buf + used, sizeof(buf) - used, "%s%s=#%06X",
					used ? " " : "", (i == kChannelFloraScanned) ? "floraScanned" : kCategoryName[i],
					ChannelColorFor(i));
				if (n <= 0) {
					break;
				}
				used += static_cast<std::size_t>(n);
			}
			REX::INFO("channel: ✅ 已创建 {} 个自建颜色通道（模板 = 引擎 state0 参数块；"
					  "NoFill={} -> {}；不写状态表 / 不覆盖引擎配色）| colors: {}",
				kChannelCount, g_cfg.noFill,
				g_cfg.noFill ? "只留轮廓" : "实心填充", buf);
			return true;
		}

		// 挂载：复刻引擎 0x17D4CD0 的「挂上」visitor。
		//   visitor = { vtable = 0x4B2FA10, data = &{manager, manager->0x40} }
		//   ★ 回调（0x653FC0 = add rcx,8; jmp 0x653DD0）期望 rcx = visitor+8，
		//     即 data 指向的 8 字节里存的就是「data 字段的值」—— 两个字段都要照抄。
		bool OutlineRefViaChannel(RE::TESObjectREFR* a_ref, std::uint32_t a_ch)
		{
			if (!g_channelsReady || a_ch >= kChannelCount) {
				return false;
			}
			auto* const mgr = g_channels[a_ch].mgr;
			if (!mgr) {
				return false;
			}
			const auto root = RefGet3D(a_ref);
			if (!root) {
				return false;  // 3D 还没加载 ⇒ 与旧路径一致：等下一轮
			}
			struct MountData
			{
				void*         mgr;
				std::uint32_t id;
				std::uint32_t pad;
			};
			struct Visitor
			{
				void* vtbl;
				void* data;
			};
			MountData data{ mgr, g_channels[a_ch].mgrId, 0 };
			Visitor   visitor{ reinterpret_cast<void*>(g_channelMountVisorVt), &data };
			g_outlineVisit(&visitor, root.get());
			return true;
		}

		// 摘除：复刻引擎 0x17D4CD0 开头的「摘除」visitor。
		//   visitor = { vtable = 0x4B2F9F0, data = &manager }（★ 先解引用一次才是管理器）
		bool UnoutlineRefViaChannel(RE::TESObjectREFR* a_ref, std::uint32_t a_ch)
		{
			if (!g_channelsReady || !g_outlineGraphRemoveReady || a_ch >= kChannelCount) {
				return false;
			}
			if (!g_channels[a_ch].mgr) {
				return false;
			}
			const auto root = RefGet3D(a_ref);
			if (!root) {
				return false;
			}
			struct Visitor
			{
				void* vtbl;
				void* mgrSlot;  // = &g_channels[a_ch].mgr（数组固定 ⇒ 地址稳定）
			};
			Visitor visitor{ reinterpret_cast<void*>(g_outlineRemoveVisorVt), &g_channels[a_ch].mgr };
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

		// 把引用挂到（或重申到）原生 outline 的 a_state 状态。
		// ★★★ v5.0：a_ch = 自建通道号（ChannelMode=1 时用；0xFFFFFFFF = 不带通道信息）。
		bool OutlineRef(RE::TESObjectREFR* a_ref, std::uint32_t a_state, std::uint32_t a_ch = 0xFFFFFFFFu)
		{
			if (!g_state.nativeReady || !a_ref) {
				return false;
			}
			// ★★★ v5.0：通道模式 —— 走自建通道（不读状态表、不碰引擎配色块）。
			//   未就绪时返回 false（本轮不挂；下一轮 EnsureChannels 建好后再挂，
			//   通常在首次扫描的一两轮内完成）。g_channelsFailed 后自动落到旧路径。
			if (g_cfg.channelMode && !g_channelsFailed) {
				if (!EnsureChannels() || a_ch >= kChannelCount) {
					return false;
				}
				return OutlineRefViaChannel(a_ref, a_ch);
			}
			// ↓↓↓ 旧路径（ChannelMode=0，或通道自动回退后）↓↓↓
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

		// ★★★ v5.0：a_chHint = 账本里记的自建通道号（通道模式用；0xFFFFFFFF = 未知）。
		void UnoutlineRef(RE::TESObjectREFR* a_ref, std::uint32_t a_chHint = 0xFFFFFFFFu)
		{
			if (!g_state.nativeReady || !a_ref) {
				return;
			}
			// ★★★ v5.0：通道模式 —— 摘除走自建通道（不读 / 不写引擎状态表）。
			//   ① 已就绪：按 hint 摘（hint 缺失时兜底遍历全部通道 —— 罕见路径）；
			//   ② 未就绪但通道模式开着（= 从没挂过）：什么都不用做；
			//   ③ 其余（ChannelMode=0 或已回退）：落到下面的旧路径。
			if (g_channelsReady) {
				if (a_chHint < kChannelCount) {
					if (UnoutlineRefViaChannel(a_ref, a_chHint)) {
						++g_state.outlineRemoved;
					}
				} else {
					bool any = false;
					for (std::uint32_t i = 0; i < kChannelCount; ++i) {
						any = UnoutlineRefViaChannel(a_ref, i) || any;
					}
					if (any) {
						++g_state.outlineRemoved;
					}
				}
				return;
			}
			if (g_cfg.channelMode && !g_channelsFailed) {
				return;  // 通道未就绪 = 没挂过（挂载分支会等通道就绪）
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
					// ★★★ v4.25：「植物」类别的**第二个状态**（已扫描，默认 5）也要试 ——
					//   它可能不在任何类别的 StateXxx 里（用户把 StateFloraScanned 改成
					//   别的值时就可能落单），但完全可能正是这条高亮挂着的槽位。
					const auto floraScannedState = FloraScannedState();
					if (!done && floraScannedState < kOutlineManagerUsed && !tried[floraScannedState]) {
						tried[floraScannedState] = true;
						done = OutlineUnhighlightRef(a_ref, floraScannedState);
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
			auto budgetLimit = g_cfg.maxOutlineOpsPerScan > 0
			                     ? static_cast<std::uint32_t>(g_cfg.maxOutlineOpsPerScan)
			                     : 0xFFFFFFFFu;
			// ★★★ v4.29：resync（放下扫描仪 / 管理器数量下跌）之后的提速窗口 ——
			//   见常量区 kResyncBoostMs。窗口内取 min(boost, 原预算) 与原预算的较大者 ⇒
			//   「预算比 boost 还大」的配置不会被**降低**（只提速、不降速）。
			if (budgetLimit != 0xFFFFFFFFu && a_nowMs < g_state.resyncBoostUntilMs &&
				g_cfg.resyncBoostBudget > budgetLimit) {
				budgetLimit = g_cfg.resyncBoostBudget;
			}
			std::uint32_t budget = budgetLimit;
			g_state.opsThisScan  = 0;
			g_state.opsDeferred  = 0;

			// ---- 1) 标记 / 取消标记「掉队」----
			// ★ v2.2 的宽限期：目标掉出集合后**先只打一个时间戳**，到点还没回来才真正摘。
			//   走过去时目标会在集合边缘反复进出，没有这一步就是每 200ms 一轮的
			//   Set/Remove 抖动（主线程被这些调用磨出 hitch）；有了它，绝大多数
			//   「进出」都只是改一个时间戳，**零引擎调用**。
			const auto grace = static_cast<std::uint64_t>(g_cfg.unhighlightGraceMs);
			// ★★★ v4.23：举着扫描仪时，「星球扫描目标」类别的条目**既不重申也不排摘除**
			//   （原因：它们进的正是原版扫描目标的 state 槽位，而引擎此刻正在用；
			//    若照常走宽限期，~1.5 秒后 UnoutlineRef 会把管理器里那条 highlight
			//    摘掉 —— 那可能正是引擎刚给这个目标挂上的）。
			//   放下扫描仪那一刻 DetectEngineOutlineLoss → MarkAllForReassert 会把
			//   dropAt 清零并按预算整批重申（★ v4.25：重申时会按「扫没扫过」重新选状态
			//   —— 已扫描的用 StateFloraScanned，见 Rescan 里那一段），行为自洽。
			const bool yieldTargets = g_cfg.yieldTargetsWhileScanning && g_state.monocleOpen;
			for (auto& [ref, e] : g_state.outlined) {
				// ★★★ v4.25：让位改成**按类别**判断（v4.23 是按状态）—— 「植物」类别现在
				//   有两个状态（未扫描 7 / 已扫描 5），按状态判断会漏掉「已扫描」那一半，
				//   而它用的 state 5 正是引擎此刻在用的槽位（漏保护 ⇒ 宽限期一到，
				//   MOD 会把引擎刚给这个目标挂的那条 highlight 摘掉）。见 OutlineEntry::cat。
				if (yieldTargets && e.cat == static_cast<std::uint8_t>(Category::kFlora)) {
					e.dropAt = 0;
					continue;
				}
				if (chosenSet.find(ref) != chosenSet.end()) {
					e.dropAt = 0;  // 又回来了：什么都不用做
				} else if (e.dropAt == 0) {
					e.dropAt = a_nowMs + grace;
				}
			}

			// ---- 2) 到期的摘除（受预算限制）----
			{
				PhaseTimer tUnh{ &g_state.tUnhUs };
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
					UnoutlineRef(it->second.ref.get(), it->second.channel);
					it = g_state.outlined.erase(it);
				}
			}

			// 重挂后「下次重申时刻」的公共计算（2.5 步与第 3 步共用）。
			// ★ v4.0：状态不再是全局一个 —— 每个候选带自己的 state（分类分色，
			//   由 Rescan 里的 ClassifyBase + g_cfg.stateByCategory 算好）。
			const auto reassert = static_cast<std::uint64_t>(g_cfg.reassertMs);
			const auto nextMs   = reassert ? a_nowMs + reassert : UINT64_MAX;

			// ---- 2.5) ★★★ v4.29：状态变化优先（青 → 绿 / 绿 → 青）----
			//   背景（用户实测：「植物和矿石的高亮速度明显低于其他物品」）：
			//   放下扫描仪后，引擎拆表 ⇒ 全部目标转「待重申」；而「刚扫完 ⇒ 青变绿」
			//   的状态变化也排在**同一条队列**里按距离消耗预算（268 个 / 64 条每轮
			//   ⇒ 最多约 1 秒）⇒ 用户看到的就是「植物 / 矿石慢一拍才变色」。
			//   做法：把「挂着但 state 已经不对」的条目**先处理**（它们通常只有
			//   几个到几十个），让变色不排队。处理完把 reassertMs 复位成 nextMs，
			//   免得第 3 步把它再摘挂一次（那里的条件会因此不再成立）。
			{
				std::uint32_t changed = 0;
				for (auto* c : a_chosen) {
					auto it = g_state.outlined.find(c->ref);
					if (it == g_state.outlined.end() || it->second.dropAt != 0) {
						continue;  // 没挂过 / 待摘的走第 3 步（新挂 / 不复活）
					}
					if (LedgerKeyOf(it->second) == LedgerKeyOf(*c)) {
						continue;  // 挂的键一致（state / 通道）⇒ 没什么可优先的
					}
					if (budget == 0) {
						++g_state.opsDeferred;
						break;
					}
					--budget;
					++g_state.opsThisScan;
					++changed;
					UnoutlineRef(c->ref, it->second.channel);  // 先摘（否则两条通道各留一条 = 双层描边）
					if (OutlineRef(c->ref, c->state, c->channel)) {
						it->second.reassertMs = nextMs;
						it->second.state      = c->state;
						it->second.channel    = c->channel;  // ★ v5.0
						it->second.cat        = c->cat;
						it->second.last3D     = RefGet3D(c->ref).get();
					} else {
						it = g_state.outlined.erase(it);
					}
				}
				// 只在「优先换色」真的干活且量较大时留一行（免得刷日志）
				if (changed >= 8) {
					REX::INFO("native outline: 状态变化优先换色 {} 个（青→绿 / 绿→青不等队列，★ v4.29）",
						changed);
				}
			}

			// ---- 3) 新目标 / 到重申时刻的（同样受预算限制）----
			PhaseTimer tAdd{ &g_state.tAddUs };
			for (auto* c : a_chosen) {
				auto it = g_state.outlined.find(c->ref);
				if (it == g_state.outlined.end()) {
					if (budget == 0) {
						++g_state.opsDeferred;
						continue;
					}
					--budget;
					++g_state.opsThisScan;
					if (OutlineRef(c->ref, c->state, c->channel)) {
						OutlineEntry e;
						e.ref        = RE::NiPointer<RE::TESObjectREFR>{ c->ref };
						e.reassertMs = nextMs;
						e.state      = c->state;
						e.channel    = c->channel;  // ★ v5.0：通道模式用
						e.cat        = c->cat;  // ★ v4.25：让位保护按类别判断
						// ★ v4.7：记下挂的时候的 3D 根（之后只要它变了就说明 3D 被重建过 ⇒
						//   引擎侧那条登记已经丢了 ⇒ 见「4) 3D 复检」）
						e.last3D = RefGet3D(c->ref).get();
						g_state.outlined[c->ref] = std::move(e);
					}
				} else if (a_nowMs >= it->second.reassertMs || LedgerKeyOf(it->second) != LedgerKeyOf(*c)) {
					// ★★★ v4.25：`it->second.state != c->state` 这一条是**必须的** ——
					//   默认 `ReassertMs=0` 时 reassertMs 写成 UINT64_MAX（= 永不重申），
					//   若只按时间判断，「星球目标刚被扫描完 ⇒ 该从青色换成绿色」这件事
					//   就永远不会被应用（那条高亮会一直停在扫描前的状态）。
					//   ⇒ 只要「想要的 state」与「挂着的 state」不一致就立刻走一次
					//     摘 + 挂（走同一份每轮预算，不会突发）。
					if (budget == 0) {
						++g_state.opsDeferred;
						continue;
					}
					--budget;
					++g_state.opsThisScan;
					// 状态 / 通道变了：先把旧的摘掉，
					// 否则同一个引用会同时留在两个管理器里 ⇒ 两层描边。
					if (LedgerKeyOf(it->second) != LedgerKeyOf(*c)) {
						UnoutlineRef(c->ref, it->second.channel);
					}
					if (OutlineRef(c->ref, c->state, c->channel)) {
						it->second.reassertMs = nextMs;
						it->second.state      = c->state;
						it->second.channel    = c->channel;  // ★ v5.0
						it->second.cat        = c->cat;  // ★ v4.25
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
				// ★ 订正 R3：3D 复检（RefGet3D × Verify3DPerScan）耗时单列 ——
				//   它原来是混在 tAdd 里的，而 RefGet3D 是**引擎虚调用**（不是纯内存读），
				//   是 add 段 14ms 的主要候选来源（见 timing2 输出）。
				PhaseTimer t3D{ &g_state.t3DUs };
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
					if (OutlineRef(c->ref, c->state, c->channel)) {
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
				UnoutlineRef(entry.ref.get(), entry.channel);
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
			// ★★★ v4.29：开「恢复提速」窗口 —— 这段时间内每轮预算提到
			//   resyncBoostBudget（并且在 SyncNativeOutline 里把状态变化排到最前），
			//   治用户实测的「植物 / 矿石比其它物品慢一拍」。见常量区 kResyncBoostMs。
			const bool boostOn = g_cfg.resyncBoostMs > 0 && g_cfg.resyncBoostBudget > 0;
			if (boostOn) {
				g_state.resyncBoostUntilMs = NowMs() + g_cfg.resyncBoostMs;
			}
			REX::INFO("native outline: 引擎侧高亮疑似丢失（{}）-> {} 个已挂目标转入「待重申」"
					  "（按每轮 {} 条预算重挂，累计 {} 次{}）",
				a_reason, touched, g_cfg.maxOutlineOpsPerScan,
				g_state.outlineResyncs,
				boostOn ? "；未来 " + std::to_string(g_cfg.resyncBoostMs) + "ms 内提速到每轮 " +
							  std::to_string(g_cfg.resyncBoostBudget) +
							  " 条 + 状态变化优先（ResyncBoostMs / ResyncBoostBudget）"
						: std::string{});
		}

		void DetectEngineOutlineLoss(std::uint64_t a_nowMs)
		{
			// MonocleMenu 的开/关状态**每帧都跟踪**（即使功能关着），
			// 这样开关功能不会造成一次假的「由开变关」。
			const bool monocleOpen = IsMonocleMenuOpen();
			const bool justClosed  = g_state.monocleOpen && !monocleOpen;
			const bool justOpened  = !g_state.monocleOpen && monocleOpen;
			g_state.monocleOpen    = monocleOpen;

			// ★★★ v4.25：**放下扫描仪的那一刻**把「星球目标是否已扫描」的判据缓存作废 ——
			//   这一局很可能刚刚扫成了某个矿脉 / 植物（这正是用户要的「扫完就变绿」），
			//   不请缓存的话最多要等 kFloraScanCacheTtlMs（2 秒）才换色。
			//   放在所有早退之前：即使功能关着也照样跟踪（免得开局时留下陈旧判据）。
			if (justClosed) {
				InvalidateFloraScannedCache("放下扫描仪");
			}

			// ★★★ v4.24：刚举起扫描仪 ⇒ 1.5 秒后打一行「管理器占用」快照
			//   （那一刻引擎的求值循环已经写过至少一轮，11 个槽位里谁有货一目了然；
			//    见 LogManagerOccupancy。每会话限流 kManagerOccupancyMax 次。）
			if (justOpened && g_cfg.managerOccupancyProbe && g_state.manDumpRuns < kManagerOccupancyMax) {
				g_state.manDumpPending = true;
				g_state.manDumpAtMs    = a_nowMs + 1500;
			}

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
				// ★ v4.18：引擎刚销毁过管理器（0x17D4B30 = 销毁 11 个 + 清空状态表，
				//   典型触发点：用完原版扫描仪、UI/相机大重建）。管理器**只在创建那一刻**
				//   读配色表（见 WriteColorOverrides 的长注释），所以趁它们还没被重建，
				//   把覆盖色重新写进表 —— 这样随后 EnsureManagerFor 建出来的管理器
				//   带的就是我们的颜色（否则会退回原生色）。
				//   只写表、不 ensure：ensure 会由重挂路径的 EnsureManagerFor 触发。
				WriteColorOverrides("post-clear");
			}
			g_state.lastLiveManagers = live;
		}

		// ====================================================================
		// 扫描
		// ====================================================================
		// ★★★ 订正 R4：单轮扫描的「墙钟 vs 线程 CPU」计量 + 最慢一轮快照。
		//   为什么需要它（本轮排查的核心）：
		//     · 静态场景（站着不动）：1538 个引用 → loop ≈ 7ms；
		//     · 动态场景（走动 / 战斗）：1545 个引用 → loop ≈ 63ms。
		//   遍历量、判空次数都一样 ⇒ 只能说明**单次操作的墙钟成本**变了。
		//   于是必须能把「这几十毫秒」拆成两类：
		//     ① 我们的指令 / 内核调用真的在烧 CPU（CPU ≈ 墙钟）；
		//     ② 线程被抢占 / 等内存（页错误）/ 卡在内核（CPU ≪ 墙钟）。
		//   判据 = `GetThreadTimes` 的线程 CPU 时间（`scan … cpu=…`；`卡顿` 见 State）。
		//   ★ 作用域结束（含所有 early return）自动结算；只有真的走过遍历的轮
		//     （refs > 0）才进「分段平均 / 最慢一轮」——早退轮会把平均稀释掉。
		struct ScanProfile
		{
			std::uint64_t wall0Us  = 0;
			std::uint64_t cpu0Us   = 0;
			std::uint64_t walk0Us  = 0;
			std::uint64_t loot0Us  = 0;
			std::uint64_t cls0Us   = 0;
			std::uint64_t flora0Us = 0;
			std::uint64_t stallUs0 = 0;
			std::uint32_t stall0   = 0;
			std::uint32_t refs0    = 0;

			ScanProfile()
			{
				wall0Us  = NowUs();
				cpu0Us   = ThreadCpuUs();
				walk0Us  = g_state.tWalkUs;
				loot0Us  = g_state.tLootUs;
				cls0Us   = g_state.tClassifyUs;
				flora0Us = g_state.tFloraUs;
				stallUs0 = g_state.stallUs;
				stall0   = g_state.stallCount;
				refs0    = static_cast<std::uint32_t>(g_state.refsWalkCur + g_state.refsWalkRing);
			}

			~ScanProfile()
			{
				if (wall0Us == 0) {
					return;  // 已结算过
				}
				const auto wall = NowUs() - wall0Us;
				wall0Us         = 0;
				const auto cpu  = ThreadCpuUs() - cpu0Us;
				const auto refs = static_cast<std::uint32_t>(
					(g_state.refsWalkCur + g_state.refsWalkRing) - refs0);
				++g_state.scanMsSamples;
				g_state.scanMsTotal += wall;
				if (wall > g_state.scanMsMax) {
					g_state.scanMsMax = wall;
				}
				if (refs == 0) {
					return;  // 早退轮（形状没过 / 还没稳定 / 静置期…）：单列，不污染平均
				}
				++g_state.scanFullSamples;
				g_state.scanRefsLast = refs;
				g_state.worstScanRefsSum += refs;  // 诊断：完成轮的平均遍历量
				if (wall > g_state.worstScanUs) {
					g_state.worstScanUs       = wall;
					g_state.worstCpuUs        = cpu;
					g_state.worstRefs         = refs;
					g_state.worstWalkUs       = g_state.tWalkUs - walk0Us;
					g_state.worstLootUs       = g_state.tLootUs - loot0Us;
					g_state.worstClassifyUs   = g_state.tClassifyUs - cls0Us;
					g_state.worstFloraUs      = g_state.tFloraUs - flora0Us;
					g_state.worstStallUs      = g_state.stallUs - stallUs0;
					g_state.worstStalls       = g_state.stallCount - stall0;
				}
			}

			ScanProfile(const ScanProfile&) = delete;
			ScanProfile& operator=(const ScanProfile&) = delete;
		};

		void Rescan(std::uint64_t a_nowMs, RE::PlayerCharacter* a_player)
		{
			ScanProfile profile;  // ★ 订正 R4：本轮计量（作用域结束自动结算）
			// ★★★ v4.23：「让位原版」只针对**星球扫描目标**（见 Config::yieldTargets-
			//   WhileScanning 的长注释）—— 判断放在类别循环里（下面 `cat` 处），
			//   因为其它类别必须在举着扫描仪时照常重申（否则引擎建 HUD 时清掉管理器，
			//   「圆圈外也高亮」就断了）。这里只做「当前是否在让位窗口里」的快照。
			const bool yieldTargets = g_cfg.yieldTargetsWhileScanning && g_state.monocleOpen;
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
				// ★★★ 订正 R3（分片）：本片段在引用数组里的起点与长度。
				//   当前 cell 恒为 begin=0 / count=全量（玩家所在处必须每轮即时响应）；
				//   环内 cell 每轮只取「一片」（begin=sliceCursor、count=片大小），
				//   遍历总量从 kRingRefsCap(60000) 降到 ≈ 总量 / RingSliceMaxRounds。
				std::uint32_t             begin = 0;
				std::uint32_t             count = 0;
			};
			static std::array<ScanCell, 1 + kRingCellMax> scanCells;
			std::size_t                                    cellCount = 0;

			RawArray    arr{};
			std::size_t usedOff = 0;
			{
				// ★ v2.3：这一段就是实测里那个「什么都没干却花 31ms」的元凶，
				//   单独计时 + 配合 g_vqCalls（真的问了几次内核）一起看。
				PhaseTimer tShape{ &g_state.tShapeUs, &g_state.tShapeMaxUs };

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
					0,  // ★ 订正 R3：当前 cell 不分片（玩家所在处每轮全扫）
					std::min<std::uint32_t>(arr.size, kMaxRefsSanity) };
				// 当前 cell 进环（= 玩家真的在这里；「连续过渡」判据与「边界对面也扫」都靠它）
				RingTouch(cell, a_nowMs, arr.size);
			}

			// ---- ★ v4.7：环里其它 cell（形状 + 自洽性都过才一起扫）----
			//   ★★★ 2026-09-27（订正 R3，性能）：环内 cell **分片遍历** ——
			//     背景：kRingRefsCap = 60000（环里各 cell 引用数合计上限），而 loop 段
			//     是**每轮全量遍历**（+ 分类 + 距离，纯内存读）⇒ 用户当轮日志里
			//     `loop avg=55ms`（200ms 一轮 = 主线程占用 ~28%）几乎全在这里 ——
			//     6 万引用 × ~0.9µs（cache miss 主导）≈ 54ms，数字完全对上。
			//     治法：每个环内 cell 的引用数组**均分成 ≤ RingSliceMaxRounds 片**，
			//     每轮只遍历**一片**（游标轮转）⇒ 每轮环内遍历量 ≈ 环总量 / N
			//     （默认 6 万/5 ≈ 1.2 万，开销降到 ~10ms 量级）。
			//   ★ 观感安全性的两条硬约束（都在 LoadConfig 里钳死）：
			//     ① 分片周期（= N × ScanIntervalMs）**必须 < UnhighlightGraceMs** ——
			//        否则「本轮没扫到」的目标会在宽限期到期时被 SyncNativeOutline 摘掉、
			//        下一片再挂回来（闪烁）。默认 5 × 200ms = 1000ms < 1500ms ✓；
			//     ② 小 cell（≤ kRingSliceMinRefs）不分片 —— 单 cell 场景行为与旧版一致。
			//   ★ 不变量：环内 cell 的「当前 cell」永远不在这里（上面已 scanCells[0]）。
			{
				const std::uint32_t sliceRounds = g_cfg.ringSliceRoundsEff;
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

					// ---- 分片（见上方长注释）----
					const std::uint32_t total = std::min<std::uint32_t>(ringArr.size, kMaxRefsSanity);
					std::uint32_t       begin = 0;
					std::uint32_t       take  = total;
					if (sliceRounds > 1 && total > kRingSliceMinRefs) {
						const std::uint32_t slice = (total + sliceRounds - 1) / sliceRounds;  // 向上取整
						if (rc.sliceCursor >= total) {
							rc.sliceCursor = 0;  // 数组缩过 / 防御：游标越界就从头来
						}
						begin = rc.sliceCursor;
						take  = std::min(slice, total - begin);
						// 下一片起点（扫到尾巴就回到数组头 —— 分片是环形的）
						rc.sliceCursor = (begin + take >= total) ? 0 : (begin + take);
						g_state.ringSliceDeferred += total - take;
					} else {
						rc.sliceCursor = 0;  // 小 cell / 分片关：每轮全扫
					}
					scanCells[cellCount++] = ScanCell{ rc.cell,
						reinterpret_cast<RE::TESObjectREFR* const*>(ringArr.data),
						begin, take };
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
				PhaseTimer tLoop{ &g_state.tLoopUs, &g_state.tLoopMaxUs };
				// ★ 订正 R3：loop 内「遍历引用 + 分类 + 距离」整段（**含**下面
				//   tLoot / tFlora 两个子项；配合 refsWalkCur/Ring 判断是否遍历量问题）。
				PhaseTimer tWalk{ &g_state.tWalkUs };

			// ★ v4.7：对「当前 cell + 环内 cell」逐个遍历（见上面 scanCells 的说明）。
			//   每个 cell 只认**它自己**的引用（`ref->parentCell == sc.cell`）：
			//   这条既是最强的自洽判据，也顺带把「读到别的 cell 的引用」挡在外面。
			for (std::size_t ci = 0; ci < cellCount; ++ci) {
			const auto& sc = scanCells[ci];
			auto* const*        list  = sc.list;
			const std::uint32_t begin = sc.begin;  // ★ 订正 R3：环内 cell 的分片起点
			const std::uint32_t count = sc.count;
			// ★ 订正 R3：窗口内「实际遍历的引用数」分两桶（当前 cell / 环内分片）——
			//   它就是 loop 耗时的分母；用户再报「帧数下降」时先看这两个数。
			if (ci == 0) {
				g_state.refsWalkCur += count;
			} else {
				g_state.refsWalkRing += count;
			}

			// ★ v4.2：库存列表偏移标定（**每会话一次**，只拿容器当样本）。
			//   放在这里是因为它要顺序扫引用找样本；标定完成后这一段不再执行，
			//   热路径里一次 VirtualQuery 都不会有。
			if (!g_state.invCalibDone && g_cfg.skipEmptyLoot) {
				CalibrateInventory(list + begin, count);
			}

			// ★★ v4.17：「资源」关键词判据的启动自检（只做一次；通过/失败后都不再执行）。
			//   数据没就绪时直接返回，下一轮扫描再来。
			if (g_state.resKeywordTest == 0 && g_cfg.resourceByKeyword) {
				ResourceKeywordSelfTest();
			}

			g_state.stallCheckUs = NowUs();  // ★ 订正 R4：卡顿检测的起点
			for (std::uint32_t j = 0; j < count; ++j) {
				// ★★★ 订正 R4：**卡顿检测** —— 每 64 个引用取一次时刻；相邻两次的间隔
				//   ≥ kStallThresholdUs(3ms) 就记一次。用途：把「我们的指令慢」与
				//   「主线程被抢占 / 等内存（页错误）／卡在内核」分开 ——
				//   若「卡顿合计」占了 walk 的大头，就不再是优化代码能解决的事。
				if ((j & 63u) == 0u) {
					const auto ts  = NowUs();
					const auto gap = ts - g_state.stallCheckUs;
					if (gap >= kStallThresholdUs) {
						++g_state.stallCount;
						g_state.stallUs += gap;
						if (gap > g_state.stallMaxUs) {
							g_state.stallMaxUs = gap;
						}
					}
					g_state.stallCheckUs = ts;
				}
				auto* ref = list[begin + j];
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
				bool       isCorpse = false;
				const auto tc0      = NowUs();  // ★ 订正 R4：分类耗时单列（含 ACHR 判决探针）
				const int  cat      = ClassifyRef(ref, base, d2, isCorpse);
				g_state.tClassifyUs += NowUs() - tc0;
				++g_state.cntClassify;
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
				// ★★★ v4.23：举着扫描仪时，**星球扫描目标（「植物」= FLOR）让位原版** ——
				//   它们用的是 state 7（原版 `TargetScannable`），引擎此刻正在往这个
				//   槽位写「扫描前 / 扫描后 / 正在扫描」；MOD 一重申就把原版色盖掉
				//   （用户实测：「扫描前和扫描后颜色无法区分」/「颜色完全不显示」）。
				//   让位 = 不进候选集合（因而既不新挂、也不重申、也不会被排摘除 ——
				//   摘除保护见 SyncNativeOutline 第 1 步里同一开关）。
				if (yieldTargets && cat == static_cast<int>(Category::kFlora)) {
					++g_state.skipYield;
					// ★★★ v4.26 / v5.1：让位的同时**顺手把引擎写的结果记下来** ——
					//   引擎此刻正在给这一类目标写 state 4/5（已扫描，绿）或 7/8（未扫描，青）；
					//   读一眼它写的是什么，就能知道「**这个引用**扫没扫过」
					//   （见 ProbeFloraEngineState；★ v5.1：只读走树，零副作用）。
					const auto tf0 = NowUs();  // ★ 订正 R4：计入 tFlora（µs）
					ProbeFloraEngineState(ref, base);
					g_state.tFloraUs += NowUs() - tf0;
					++g_state.cntFlora;
					continue;
				}
				// ★ v4.2：容器 / 尸体「搜空即熄灭」——库存为空就不进候选集合。
				//   掉出候选之后由 SyncNativeOutline 的宽限期（UnhighlightGraceMs）
				//   在约 1.5 秒内把描边摘掉 ⇒ 观感是「刚搜完就灭」。
				//   判空是纯内存读；读不到 / 形状不对 ⇒ 未知 ⇒ 按「有东西」处理（照常亮）。
				//
				//   ★★ v4.9 / v4.10：展示柜（武器箱 / 武器架 / 头盔架…）是例外 ——
				//     它的内容只在搜刮界面打开期间以 `kTemporary` 条目投影进库存，
				//     关着时读到 size=0（用户实测：「关着不亮、一打开才亮」）。
				//     ⇒ v4.9：这类容器**不因读到「空」而熄灭**，否则关着的武器箱
				//       永远不亮；
				//     ⇒ v4.10：但「拿空后」必须熄灭（用户实测：「拿空了却不会熄灭」）
				//       —— 判据 = **同一段容器界面打开期**里读到的空才是真拿空
				//       （``DisplayCaseReadsEmpty``），关掉后靠粘性保持熄灭。
				//     识别 = 静态白名单（SasDisplayCases.h，118 个原版记录）
				//     ∨ 运行期学习集合；完整实证见常量区「v4.9 展示柜」/「v4.10」。
				//     开关：INI `SkipDisplayCaseEmpty=0`（退回 v4.8）、
				//     `DisplayCaseUiEmpty=0`（退回 v4.9）。
				const bool displayCase =
					(!isCorpse && cat == static_cast<int>(Category::kContainer) &&
						g_cfg.skipDisplayCaseEmpty && IsDisplayCaseBase(base));
				if (isCorpse || cat == static_cast<int>(Category::kContainer)) {
					if (g_cfg.skipEmptyLoot && g_invOff != 0) {
						// ★ v4.10：展示柜要额外知道「本轮有没有见到投影条目」——
						//   它是「同一段打开期」判据的另一半（常量区「v4.10」）。
						//   ★ v4.13：展示柜还要**内容快照**（记账判据的基准，见常量区「v4.13」）。
						bool      sawTemporary = false;
						DcSnapOut snap{};
						// ★★★ 订正 R4（性能）：判空结果按引用缓存（见 Config::lootCacheTtlMs）。
						//   原来每个容器 / 尸体**每轮**都要走一遍库存链（SafeReadMem 好几个
						//   字段 + 逐条目），实测 30+ 次/轮；而它的权威变化信号是**事件**
						//   （拿 / 放物品、容器界面开关）—— 那些一发生就整体作废缓存。
						//   缓存命中 ⇒ 一次哈希查找，零内存读、零内核调用。
						//   ★ 展示柜不进缓存：它的「投影条目 / 段号」状态机本来就要每轮看。
						int  loot     = -3;  // -3 = 还没有结论（要走判空）
						bool fromMemo = false;
						if (!displayCase && g_cfg.lootCacheTtlMs > 0) {
							const auto it = g_state.lootMemo.find(ref);
							if (it != g_state.lootMemo.end() &&
								it->second.serial == g_state.lootMemoSerial &&
								a_nowMs - it->second.atMs < static_cast<std::uint64_t>(g_cfg.lootCacheTtlMs)) {
								loot     = it->second.loot;
								fromMemo = true;
								++g_state.cntLootMemo;
							}
						}
						if (!fromMemo) {
							const auto tl0 = NowUs();  // ★ 订正 R4：判空耗时单列（µs）
							loot = RefLootState(ref, displayCase ? &sawTemporary : nullptr,
								displayCase ? &snap : nullptr);
							g_state.tLootUs += NowUs() - tl0;
							++g_state.cntLoot;
							if (!displayCase) {
								if (g_state.lootMemo.size() >= kLootMemoMax) {
									g_state.lootMemo.clear();  // 兜异常增长（正常几十条）
								}
								g_state.lootMemo[ref] =
									State::LootMemoRec{ a_nowMs, g_state.lootMemoSerial, loot };
							}
						}
						// ★ v4.8：容器「判空链路快照」探针（首次 + 判决变化时各一条）。
						//   用户报「武器箱关着不亮、一打开就亮」—— 这两条记录就是答案：
						//   见 ContProbe 顶部的长注释（关着时的 size/sum/skipNp/skipEq
						//   与打开后一对比即可定位）。
						if (cat == static_cast<int>(Category::kContainer) && !isCorpse) {
							const auto tp0 = NowUs();  // ★ 订正 R4：诊断探针耗时单列
							ContProbe(ref, base, d2, loot, displayCase);
							g_state.tProbeUs += NowUs() - tp0;
							++g_state.cntProbe;
						}
						// ★★ v4.11：展示柜**逐帧观察**登记 + 「看到投影却没有界面标志」的取证/自愈。
						//   ① 逐帧观察把「拿空 ⇒ 立刻关掉」的观测窗口从 200ms 缩到 1 帧；
						//   ② 看到投影（`tp=1` 或 `sawTemporary`）说明搜刮界面确实开着，
						//      这时若我们的界面标志还是 false ⇒ 说明信号没接上（v4.10 的
						//      实测就是这样）⇒ 打菜单快照 + 尝试自愈学习（见常量区「v4.11」）。
						if (displayCase) {
							DcWatchAdd(ref, d2);
							// 只有「真的看到了投影条目」（`tp`）才做界面取证 —— 否则关着的
							// 武器箱每轮都会触发一次菜单快照（60 次 IsMenuOpen），既浪费
							// 又会刷日志。逐帧观察那边也有一条同样的取证入口。
							if (sawTemporary && !g_state.containerUiOpen) {
								DcUiProbe(ref);
							}
						}
						if (loot == 0) {
							// ★★ v4.10：展示柜的「空」分两种（用户实测：*「关着的武器箱
							//   会高亮了，但是拿空了却不会熄灭了」*）：
							//     · 同一段打开期里读到的空（= 内容被拿光）⇒ 判空（熄灭），
							//       并建立**粘性** ⇒ 关掉后保持熄灭 = 「拿空即灭」；
							//     · 关闭状态 / 从没打开过读到的空（= 内容未知）⇒ 照常亮
							//       （v4.9 的目标，不回归）。
							//   完整判据见常量区「v4.10 展示柜拿空即灭」/「v4.13」的长注释。
							bool emptyNow = !displayCase;  // 非展示柜：读到空就是空（行为不变）
							if (displayCase && g_cfg.displayCaseUiEmpty) {
								// ★ v4.13：「连续读到空」的计时（扫描节拍上也维护一份，
								//   这样即使逐帧观察表里没有它，兜底判据也有依据）
								auto& ds = DisplayCaseVerdictFor(ref);
								if (ds.emptySinceMs == 0) {
									ds.emptySinceMs = a_nowMs;
								}
								emptyNow = DisplayCaseReadsEmpty(ref, a_nowMs);
								if (emptyNow) {
									MarkDisplayCaseEmptied(ref,
										ds.quickOpenMs && !ds.sawTakeEvt ? "兜底（快速面板 + 连续读到空）"
																		 : "打开期里读到空",
										ds.quickOpenMs != 0 && !ds.sawTakeEvt);
								}
							}
							if (emptyNow) {
								++g_state.emptySkips;
								continue;  // 空 ⇒ 不进候选 ⇒ 宽限期后熄灭
							}
							// ★ v4.9：展示柜读到「空」不算空（关闭时内容本来就不在库存里）
							++g_state.displayCaseSkips;
						} else if (loot == 1) {
							++g_state.lootNotEmpty;
							// ★ v4.10：展示柜记「有东西」+ 刷新「这一段打开期」（见到投影条目时）
							//   ★ v4.13：顺便刷新**内容快照**（记账判据的基准）
							if (displayCase) {
								NoteDisplayCaseOccupied(ref, sawTemporary, &snap);
							}
							// ★ v4.4：「判到有东西」的尸体 ⇒ 打一份**库存明细**
							//   （只对近距离的、每个 ref 只打一次；上限 LootProbeMax）。
							//   这是「拿空还亮」的取证主力：残留条目会被逐条列出。
							if (isCorpse) {
								const auto tp0 = NowUs();  // ★ 订正 R4：探针耗时单列
								LootProbe(ref, d2);
								g_state.tProbeUs += NowUs() - tp0;
								++g_state.cntProbe;
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
				// ★★★ v5.0：自建通道号（通道模式用；旧路径忽略）——
				//   默认 = 类别号（每类一条独立通道）；「已扫描的植物」见下面那段。
				c.channel = static_cast<std::uint32_t>(cat);
				// ★★★ v4.25：星球目标（「植物」= FLOR：矿石 / 气体 / 液体 / 植物）——
				//   **已经扫描过的**换成「已扫描」状态（默认 5 = 原版那个绿），
				//   没扫描过的保持 StateFlora（7 = 原版青色脉冲）。
				//   ★ v4.28：判据 = ① 引擎自己的 `GetOutlineState(ref)`（主判据，
				//     植物 / 矿石 / 气体 / 液体通用）→ ② 引擎亲手画过的绿（学习）
				//     → ③ 资源链（只对「产出物品是 LVLI」的 FLOR 成立）—— 见常量区。
				//   带缓存；举着扫描仪时这一段根本走不到（上面已经 yield 掉了）。
				//   ⇒ 放下扫描仪之后：扫过的 = 绿、没扫过的 = 青，与原版一致。
				const bool floraJudgeOn =
					(g_cfg.floraScannedByEngineState && g_scannableOutlineStateReady) ||
					(g_cfg.floraScannedByResource && g_isResourceScannedReady);
				if (cat == static_cast<int>(Category::kFlora) && floraJudgeOn) {
					const auto tf0 = NowUs();  // ★ 订正 R4：植物 / 矿脉判据耗时单列（µs）
					const bool floraScanned = FloraTargetScanned(ref, base);
					g_state.tFloraUs += NowUs() - tf0;
					++g_state.cntFlora;
					if (floraScanned) {
						c.state = FloraScannedState();
						// ★★★ v5.0：通道模式下「已扫描」走专属通道（绿），
						//   与「未扫描」（kFlora = 青）分开 —— 与 state 7/5 的分法同构。
						c.channel = static_cast<std::uint32_t>(kChannelFloraScanned);
						++g_state.floraScannedSel;
					} else {
						++g_state.floraUnscannedSel;
					}
				}
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
				PhaseTimer tSync{ &g_state.tSyncUs, &g_state.tSyncMaxUs };
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
					// ★★★ v5.1.5（订正 R5）/ v5.1.6（订正 R6）：如果这次载入是**读档事件**
					//   发的（引擎的 `TESLoadGameEvent`），那么世界状态可能比记忆**更旧**
					//   ⇒ 记下「待复核」；**不立刻**处理（载入刚结束那一瞬间的复核会拿到
					//   假证伪，用户实测的「传送后变青」就是这么来的 —— 见 Config 里 v5.1.6 段）。
					//   等世界稳定后由 MaybeProcessFloraSaveLoad 真正处理
					//   （证据驱动：一个 base 都证伪不了 ⇒ 判定传送 / 读同一存档，记忆保持有效）。
					//   ★ 快速旅行 / 进门**不会**有这条事件 ⇒ 记忆照旧（同存档内有效）。
					NoteFloraSaveLoad("载入画面关闭", now);
				}
				g_state.loadingSeen = loading;
			}

			// ★ v4.11：先把「菜单事件通道」挂上（首次进世界时注册；失败每 4 秒重试）。
			//   v4.10 的轮询在实测里对 ContainerMenu **全程读到 0**（见常量区「v4.11」），
			//   所以这条事件路是「拿空即灭」能不能生效的关键。
			//   ★ v4.13：菜单路实测对「快速搜刮面板」本来就无效（面板不是菜单）——
			//     真正干活的换成下面那两个**游戏事件**（见常量区「v4.13」）。
			EnsureMenuEventSink(now);
			EnsureLootEventSinks(now);
			// ★★★ v5.1.5（订正 R5）：读档事件通道（「按存档隔离记忆」的唯一可靠信号）。
			EnsureLoadGameSink(now);
			//   兜底：读档事件在「我们还没看到载入画面」时就发出来了
			//   （典型：从主菜单直接读档 —— 游戏的 LoadingMenu 在我们的 Tick 跑起来
			//    之前就关了）⇒ 立刻处理，别把它错留到下一次「快速旅行」的载入画面
			//    关闭时才处理（那会让「存档边界」晚一步生效 = 那一小段窗口仍然假绿）。
			//   ★ 延迟守卫 1.5s：如果这次读档其实还会弹载入画面，正常路径会在它关闭时
			//     处理（那时标志已被消费）⇒ 这条兜底只覆盖「压根没看到载入画面」的情形，
			//     也避免在「旧世界还开着、载入画面还没弹出来」的那一瞬间就去做资源链复核。
			if (!IsLoadingScreenUp() && g_state.saveLoadPending.load(std::memory_order_relaxed) &&
				now - g_state.saveLoadPendingAtMs.load(std::memory_order_relaxed) >= 1500) {
				// ★★★ v5.1.6（订正 R6）：同样只记「待复核」（延后到世界稳定，见下）
				NoteFloraSaveLoad("读档事件（Tick 兜底：没看到载入画面）", now);
			}
			// ★★★ v5.1.6（订正 R6）：待复核到点了吗？（载入后 4 秒 + 没有新的载入 ⇒
			//   世界已稳定）—— 放这里统一处理，保证「复核拿到的一定是稳定世界的答案」。
			MaybeProcessFloraSaveLoad(now);
			//   事件是引擎在别的调用点发的 ⇒ 每帧把队列里的记录消化成判决
			//   （记账减账 / 快速面板会话；判据见常量区「v4.13」）。
			ProcessLootEvents(now);

			// ★ v4.10 / v4.11：容器（搜刮）界面的开/关 —— 每帧看一眼。每次「开 ↔ 关」
			//   翻转都把**段号** +1：展示柜的「拿空」判据靠段号把「同一段打开期」与
			//   「上一段（已关闭）」严格分开（推导见常量区「v4.10 展示柜拿空即灭」）。
			//   ★ v4.11：状态 = **事件 ∨ 轮询** —— 事件那条路（引擎自己的菜单事件）在
			//     实测里是唯一能读到 `ContainerMenu` 的；轮询保留是为了「事件通道没注册
			//     成功」时仍有兜底（对 LoadingMenu / FaderMenu 这条路本来是好的）。
			//   ★ 放在每帧路径上（不是 200ms 扫描路径）—— 关闭动作会让投影立刻收回
			//     （size→0），段号必须在那一刻就变，否则「关掉没拿」会被误判成「拿空」。
			{
				// ★ v4.12：事件侧的「容器菜单开着吗」**每帧在主线程重算一次**（不再只在收到事件时算）——
				//   自愈学习到新菜单名之后，它此刻「正开着」这件事必须**立刻**生效：否则要等
				//   下一次菜单事件才会更新，而「拿空」往往就发生在这一小段窗口里。
				bool evOpen = g_state.menuEvtOpen.load(std::memory_order_relaxed);
				if (g_cfg.containerMenuEvents && g_state.menuSinkRegistered) {
					std::scoped_lock evtLock{ g_state.menuEvtLock };
					evOpen              = MenuEvtContainerOpenLocked();
					g_state.menuEvtOpen = evOpen;
				}
				const bool uiOpen = g_cfg.containerMenuEvents ? (evOpen || PollContainerMenuOpen())
															 : PollContainerMenuOpen();
				if (uiOpen != g_state.containerUiOpen) {
					g_state.containerUiOpen = uiOpen;
					++g_state.containerUiSerial;
					// ★★★ 订正 R4：界面开 / 关 = 内容可能已经变过（投影收起 / 展开）
					//   ⇒ 判空缓存整体作废（判空延迟与旧行为一致）。
					++g_state.lootMemoSerial;
					if (uiOpen) {
						++g_state.containerUiOpens;
					}
				}

				// ★ v4.12：**首条事件的交叉校验放到主线程做**（事件回调里绝不调引擎函数 ——
				//   回调可能跑在 UI 线程、而 `IsMenuOpen` 内部要抢全局锁，不能在回调里碰）。
				//   这一行同时证明「事件通道接对了地方 + 负载解出来了」。
				if (g_cfg.containerMenuEvents && g_state.menuSinkRegistered &&
					!g_state.menuEvtCrossChecked && g_state.menuEvtTotal.load() > 0) {
					char firstName[kMenuEvtNameMax]{};
					{
						std::scoped_lock evtLock{ g_state.menuEvtLock };
						std::snprintf(firstName, sizeof(firstName), "%s", g_state.menuEvtLastOpen);
					}
					if (firstName[0]) {
						g_state.menuEvtCrossChecked = true;
						const bool polled = IsMenuNameOpenNow(firstName);
						REX::INFO("menu events: 首条事件交叉校验（主线程）: 最近一次打开 \"{}\" | "
								  "IsMenuOpen(\"{}\")={} | 事件总数={} -> {}",
							firstName, firstName, polled ? 1 : 0, g_state.menuEvtTotal.load(),
							polled ? "事件与轮询都看到它（通道接对了）"
								   : "事件收到了、但 IsMenuOpen 看不到它（正常：它可能不在 "
									 "`UI+0x450` 那张表里 / 屏幕提示类菜单不注册）");
					}
				}
			}

			// ★ v3.2：检测「引擎把我们的高亮清掉了」的时机并自动重挂
			//   （核心场景 = 玩家用原版手持扫描仪；见 DetectEngineOutlineLoss 的说明）。
			//   放在扫描节流之前：切换要即时被看到，不能等 200ms 的扫描窗口。
			//   ★ v4.0：放在读档处理**之后** —— 读档会把已挂目标标成「待摘」，
			//     顺序反了会被「重挂」覆盖掉（见 DetectEngineOutlineLoss 里的静置期判断）。
			DetectEngineOutlineLoss(now);

			// ★★★ v4.24：管理器占用探针 —— 「举起扫描仪」1.5 秒后打一次快照
			//   （引擎此刻至少写过一轮状态；见 LogManagerOccupancy / Config::managerOccupancyProbe）。
			//   放在 `!g_state.on` 提前返回**之前**：即使 F8 关掉功能也能取证。
			if (g_state.manDumpPending && g_state.monocleOpen && now >= g_state.manDumpAtMs) {
				g_state.manDumpPending = false;
				++g_state.manDumpRuns;
				LogManagerOccupancy("举着扫描仪");
			}

			if (!g_state.on) {
				++g_state.skipOff;
				return;
			}

			// ★ v4.11：展示柜**逐帧**观察（表空时几乎零成本）—— 把「拿空 ⇒ 立刻关掉」
			//   的观测窗口从 200ms 缩到 1 帧（观察表在 Rescan 里填，见 DcWatchAdd）。
			//   必须放在上面「容器界面开/关」那句之后：它判「同一段打开期」用的正是
			//   刚刷新过的 `containerUiOpen` / `containerUiSerial`。
			DcTickWatch();

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

			// ★★★ 订正 R4：本轮扫描的「墙钟 / 线程 CPU / 各分段」计量现在全部在
			//   `Rescan` 内部的 `ScanProfile`（作用域结束结算）—— 这样早退轮也能被
			//   正确归类（早退轮不进「分段平均」，见 ScanProfile 的说明）。
			Rescan(now, player);
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
				REX::INFO("  skip (窗口内): off={} throttle={} loading={} noplayer={} nocell={} settle={} shape={} unstable={} warmup={} yield={}",
					g_state.skipOff, g_state.skipThrottle, g_state.skipLoading,
					g_state.skipNoPlayer, g_state.skipNoCell, g_state.skipSettle,
					g_state.refsRejected, g_state.skipUnstable, g_state.skipWarmup,
					g_state.skipYield);
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
				g_state.skipYield      = 0;
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

				// ★★★ v5.0：自建颜色通道状态（通道模式下这一行说明一切）
				if (g_cfg.channelMode) {
					const char* st = g_channelsReady
					                   ? "已就绪"
					                   : (g_channelsFailed ? "失败(已回退旧路径)" : "等待模板标定");
					REX::INFO("  channel (自建颜色通道): 状态={} 通道={}/{} 已挂={} | "
							  "不写引擎状态表 / 不覆盖引擎配色（原版颜色 100% 原版）",
						st, g_channelsReady ? kChannelCount : 0, kChannelCount,
						g_state.outlined.size());
				}

				// ★★★ v4.25：星球目标「扫没扫过」的分流（用户反馈「放下扫描仪后还是扫描前的
				//   青色」就是要看这一行）：
				//     `未扫描=` / `已扫描=` —— 窗口内**本轮选中**的星球目标各占多少
				//       （已扫描 = 挂的是原版绿 state StateFloraScanned）；
				//     `判据: 查询/命中/链失败/缓存=` —— 判据本身的工作情况：
				//       查询在涨 = 真的在问引擎（带缓存，一个 base 最多每 2 秒一次）；
				//       命中 > 0  = 引擎确认「这个资源已经扫描过」；
				//       链失败在涨 = 我们那条 FLOR → LVLI → MISC → IRES 链又对不上了
				//       （日志里有 `flora scan:` 细节行 + 一条 WARN）。
				REX::INFO("  planet targets (窗口内): 未扫描={} 已扫描={} | 判据: 查询={} 命中={} 链失败={} 缓存={} 偏移=0x{:X}/0x{:X} "
						  "| 引擎状态: 问={} 已扫描={} 未扫描={} 未知={} | 按引用记忆(★v5.1): 命中={} 绿={} 青={} 总量={} "
						  "| 按物种扩散(★v5.1.8): 命中={} 物种表={} 新增={} 播种={} 跨星球放行={} 星球外拒={} "
						  "| 记忆作用域(★v5.1.5): 读档={} 事件={} 跳过={} 翻案={} 重见证={} 链复核: 丢={}条/{}base 清物种表={} "
						  "| 读档边界(★v5.1.6): 事件保留={} 确认base={} 推进={} "
						  "| 存档指纹(★v5.1.7): 锚={}天 处理={} 作废={}条 表剪={} 失败={} 存档={} "
						  "| 引擎状态表: 条目={} 读={} 绿={} 青={} 无条目={} "
						  "| 引擎进度直读(★v5.2/订正R12): 问={} 满={} 未满={} 失败={} 缓存命中={} 冲突={} "
						  "兜底K1={} keyType=0x{:X}(现读) keyZero={} ready={} 记忆层={} "
						  "| 学习表: 沿用={} 捡漏={} 落盘={} 写入={} 旧格式忽略={} 无时间={}"
						  "（★ v5.1：按引用记忆 = 记忆粒度是引用；★ v5.1.2：按物种扩散 = 同 species / "
						  "同资源被权威确认后，其**所有**实例一起变绿（引擎知识库本来就是这一级；"
						  "物种表**不落盘**，重启后靠引用级记忆动态激活）；"
						  "★ v5.1.5 记忆作用域 = 每次**读档**（引擎 TESLoadGameEvent）把作用域 +1 并清空物种表 ⇒ "
						  "只有「该存档内当场见证」的绿才算数（`跳过` = 过期记忆被忽略的次数、`翻案` = "
						  "引擎当场画「青」把过期绿降级的次数、`重见证` = 过期条目在本存档内被重新确认的次数、"
						  "`链复核 丢` = 资源链证明「这个存档里没扫描」而丢掉的条目/base 数"
						  "（★ v5.1.7 起**恒为 0**：不再删条目，只按作用域失效）、"
						  "`星球外拒` = 按物种扩散被「不在同一颗星球」挡下的次数"
						  "（★ v5.1.8 起**默认口径不再拒绝**：物种知识按引擎口径跨星球生效，"
						  "`跨星球放行` 记的就是这种放行的次数；`播种` = 用落盘记忆填进物种表的 base 数，"
						  "它让「传送 / 继续同一存档」时同 species 的实例**不必等举扫描仪**就是绿的））——"
						  "如果读档在涨而丢/跳过不动，说明那些绿在本存档里**确实**是已扫描的；"
						  "★ v5.1.6 起这条被用来判定「传送 / 读同一存档」——链复核一个 base 都证伪不了 ⇒ "
						  "**不推进作用域、不清物种表、不丢条目**（`事件保留` 记的就是这种「传送被误报成读档」的次数；"
						  "`确认base` = 复核里被链确认「本存档里已扫描」的 base 数；`推进` = 真正推进过几次存档边界）；"
						  "★★ v5.1.7（订正 R7）**默认口径已换成「存档时间指纹」**（上面那一段；`读档边界` "
						  "那段只剩回退路径的计数）：`锚` = 本存档的游戏时间（天，`Calendar::gameDaysPassed`）；"
						  "`处理` = 用指纹处理过几次读档；`作废` = 累计有多少条记忆「不在本存档作用域」"
						  "（= 学习时刻晚于本存档 —— **不删条目**，换更新的存档就自动回来）；"
						  "`表剪` = 物种表被剪掉的 base 数；`失败` = 游戏时间读不到的次数（那几次回退到 "
						  "v5.1.6 的链证据路径）；`存档` = 从 `BGSSaveLoadManager` 读到的存档名（诊断）；"
						  "引擎状态表 = 引擎那棵「引用→状态」红黑树的条目数 —— **玩多久都应该基本稳定**，"
						  "持续单调增长 = 有代码在往里插条目；命中/捡漏 = 判定在干活、沿用 = 保住的绿；"
						  "落盘 = 启动读回条数 / 写入 = 本会话新增 / 无时间 = 落盘行里没有时间字段的条数）"
						  " | 窗口取证={} ready={}/{}",
					g_state.floraUnscannedSel, g_state.floraScannedSel,
					g_state.floraScanQueries, g_state.floraScanHits,
					g_state.floraScanShapeFails, g_state.floraScannedCache.size(),
					g_floraProduceOff, g_floraResArrayOff,
					g_state.floraEngineStateQueries, g_state.floraEngineStateScanned,
					g_state.floraEngineStateUnscanned, g_state.floraEngineStateUnknown,
					g_state.floraRefHits, g_state.floraRefGreenNew, g_state.floraRefCyanNew,
					g_state.floraRefKnow.size(),
					g_state.floraBaseHits, g_state.floraBaseKnow.size(), g_state.floraBaseNew,
					g_state.floraBaseSeeded, g_state.floraBaseCrossPlanet,
					g_state.floraBasePlanetDenied,
					g_state.floraSaveLoads, g_state.loadEvtTotal.load(std::memory_order_relaxed),
					g_state.floraScopeSkipped, g_state.floraScopeDemoted, g_state.floraScopeRescoped,
					g_state.floraScopeDropped, g_state.floraScopeDroppedBase, g_state.floraScopeClearedBase,
					g_state.floraScopeKept, g_state.floraScopeConfirmedBase, g_state.floraSaveEpoch,
					(g_state.floraSaveDaysFloor >= 0.0f ? std::to_string(g_state.floraSaveDaysFloor)
														: std::string("无")),
					g_state.floraFpLoads, g_state.floraFpVoided, g_state.floraFpClearedBase,
					g_state.floraFpFailures,
					(g_state.floraFpSaveName[0] ? std::string(g_state.floraFpSaveName) : std::string("-")),
					OutlineStateTableCount(),
					g_state.floraTableProbes, g_state.floraTableGreen, g_state.floraTableCyan,
					g_state.floraTableNoEntry,
					g_state.floraProgQueries, g_state.floraProgFull, g_state.floraProgPartial,
					g_state.floraProgFails, g_state.floraProgCacheHits, g_state.floraProgConflict,
					g_state.floraProgK1Fallback,
					static_cast<unsigned>(g_floraKeyTypeLast), g_state.floraProgKeyZero,
					g_floraProgressReady ? 1 : 0,
					(g_cfg.floraUseMemory ? "ON（v5.1.8 旧口径）" : "关（默认，抛弃记忆）"),
					g_state.floraStickyKeeps, g_state.floraStatusTableHits,
					g_state.floraPersistLoaded, g_state.floraPersistWrites,
					g_state.floraPersistIgnored, g_state.floraPersistNoTime,
					g_state.floraScanDumps,
					g_isResourceScannedReady ? 1 : 0,
					g_scannableOutlineStateReady ? 1 : 0);
				g_state.floraUnscannedSel = 0;
				g_state.floraScannedSel   = 0;

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
							  "| 判空跳过: np={} eq={} | 展示柜跳过={} 展示柜拿空={} 逐帧拿空={} 逐帧帧数={}",
						g_state.corpseSeen, g_state.corpseByBit, g_state.corpseByFlag,
						g_state.corpseByLife, g_state.corpseByBleed,
						g_state.corpseProps, g_state.corpseUncSeen,
						g_state.achrSeen, g_state.achrLive,
						g_state.corpseUncSkipped,
						g_state.skipDeleted, g_state.skipParentCell,
						g_state.emptySkips, g_state.lootNotEmpty, g_state.lootUnknown,
						g_state.lootNullInv, g_state.lootBadShape, invOff,
						g_state.lootSkipNonPlayable, g_state.lootSkipEquipped,
						g_state.displayCaseSkips, g_state.displayCaseEmptied,
						g_state.displayCaseEmptiedFrame, g_state.dcWatchFrames);
					// ★ v4.11：容器界面信号与菜单事件（诊断一行看完）：
					//   `ui=` 事件∨轮询的最终状态；`ev=` 事件侧；`opens=`= 开过的段数；
					//   `evt=` 收到的菜单事件总数（0 = 事件通道没生效 ⇒ 看 WARN）；
					//   `learn=` 自愈学习到的菜单名个数。
					REX::INFO("  container menu (窗口内): ui={} ev={} opens={} serial={} | menu events: total={} container={} bad={} "
							  "| learned={} | watch: 表内={} dumps={}",
						g_state.containerUiOpen ? 1 : 0, g_state.menuEvtOpen.load() ? 1 : 0,
						g_state.containerUiOpens, g_state.containerUiSerial,
						g_state.menuEvtTotal.load(), g_state.menuEvtContainer.load(),
						g_state.menuEvtBad.load(),
						g_state.containerMenuLearnedCount, g_state.dcWatchCount, g_state.menuDumps);
					// ★ v4.13：**游戏事件通道**（这才是「拿空即灭」现在依赖的信号）：
					//   `物品事件 total=` 收到的 TESContainerChangedEvent 条数
					//     （take/put 两侧各算一条；drop= 队列满被丢掉的条数）；
					//   `命中=` 落在逐帧观察表上的条数；`减账/加账=` 真的动过账的次数；
					//   `记账判空=` **核心指标**（非 0 且增长 = 「拿空即灭」生效）；
					//   `快速面板 total/命中=` QuickContainerOpenedEvent；`兜底判空=` 兜底判据生效次数
					//     （★ v4.14 起兜底默认关 ⇒ 这一项应恒为 0；非 0 = INI 打开了兜底）。
					REX::INFO("  loot events (累计): 物品事件 total={} take={} put={} drop={} bad={} | 命中={} 未命中={} "
							  "| 减账={} 加账={} 记账判空={} || 快速面板 total={} 命中={} 未命中={} 兜底判空={} "
							  "| last: ref={:08X} {}ms 前",
						g_lootEvtTotal.load(), g_lootEvtTakeTotal.load(), g_lootEvtPutTotal.load(),
						g_lootEvtDropped.load(), g_lootEvtBad.load(),
						g_state.lootEvtMatches, g_state.lootEvtUnmatched,
						g_state.lootEvtTakeApplied, g_state.lootEvtPutApplied, g_state.lootEvtEmptyDecided,
						g_lootEvtQuickTotal.load(), g_state.quickOpenMatched, g_state.quickOpenUnmatched,
						g_state.quickOpenFallbackDecided,
						g_state.quickOpenLastFid,
						static_cast<unsigned long long>(g_state.quickOpenLastMs
															? (NowMs() - g_state.quickOpenLastMs)
															: 0));
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
					g_state.displayCaseEmptied = 0;  // ★ v4.10
					g_state.displayCaseEmptiedFrame = 0;  // ★ v4.11（逐帧判出「拿空」的次数）
					g_state.dcWatchFrames    = 0;         // ★ v4.11（逐帧观察跑了多少帧）
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

				// ★ v4.19：渲染侧参数快照（每会话最多 3 次，见 LogRendererParams）——
				//   这里的时机最好：管理器早就建好了、颜色覆盖也早写完了。
				LogRendererParams("stats");

				// 性能窗口：每轮扫描耗时（max 才是「卡顿」的感觉来源）与单轮引擎调用数。
				//   ★ 订正 R4：这里是**墙钟**（µs 计时 / 1000）；`完成` = 真的跑完遍历的轮数。
				REX::INFO("  timing: scan avg={}ms max={}ms 完成={}/{} 轮 ops={} deferred={} loading={}",
					g_state.scanMsSamples ? g_state.scanMsTotal / g_state.scanMsSamples / 1000 : 0,
					g_state.scanMsMax / 1000,
					g_state.scanFullSamples, g_state.scanMsSamples,
					g_state.opsThisScan,
					g_state.opsDeferred,
					g_state.loadingNow ? 1 : 0);

				// ★ v2.3：把「32ms 到底花在哪」直接拆开打出来（数都是窗口内平均）。
				//   shape = 读 cell + 引用数组形状校验（vq = 真的问内核几次 VirtualQuery）
				//   loop  = 遍历全部引用 + 排序 + 挑选
				//   sync  = SyncNativeOutline（其中 unh=摘、add=挂）
				//   ★★★ 订正 R4：① 全部分段平均**除以「完成轮」**（早退轮会把平均稀释）；
				//     ② 单位 µs / 1000 = ms（**真 ms**，原来 ms 取整会把几百 µs 抹成 0，
				//        于是 `loot=0ms` 是假象）；③ 新增 timing3 行 = 卡顿的正面证据。
				{
					const auto avg = [&](std::uint64_t a_sum) -> std::uint64_t {
						return g_state.scanFullSamples ? a_sum / g_state.scanFullSamples / 1000 : 0;
					};
					// ★ 订正 R3：把 loop / sync 再拆一层（定义见 State 里那一段注释）。
					//   refs/scan cur / ring = 每轮实际遍历的引用数（当前 cell / 环内分片）——
					//   ring 分片生效时 `分片推迟` 会同步增长（= 本轮没扫、留给下几片的那部分）。
					REX::INFO("  timing2: shape avg={}ms max={}ms vq={}/scan | loop avg={}ms max={}ms (refs/scan: cur={} ring={} 分片推迟={} | walk={}ms loot={}ms flora={}ms) | sync avg={}ms max={}ms (unh avg={}ms add avg={}ms 3D={}ms)",
						avg(g_state.tShapeUs), g_state.tShapeMaxUs / 1000,
						g_state.scanFullSamples ? (g_vqCalls - g_state.vqCalls) / g_state.scanFullSamples : 0,
						avg(g_state.tLoopUs), g_state.tLoopMaxUs / 1000,
						avg(g_state.refsWalkCur), avg(g_state.refsWalkRing), avg(g_state.ringSliceDeferred),
						avg(g_state.tWalkUs), avg(g_state.tLootUs), avg(g_state.tFloraUs),
						avg(g_state.tSyncUs), g_state.tSyncMaxUs / 1000,
						avg(g_state.tUnhUs), avg(g_state.tAddUs), avg(g_state.t3DUs));

					// ★★★ 订正 R4：`timing3` —— 回答「动态场景为什么卡」的那一行。
					//   ① `最慢一轮` = 本轮窗口里最慢的一轮（卡顿是**最慢那几轮**造成的，
					//      不是平均值）：总墙钟 / 其中线程 CPU（`cpu=`）—— 两者差得越远，
					//      越说明那几十毫秒不是我们的指令，而是**被抢 / 等内存 / 内核里**；
					//   ② `卡顿` = 遍历内部的空档（每 64 个引用取一次时刻，间隔 ≥ 3ms）：
					//      次数 / 合计 / 最大 —— 「合计」若占了 walk 的大头，同上结论；
					//   ③ 细分 = classify（分类 + ACHR 探针）/ loot（判空）/ probe（诊断探针）/
					//      flora（植物判据）/ 其它（= walk 减去上面四项，纯指针遍历 + 距离）；
					//   ④ 调用 = 各项**真实执行次数**（配耗时一起看才有意义）+ 判空缓存命中；
					//   ⑤ `vq` / `内核读` = 真的进了几次内核（订正 R4 之后应接近 0；
					//      这两个数在动态场景里涨起来 = 直读撞异常 / INI 关了 FastReadMem）。
					const auto avgUs = [&](std::uint64_t a_sum) -> std::uint64_t {
						return g_state.scanFullSamples ? a_sum / g_state.scanFullSamples : 0;
					};
					const std::uint64_t walkAvg = avgUs(g_state.tWalkUs);
					const std::uint64_t subAvg  = avgUs(g_state.tClassifyUs) + avgUs(g_state.tLootUs) +
												  avgUs(g_state.tProbeUs) + avgUs(g_state.tFloraUs);
					REX::INFO("  timing3: 完成={}轮 平均遍历={}个引用/轮 | 细分(µs/轮): walk={} classify={} loot={} probe={} flora={} 其它={} | "
							  "调用/轮: classify={} loot={}(缓存命中={}) flora={} probe={} vq={} 内核读={} 直读异常={} | "
							  "卡顿: 次数={} 合计={}µs 最大={}µs | 最慢一轮: 总={}µs cpu={}µs 引用={} walk={}µs 卡顿={}µs/{}次",
						g_state.scanFullSamples,
						g_state.scanFullSamples ? g_state.worstScanRefsSum / g_state.scanFullSamples : 0,
						walkAvg, avgUs(g_state.tClassifyUs), avgUs(g_state.tLootUs),
						avgUs(g_state.tProbeUs), avgUs(g_state.tFloraUs),
						walkAvg > subAvg ? walkAvg - subAvg : 0,
						g_state.scanFullSamples ? g_state.cntClassify / g_state.scanFullSamples : 0,
						g_state.scanFullSamples ? g_state.cntLoot / g_state.scanFullSamples : 0,
						g_state.scanFullSamples ? g_state.cntLootMemo / g_state.scanFullSamples : 0,
						g_state.scanFullSamples ? g_state.cntFlora / g_state.scanFullSamples : 0,
						g_state.scanFullSamples ? g_state.cntProbe / g_state.scanFullSamples : 0,
						g_state.scanFullSamples ? (g_vqCalls - g_state.vqCalls) / g_state.scanFullSamples : 0,
						g_state.scanFullSamples ? (g_kernelReads - g_state.kernelReads) / g_state.scanFullSamples : 0,
						g_state.scanFullSamples ? (g_sehFaults - g_state.sehFaults) / g_state.scanFullSamples : 0,
						g_state.stallCount, g_state.stallUs, g_state.stallMaxUs,
						g_state.worstScanUs, g_state.worstCpuUs, g_state.worstRefs,
						g_state.worstWalkUs, g_state.worstStallUs, g_state.worstStalls);
				}
				g_state.vqCalls       = g_vqCalls;
				g_state.sehFaults     = g_sehFaults;
				g_state.kernelReads   = g_kernelReads;
				g_state.scanMsTotal   = 0;
				g_state.scanMsMax     = 0;
				g_state.scanMsSamples = 0;
				g_state.tShapeUs      = 0;
				g_state.tShapeMaxUs     = 0;
				g_state.tLoopUs       = 0;
				g_state.tLoopMaxUs      = 0;
				g_state.tSyncUs       = 0;
				g_state.tSyncMaxUs      = 0;
				g_state.tUnhUs        = 0;
				g_state.tAddUs        = 0;
				// ★ 订正 R3：新拆出来的几段（loop/sync 细分）同步清零
				g_state.tWalkUs           = 0;
				g_state.tLootUs           = 0;
				g_state.tFloraUs          = 0;
				g_state.t3DUs             = 0;
				g_state.refsWalkCur       = 0;
				g_state.refsWalkRing      = 0;
				g_state.ringSliceDeferred = 0;
				// ★★★ 订正 R4：新增的细分计时 / 卡顿检测 / 调用计数 / 最慢一轮快照
				//   （timing3 行的数据源；分母 scanFullSamples 与 scanRefsLast 一并清零）
				g_state.tClassifyUs       = 0;
				g_state.tProbeUs          = 0;
				g_state.scanFullSamples   = 0;
				g_state.scanRefsLast      = 0;
				g_state.stallCount        = 0;
				g_state.stallUs           = 0;
				g_state.stallMaxUs        = 0;
				g_state.cntLoot           = 0;
				g_state.cntLootMemo       = 0;
				g_state.cntFlora          = 0;
				g_state.cntClassify       = 0;
				g_state.cntProbe          = 0;
				g_state.worstScanUs       = 0;
				g_state.worstScanRefsSum  = 0;
				g_state.worstRefs         = 0;
				g_state.worstWalkUs       = 0;
				g_state.worstLootUs       = 0;
				g_state.worstFloraUs      = 0;
				g_state.worstClassifyUs   = 0;
				g_state.worstStallUs      = 0;
				g_state.worstStalls       = 0;
				g_state.worstCpuUs        = 0;

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

		// ★★★ v5.2：引擎扫描进度直读的 5 个**纯查询**函数（签名校验 + key 常量提取；
		//   任何一项不过 ⇒ 自动禁用新判据、回退旧判据链，不影响高亮本身）
		ResolveFloraProgressFunctions();

		// ★ v4.0：读档自愈不再需要任何 Papyrus / ESM 通道 —— 直接看载入画面
		//   （见 Tick 里的「载入画面由开变关」），另有「换 cell」与
		//   「HighlightManager 数量下跌」两路判据兜底。

		task->AddPermanentTask(Tick);

		REX::INFO("SAS_AlwaysScan installed (main thread {})", g_mainThreadId.load());
		return true;
	}
}
