// ============================================================================
//  Starfield Always Scan - 核心逻辑（SAS_AlwaysScan.dll）
//
//  目标（见 AGENTS.md）：
//    ① 不装备手持扫描仪，也能一直有「扫描仪高亮」效果；
//    ② 高亮不再受原版那个「屏幕中央圆圈」限制，改成玩家周围一个半径；
//    ③ 一个快捷键开关。
//
//  分工（沿用上一代项目验证过的架构）：
//    · **原生侧（本文件）**：遍历玩家所在 cell 的引用数组（引擎自带的
//      TESObjectCELL::ForEachReference，带 BSAutoReadLock，几千个引用只有微秒级），
//      按「距离 + base form 类型白名单」筛出该亮的对象，把结果追加进 FormList 信箱。
//      没有关键词、没有 120 上限、不调用任何引擎函数。
//    · **Papyrus 侧（SAS_Bridge.psc）**：只负责把信箱里的引用逐个 Play/Stop。
//      「给引用播 EFSH」这个能力目前只有 Papyrus 暴露（EffectShader.Play/Stop）,
//      所以保留一个极轻的桥；它不做任何搜索、不做任何判断。
//
//  跨语言通道（两个信箱 + 游标）：
//      SAS_OpList   (FLST) ← DLL 追加「该亮」的引用
//      SAS_OpCursor (GLOB) → 脚本发布「已处理到第几个」
//      SAS_StopList (FLST) ← DLL 追加「该灭」的引用（只在关掉开关时用）
//      SAS_StopCursor(GLOB)→ 同上
//      SAS_Epoch    (GLOB) → 脚本读档时 +1，DLL 看到变化就整批重置
//    DLL 只在 cursor == size（**相等**判定，防读档回滚）时才清表。
//
//  几个「不要再踩」的坑（全部来自上一代项目的实测）：
//    ① **绝不缓存 BSTArray 的 data()/capacity()**：清表可能释放/搬移缓冲，
//       缓存下来就是悬空指针，往那儿写 8 字节 → HEAP_CORRUPTION 延迟崩溃。
//       本文件每次现读，运行期零缓存。
//    ② **换 cell / 读档时不能立刻释放发光集合**：那批 ref 的 NiPointer 是它们
//       唯一的保活来源，而信箱里可能还有桥没读的 op → 立刻释放就是 Play 野指针。
//       所以走「退役队列」，继续保活 2.5 秒再释放。
//    ③ **只看主线程**：SFSE 的 AddPermanentTask 挂在 Command_Process 上，
//       读档期间加载线程也会调它；非主线程直接返回。
//    ④ **游标必须早于 Play/Stop 推进**（这条在脚本侧）。
// ============================================================================

#include "PCH.h"

#include "AlwaysScan.h"

#include "RE/B/BGSListForm.h"
#include "RE/F/FormTypes.h"
#include "RE/N/NiAVObject.h"
#include "RE/N/NiPoint.h"
#include "RE/N/NiSmartPointer.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/T/TESForm.h"
#include "RE/U/UI.h"
#include "RE/T/TESGlobal.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/T/TESObjectREFR.h"

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

		// ESM 里的局部 FormID（低 24 位）。运行时前缀由 quest 的 FormID 高字节推出，
		// 所以这里的顺序必须与 tools\xedit-scripts\build_sas.pas 完全一致。
		constexpr std::uint32_t kLocalOpList     = 0x801;
		constexpr std::uint32_t kLocalOpCursor   = 0x802;
		constexpr std::uint32_t kLocalStopList   = 0x803;
		constexpr std::uint32_t kLocalStopCursor = 0x804;
		constexpr std::uint32_t kLocalEpoch      = 0x805;
		// ★ v2.2 新增：Papyrus 侧的诊断通道（见 docs/04「任务引导路径」）。
		//   原生模式下两个信箱是空闲的，所以借它们让脚本把自己的状态回报给 DLL，
		//   由 DLL 统一打在主日志里 —— 这样「脚本有没有跑 / 引导法术装上了没有」
		//   就不用去翻 Papyrus 日志了。
		constexpr std::uint32_t kLocalGuideHb    = 0x807;  // 心跳计数（脚本每 0.25s +1）
		constexpr std::uint32_t kLocalGuideState = 0x808;  // 0=脚本没跑 2=法术在但效果没生效 3=法术+效果都在
		// ★ v2.4 新增（面包屑引导路径，见 docs/04 第九节）：
		//   SAS_On            DLL → 脚本：F8 开关状态（1/0）。脚本据此决定画不画面包屑。
		//   SAS_GuideMarkers  脚本 → DLL：当前点亮的路径标记数（-1 = 开着但找不到引导目标）。
		constexpr std::uint32_t kLocalSASOn        = 0x809;
		constexpr std::uint32_t kLocalGuideMarkers = 0x80A;

		constexpr const char* kBindQuestEdid = "SAS_AlwaysScanQuest";

		constexpr std::size_t kMaxMailbox = 512;  // 信箱硬上限，超过就背压

		// 换 cell / 读档时把已点亮的引用继续保活这么久再释放（见文件头坑②）
		constexpr std::uint64_t kRetireKeepAliveMs = 2500;

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
		constexpr std::size_t  kOffFormType  = 0x2E;
		constexpr std::uint8_t kFormTypeREFR = static_cast<std::uint8_t>(RE::FormType::kREFR);
		constexpr std::uint8_t kFormTypeACHR = static_cast<std::uint8_t>(RE::FormType::kACHR);

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

		// 合理性上限：一个 cell 的引用数不可能超过这个数（落到这里就是读到垃圾了）
		constexpr std::uint32_t kMaxRefsSanity = 200000;

		// 换 cell / 读档后先静置这么久再开始扫描（引擎此时还在重建世界）
		constexpr std::uint64_t kSettleAfterSceneChangeMs = 2000;
		// 引用数组长度连续两轮一致才认（长度突变 = 加载线程还在往数组里塞）
		constexpr std::uint32_t kRefsStableRounds = 2;

		constexpr std::uint64_t kStatsLogIntervalMs = 5000;
		constexpr std::uint64_t kBindWarnIntervalMs = 5000;

		// 偏航角 → 前向量的符号（见 Rescan 里 OnlyInFront 的说明）。
		// Gamebryo/NetImmerse 常规：yaw=0 面向 +Y，forward=(sin(z), cos(z))。
		constexpr float kYawForwardSign = 1.0f;


		// ====================================================================
		// 配置（Data\SFSE\Plugins\SAS_AlwaysScan.ini）
		// ====================================================================
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
			float         glowDurationSec = 90.0f;
			int           maxTargets      = 256;
			double        opsPerSecond    = 8.0;
			double        bucketSize      = 80.0;
			int           scanIntervalMs  = 200;
			bool          logStats        = true;

			// --- 视觉层（见 docs/03-原生outline高亮实现.md）---
			//  1 = 原生 outline 高亮（引擎自带，和原版扫描仪同一套渲染）
			//  0 = 旧方案：走 Papyrus 桥 Play 一条 EFSH（上一代 mod 的做法）
			int           highlightMode   = 1;
			// 原生高亮使用的 outline 状态 0..11（对应 GMST 里 12 种配色/样式）。
			//  0/1 = Generic（未扫描/已扫描），2/3 = Scannable 系，7/8 = 另一组，9 = 追踪
			int           outlineState    = 0;
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
		};

		// ====================================================================
		// 运行期状态
		// ====================================================================
		struct GlowEntry
		{
			RE::NiPointer<RE::TESObjectREFR> ref;
			std::uint64_t                    dueMs = 0;  // 该重新 Play 的时刻(ms)
		};

		struct RetiredBatch
		{
			std::uint64_t                                 releaseMs = 0;
			std::vector<RE::NiPointer<RE::TESObjectREFR>> refs;
		};

		// 原生 outline：一个已挂高亮的引用 + 下次该重申的时刻
		struct OutlineEntry
		{
			RE::NiPointer<RE::TESObjectREFR> ref;
			std::uint64_t                    reassertMs = 0;
			// 0 = 还在选中集合里；非 0 = 掉队时刻（到这个时刻之后才真正摘除）。
			// 见 Config::unhighlightGraceMs 的说明。
			std::uint64_t                    dropAt     = 0;
		};

		struct State
		{
			// --- 表单绑定 ---
			bool             bound      = false;
			std::uint32_t    prefix     = 0;
			RE::BGSListForm* opList     = nullptr;
			RE::TESGlobal*   opCursor   = nullptr;
			RE::BGSListForm* stopList   = nullptr;
			RE::TESGlobal*   stopCursor = nullptr;
			RE::TESGlobal*   epoch      = nullptr;
			// v2.2：Papyrus 侧自报状态（见 kLocalGuideHb / kLocalGuideState）
			RE::TESGlobal*   guideHb    = nullptr;
			RE::TESGlobal*   guideState = nullptr;
			// v2.4：面包屑引导路径（SAS_On 由本 DLL 写，GuideMarkers 由脚本写）
			RE::TESGlobal*   sasOn        = nullptr;
			RE::TESGlobal*   guideMarkers = nullptr;
			double           lastGuideHb    = -1.0;
			double           lastGuideState = -1.0;

			// --- 开关 ---
			bool on      = true;
			bool keyDown = false;

			// --- 发光集合 ---
			std::unordered_map<RE::TESObjectREFR*, GlowEntry> glowing;

			// --- 原生 outline：已挂上高亮的引用 ---
			//   （原生模式下不再需要令牌桶/信箱，只在「新目标」和「周期性重申」时动手）
			//
			// ★ 这里存的是 NiPointer 而不是裸指针：摘高亮时要调引擎的
			//   ClearOutlineState，而它会解引用引用取 3D —— 裸指针在引用被销毁后
			//   就是野指针。用 NiPointer 保活（和 glowing / retired 同一套做法）。
			std::unordered_map<RE::TESObjectREFR*, OutlineEntry> outlined;
			bool          nativeReady      = false;  // 核心三个引擎函数都已按签名校验通过
			// 「摘除」两个函数（Remove / Deactivate）是否可用。不可用时退化成
			// 「只把状态写回 12」——能少一点描边残留，但**摘不干净**（见上方长注释）。
			bool          nativeRemoveReady = false;
			std::uint64_t outlineRemoved   = 0;      // 成功摘除次数（诊断）
			std::uint64_t outlineRemoveMiss = 0;     // 调了 Remove 但一个管理器里都没有（诊断）
			std::uint64_t outlineUnhighlightMiss = 0; // 引擎的 0x653F60 也没动到任何管理器（诊断）
			std::uint64_t lastOutlineErrMs = 0;

			// --- 关掉开关时要熄灭的那批（分批喂给 StopList，避免一次性灌爆）---
			// ★ 这些 NiPointer **必须留到信箱被桥消费完**才能释放：信箱里存的是裸指针
			//   （FormList 不持引用计数），提前释放会让桥 Stop 一个已销毁对象。
			std::vector<RE::NiPointer<RE::TESObjectREFR>> pendingStop;
			std::size_t                                   pendingStopSent = 0;

			// --- 退役队列（保活用）---
			std::vector<RetiredBatch> retired;

			// --- 节流 / 统计 ---
			std::uint64_t lastScanMs    = 0;
			std::uint64_t lastOpMs      = 0;
			std::uint64_t lastStatsMs   = 0;
			std::uint64_t lastBindWarnMs = 0;
			std::uint64_t lastMapDiagMs = 0;
			double        tokens        = 0.0;
			std::uint64_t scanCount     = 0;
			std::uint64_t playedCount   = 0;
			std::uint64_t stoppedCount  = 0;
			std::uint64_t tickCount     = 0;
			std::uint64_t candCount     = 0;
			std::uint64_t selCount      = 0;
			std::uint64_t backpressure  = 0;

			// --- 诊断：半径内、但 base form 类型不在白名单而被跳过的类型统计 ---
			//   统计窗口 = 两条统计日志之间，打完之后清空。
			//   用途：如果游戏里发现「某个东西该亮却没亮」，看这条日志就知道它的
			//   formType 是多少，再决定要不要加进 IsHighlightableBase 的白名单。
			//   ★ v2.2：从 unordered_map 换成定长数组 —— 这段代码对**每个被拒的
			//   引用**都要执行一次，密集场景里每秒 2 万次哈希表操作，纯属白烧主线程。
			//   现在下标就是 formType，一次自增完事。
			std::array<std::uint32_t, 256> rejectTypes{};

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
			RE::TESObjectCELL* lastCell    = nullptr;
			std::uint32_t      lastEpoch   = 0;
			bool               epochSeen   = false;

			// --- cell 引用数组的稳定性判据（见 Rescan 顶部）---
			std::size_t   cellRefsOff   = 0;  // 形状校验通过的偏移（0 = 还没定）
			std::uint32_t lastRefsSize  = 0;  // 上一轮看到的引用数
			std::uint32_t stableRounds  = 0;  // 长度连续相同的轮数
			std::uint64_t settleUntilMs = 0;  // 在此之前不扫描（换场景/读档后的静置期）
			std::uint64_t refsRejected  = 0;  // 引用数组校验不通过而跳过的次数
		};

		Config               g_cfg;
		State                g_state;
		std::mutex           g_tickLock;
		std::atomic_uint32_t g_mainThreadId{ 0 };
		std::string          g_iniPath;

		// 前置声明：定义在后面的「原生 outline」小节里（SetOn / ResetForNewScene 要先用到）
		void ClearAllNativeOutline();
		void MarkAllForRemoval(std::uint64_t a_deadlineMs);

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
			g_cfg.glowDurationSec = std::clamp(getFloat("GlowDurationSecs", 90.0f), 5.0f, 600.0f);
			g_cfg.opsPerSecond   = std::clamp(static_cast<double>(getFloat("OpsPerSecond", 8.0f)), 0.5, 60.0);
			g_cfg.bucketSize     = std::clamp(static_cast<double>(getFloat("BucketSize", 80.0f)), 4.0, 512.0);

			g_cfg.highlightMode = std::clamp(getInt("HighlightMode", 1), 0, 1);
			g_cfg.outlineState  = std::clamp(getInt("OutlineState", 0), 0, 11);
			g_cfg.onlyInFront   = getInt("OnlyInFront", 1) != 0;
			g_cfg.frontFovDeg   = std::clamp(getFloat("FrontFovDeg", 110.0f), 20.0f, 360.0f);
			g_cfg.reassertMs    = std::clamp(getInt("ReassertMs", 0), 0, 60000);  // 0 = 不重申
			g_cfg.autoEnsureManagers = getInt("AutoEnsureManagers", 1) != 0;
			g_cfg.unhighlightGraceMs = std::clamp(getInt("UnhighlightGraceMs", 1500), 0, 60000);
			g_cfg.maxOutlineOpsPerScan = std::clamp(getInt("MaxOutlineOpsPerScan", 64), 0, 4096);

			REX::INFO("config: radius={:.1f}m duration={:.0f}s targets={} hotkeyVK=0x{:X} startEnabled={}",
				g_cfg.radiusMeters, g_cfg.glowDurationSec, g_cfg.maxTargets,
				g_cfg.hotkeyVk, g_cfg.startEnabled);
			REX::INFO("config: unitsPerMeter={:.4f} -> 半径 {:.1f}m = {:.1f} 游戏单位",
				g_cfg.unitsPerMeter, g_cfg.radiusMeters, g_cfg.radiusMeters * g_cfg.unitsPerMeter);
			REX::INFO("config: highlightMode={} outlineState={} onlyInFront={} frontFov={:.0f}deg reassert={} autoEnsure={}",
				g_cfg.highlightMode == 1 ? "native-outline" : "legacy-efsh",
				g_cfg.outlineState, g_cfg.onlyInFront, g_cfg.frontFovDeg,
				g_cfg.reassertMs ? std::to_string(g_cfg.reassertMs) + "ms" : std::string{ "off" },
				g_cfg.autoEnsureManagers);
			REX::INFO("config: unhighlightGrace={}ms maxOutlineOpsPerScan={}",
				g_cfg.unhighlightGraceMs, g_cfg.maxOutlineOpsPerScan);
		}

		// ====================================================================
		// ★ v2.4：把 F8 开关状态发布给 Papyrus 桥
		// ====================================================================
		// 脚本据此决定画不画「面包屑引导路径」（见 docs/04 第九节）。
		// 为什么走 GLOB：脚本侧**没有任何**「高亮开关是否打开」的查询接口 ——
		// 连「扫描仪是否举着」都得靠 RegisterForMenuOpenCloseEvent("MonocleMenu")
		// 自己记（B 社脚本注释原话：we'll need a way to check if you have the
		// scanner up or not）。所以最省事、最可靠的办法就是在 DLL 里写一个 GLOB。
		void PublishSASOn()
		{
			if (!g_state.sasOn) {
				return;
			}
			g_state.sasOn->value = g_state.on ? 1.0f : 0.0f;
		}

		// ====================================================================
		// 表单绑定
		// ====================================================================
		// 只用一条通道：先按 EDID 拿 quest（上一代实测「QUST 的 EDID 能查到」），
		// 再从它的 FormID 取本 ESM 的 load order 前缀，其余表单全用 LookupByID。
		// 这样不依赖 TESDataHandler 的内存布局，也不依赖 FLST/GLOB 的 EDID 是否驻留。
		bool BindForms()
		{
			auto* quest = RE::TESForm::LookupByEditorID(RE::BSFixedString{ kBindQuestEdid });
			if (!quest) {
				const auto now = NowMs();
				if (now - g_state.lastBindWarnMs > kBindWarnIntervalMs) {
					g_state.lastBindWarnMs = now;
					REX::WARN("BindForms: quest '{}' not found yet (plugin not loaded? wrong load order?)", kBindQuestEdid);
				}
				return false;
			}

			const std::uint32_t prefix     = quest->GetFormID() & 0xFF000000u;
			auto*               opList     = RE::TESForm::LookupByID<RE::BGSListForm>(prefix | kLocalOpList);
			auto*               opCursor   = RE::TESForm::LookupByID<RE::TESGlobal>(prefix | kLocalOpCursor);
			auto*               stopList   = RE::TESForm::LookupByID<RE::BGSListForm>(prefix | kLocalStopList);
			auto*               stopCursor = RE::TESForm::LookupByID<RE::TESGlobal>(prefix | kLocalStopCursor);
			auto*               epoch      = RE::TESForm::LookupByID<RE::TESGlobal>(prefix | kLocalEpoch);
			// v2.2 诊断通道（可选）：老版本 ESM 里没有它们，只用警告提醒，不阻断绑定。
			auto*               guideHb    = RE::TESForm::LookupByID<RE::TESGlobal>(prefix | kLocalGuideHb);
			auto*               guideState = RE::TESForm::LookupByID<RE::TESGlobal>(prefix | kLocalGuideState);
			// v2.4 面包屑引导路径（可选，同上：老 ESM 里没有也不阻断）
			auto*               sasOn         = RE::TESForm::LookupByID<RE::TESGlobal>(prefix | kLocalSASOn);
			auto*               guideMarkers  = RE::TESForm::LookupByID<RE::TESGlobal>(prefix | kLocalGuideMarkers);

			if (!opList || !opCursor || !stopList || !stopCursor || !epoch) {
				REX::WARN("BindForms: incomplete (prefix={:08X} play={} cur={} stop={} stopcur={} epoch={})",
					prefix, static_cast<bool>(opList), static_cast<bool>(opCursor),
					static_cast<bool>(stopList), static_cast<bool>(stopCursor), static_cast<bool>(epoch));
				return false;
			}

			g_state.prefix     = prefix;
			g_state.opList     = opList;
			g_state.opCursor   = opCursor;
			g_state.stopList   = stopList;
			g_state.stopCursor = stopCursor;
			g_state.epoch      = epoch;
			g_state.guideHb    = guideHb;
			g_state.guideState = guideState;
			g_state.sasOn      = sasOn;
			g_state.guideMarkers = guideMarkers;
			g_state.bound      = true;
			if (!guideHb || !guideState) {
				REX::WARN("BindForms: guide 诊断 GLOB 缺失（{} / {}）—— 请重新构建 ESM（v2.2 起新增 0x807/0x808）",
					kLocalGuideHb, kLocalGuideState);
			}
			if (!sasOn || !guideMarkers) {
				REX::WARN("BindForms: 引导路径 GLOB 缺失（{} / {}）—— 请重新构建 ESM（v2.4 起新增 0x809/0x80A）",
					kLocalSASOn, kLocalGuideMarkers);
			}
			// 首次绑定就把开关状态发布给脚本（否则脚本读到的是 pex 默认值 1）
			PublishSASOn();

			REX::INFO("forms bound: prefix={:08X} quest={:08X} arrayOfForms@0x{:X} (checked against sizeof(TESForm)=0x38)",
				prefix, quest->GetFormID(),
				static_cast<unsigned>(offsetof(RE::BGSListForm, arrayOfForms)));

			// 上一次异常退出可能在信箱里留东西；启动时清干净
			if (!opList->arrayOfForms.empty() || !stopList->arrayOfForms.empty()) {
				REX::WARN("mailbox not empty at bind (play={} stop={}) -> clearing",
					opList->arrayOfForms.size(), stopList->arrayOfForms.size());
				opList->arrayOfForms.clear();
				stopList->arrayOfForms.clear();
			}
			return true;
		}

		bool ReadGlobalValue(RE::TESGlobal* a_glob, double& a_out)
		{
			if (!a_glob) {
				return false;
			}
			const float v = a_glob->value;
			if (!std::isfinite(v) || v < -1.0f || v > 1.0e7f) {
				return false;  // 读到垃圾时宁可不动作
			}
			a_out = static_cast<double>(v);
			return true;
		}

		// ====================================================================
		// 信箱
		// ====================================================================
		bool PushTo(RE::BGSListForm* a_list, RE::TESObjectREFR* a_ref)
		{
			if (!a_list || !a_ref) {
				return false;
			}
			auto& arr = a_list->arrayOfForms;
			if (arr.size() >= kMaxMailbox) {
				++g_state.backpressure;
				return false;
			}
			arr.push_back(a_ref);
			return true;
		}

		void ClearMailbox(RE::BGSListForm* a_list)
		{
			if (a_list) {
				a_list->arrayOfForms.clear();  // 元素是裸指针、平凡析构；绝不缓存 data()
			}
		}

		// 桥脚本消费完之后才清表：cursor == size（**相等**，不是 >=）
		bool MaybeClear(RE::BGSListForm* a_list, RE::TESGlobal* a_cursor)
		{
			if (!a_list || !a_cursor) {
				return false;
			}
			const std::size_t size = a_list->arrayOfForms.size();
			if (size == 0) {
				return false;
			}
			double cur = 0.0;
			if (!ReadGlobalValue(a_cursor, cur)) {
				return false;
			}
			if (static_cast<std::size_t>(cur) == size) {
				ClearMailbox(a_list);
				return true;
			}
			return false;
		}

		// ====================================================================
		// 过滤规则（base form 类型白名单）
		// ====================================================================
		// NPC_ / ACHR / LVLN 一律排除 —— 「活人不亮」是红线。
		bool IsHighlightableBase(const RE::TESForm* a_base)
		{
			if (!a_base) {
				return false;
			}
			// ★ v3.0：引导光带的珠子**自己不能被高亮**。
			//   珠子形态是原版 MSTT「Glow*」家族（脚本侧默认 0x00098106 GlowBall10x10，
			//   候选见 SAS_Bridge.psc 的 CfgGuideMarkerFormID()）——它们是环境装饰用的
			//   自发光体，本来就不该出现在扫描高亮里；而且 20 颗珠子会白占 MaxTargets
			//   配额，每次开关光带还会带来一批挂/摘描边的 churn。
			//   这几条 FormID 全在 Starfield.esm（高 8 位是 load order index，所以不会误伤）。
			switch (a_base->GetFormID()) {
			case 0x00098105u:  // MSTT GlowCube10x10
			case 0x00098106u:  // MSTT GlowBall10x10  <- 当前使用的形态
			case 0x0009811Cu:  // MSTT GlowDisc10x10
			case 0x0001760Fu:  // MSTT GlowLightCone36
				return false;
			default:
				break;
			}
			switch (a_base->GetFormType()) {
			// 可拾取
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
			// 可交互
			case RE::FormType::kCONT:
			case RE::FormType::kACTI:
			case RE::FormType::kDOOR:
			case RE::FormType::kTERM:
			case RE::FormType::kFLOR:
			case RE::FormType::kMSTT:
				return true;
			default:
				return false;
			}
		}

		// ====================================================================
		// 状态重置
		// ====================================================================
		// 把发光集合整批搬进退役队列（继续保活），不清信箱里的 op（它们本来就该发出去）。
		void RetireGlowTable(std::uint64_t a_nowMs)
		{
			if (g_state.glowing.empty()) {
				return;
			}
			RetiredBatch batch;
			batch.releaseMs = a_nowMs + kRetireKeepAliveMs;
			batch.refs.reserve(g_state.glowing.size());
			for (auto& [key, entry] : g_state.glowing) {
				if (entry.ref) {
					batch.refs.push_back(std::move(entry.ref));
				}
			}
			g_state.glowing.clear();
			const auto n = batch.refs.size();
			g_state.retired.push_back(std::move(batch));
			REX::INFO("glow table retired (retired={})", n);
		}

		void TickRetired(std::uint64_t a_nowMs)
		{
			while (!g_state.retired.empty() && g_state.retired.front().releaseMs <= a_nowMs) {
				g_state.retired.erase(g_state.retired.begin());
			}
		}

		// 换 cell / 读档：清空两个信箱 + 退役发光集合
		void ResetForNewScene(std::uint64_t a_nowMs, const char* a_reason)
		{
			REX::INFO("reset ({}) play={} stop={} glow={} outline={}",
				a_reason,
				g_state.opList ? g_state.opList->arrayOfForms.size() : 0,
				g_state.stopList ? g_state.stopList->arrayOfForms.size() : 0,
				g_state.glowing.size(),
				g_state.outlined.size());

			// 顺序很重要：先丢信箱（裸指针），再释放保活引用，否则桥可能 Stop 到悬空对象
			ClearMailbox(g_state.opList);
			ClearMailbox(g_state.stopList);
			g_state.pendingStop.clear();
			g_state.pendingStopSent = 0;
			RetireGlowTable(a_nowMs);

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

			// 面包屑引导路径跟着开关走（脚本读这个 GLOB）
			PublishSASOn();

			if (!a_on && g_cfg.highlightMode == 1) {
				// 原生 outline：把这批引用从引擎的高亮表里**真正摘掉**
				// （Remove(managers[state]+0x18, &id) → Deactivate(id) → 状态写回 12）
				ClearAllNativeOutline();
			}

			if (!a_on && g_cfg.highlightMode != 1) {
				// 把已经点亮的整批交给桥去熄灭（分批喂，避免一次灌爆信箱）。
				// 如果上一轮还有没走完的，先把它们并进来（不清空 —— 清空会释放
				// 那些还被 StopList 里的裸指针引用的对象）。
				g_state.pendingStop.reserve(g_state.pendingStop.size() + g_state.glowing.size());
				for (auto& [key, entry] : g_state.glowing) {
					if (entry.ref) {
						g_state.pendingStop.push_back(std::move(entry.ref));
					}
				}
				g_state.glowing.clear();
				REX::INFO("pending stop queued: {} (sent={})",
					g_state.pendingStop.size(), g_state.pendingStopSent);
			}
			// 重新打开时**不动** pendingStop：让它按原顺序排空，
			// 否则信箱里那批裸指针会被提前释放（见 pendingStop 的注释）。
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
			RE::TESObjectREFR* ref = nullptr;
			float              d2  = 0.0f;  // 世界单位平方距离
			bool               lit = false; // 已经在高亮里
			float              key = 0.0f;  // 排序键
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
		void           LogManagerDiagnostics(const char* a_tag);
		void           LogManagerMapDiag(const char* a_tag, std::uint32_t a_state);
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
			LogManagerMapDiag("install", static_cast<std::uint32_t>(g_cfg.outlineState));
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

		void LogManagerDiagnostics(const char* a_tag)
		{
			REX::INFO("native outline managers[{}] = {}/{} alive (state {} -> {})",
				a_tag, CountLiveManagers(), kOutlineManagerUsed,
				g_cfg.outlineState, OutlineManagerFor(static_cast<std::uint32_t>(g_cfg.outlineState)) ? "ok" : "NULL");
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
			//   现在只调「状态表里记的那个」和「配置的那个」各一次，第一次几乎必然命中。
			if (g_outlineGraphRemoveReady) {
				const auto cfgState = static_cast<std::uint32_t>(g_cfg.outlineState);
				bool       done     = false;
				if (stateInTable < kOutlineManagerUsed) {
					done = OutlineUnhighlightRef(a_ref, stateInTable);
				}
				if (!done && cfgState != stateInTable) {
					done = OutlineUnhighlightRef(a_ref, cfgState);
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
			const auto wantState = static_cast<std::uint32_t>(g_cfg.outlineState);
			const auto reassert  = static_cast<std::uint64_t>(g_cfg.reassertMs);
			const auto nextMs    = reassert ? a_nowMs + reassert : UINT64_MAX;
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
					if (OutlineRef(c->ref, wantState)) {
						OutlineEntry e;
						e.ref        = RE::NiPointer<RE::TESObjectREFR>{ c->ref };
						e.reassertMs = nextMs;
						g_state.outlined[c->ref] = std::move(e);
					}
				} else if (a_nowMs >= it->second.reassertMs) {
					if (budget == 0) {
						++g_state.opsDeferred;
						continue;
					}
					--budget;
					++g_state.opsThisScan;
					if (OutlineRef(c->ref, wantState)) {
						it->second.reassertMs = nextMs;
					} else {
						// 挂不上（管理器又没了）：本轮撤账，下轮重新试
						it = g_state.outlined.erase(it);
					}
				}
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
		// 扫描
		// ====================================================================
		void Rescan(std::uint64_t a_nowMs, RE::PlayerCharacter* a_player)
		{
			auto* cell = a_player->parentCell;
			if (!cell) {
				return;
			}

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
				// 长度突变（或换了个偏移）= 加载线程还在往数组里塞引用 → 本轮先不动
				if (arr.size != g_state.lastRefsSize || usedOff != g_state.cellRefsOff) {
					g_state.lastRefsSize = arr.size;
					g_state.cellRefsOff  = usedOff;
					g_state.stableRounds = 0;
					return;
				}
				// 长度连续稳定若干轮才认为世界已经稳定下来
				if (++g_state.stableRounds < kRefsStableRounds) {
					return;
				}
			}

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

			// ★ v2.3：遍历 + 排序 + 挑选这一整段（纯内存读，理论上应该在 1ms 量级；
			//   实测却是 30ms 上下，所以必须单独计时把它和形状校验分开看）。
			{
				PhaseTimer tLoop{ &g_state.tLoopMs, &g_state.tLoopMax };

			auto* const*        list  = reinterpret_cast<RE::TESObjectREFR* const*>(arr.data);
			const std::uint32_t count = std::min<std::uint32_t>(arr.size, kMaxRefsSanity);

			for (std::uint32_t i = 0; i < count; ++i) {
				auto* ref = list[i];
				// 全部是纯内存读，零引擎调用
				if (!ref || !IsPlausiblePointer(reinterpret_cast<std::uint64_t>(ref))) {
					continue;
				}
				if (ref == a_player || ref->IsDeleted() || ref->IsDisabled() || ref->IsPlayerRef()) {
					continue;
				}
				if (ref->parentCell != cell) {
					continue;
				}
				// ★ 先判距离再判类型：这样「白名单没通过」的统计只在半径内做，
				//   日志里的类型统计才是有意义的（否则满屏都是墙和地形的类型）。
				const float d2 = origin.GetSquaredDistance(ref->data.location);
				if (d2 > radius2) {
					continue;
				}

				const auto* base = ref->data.objectReference.get();
				if (!IsHighlightableBase(base)) {
					// 诊断：半径内、类型不在白名单 → 记一笔类型直方图（见统计日志 rejTypes=）
					if (base) {
						++g_state.rejectTypes[static_cast<std::uint8_t>(base->GetFormType()) & 0xFF];
					}
					continue;
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

				Candidate c;
				c.ref = ref;
				c.d2  = d2;
				if (g_cfg.highlightMode == 1) {
					// 原生模式：已经在 outline 表里就是「亮着」（黏性靠它）
					c.lit = g_state.outlined.find(ref) != g_state.outlined.end();
				} else if (const auto it = g_state.glowing.find(ref); it != g_state.glowing.end()) {
					c.lit = it->second.dueMs > a_nowMs;
				}
				// 已发光的按 0.5 折扣参与排序 = 黏性：站着不动目标不抖，
				// 走动时近的新目标仍然能顶掉远的旧目标。
				c.key = c.lit ? d2 * 0.5f : d2;
				cands.push_back(c);
			}

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
			}  // ← tLoop 计时块结束
			// ------------------------------------------------------------------
			// ★ 视觉层
			// ------------------------------------------------------------------
			if (g_cfg.highlightMode == 1) {
				// 原生 outline：引擎自己画描边（和原版扫描仪同一套），
				// 不需要 Papyrus、不需要 EFSH、也不需要令牌桶。
				{
					PhaseTimer tSync{ &g_state.tSyncMs, &g_state.tSyncMax };
					SyncNativeOutline(chosen, a_nowMs);
				}
				return;
			}

			// ---- 旧方案（HighlightMode=0）：走 Papyrus 桥 Play 一条 EFSH ----
			if (cands.empty()) {
				return;
			}

			// 令牌桶：稳态 opsPerSecond 条/秒，桶容量 bucketSize（换场景时的快速铺满额度）
			const double dt = (a_nowMs - g_state.lastOpMs) / 1000.0;
			g_state.lastOpMs = a_nowMs;
			g_state.tokens   = std::min(g_cfg.bucketSize, g_state.tokens + std::max(0.0, dt) * g_cfg.opsPerSecond);

			for (auto* c : chosen) {
				if (c->lit) {
					continue;  // 还在有效期内，完全不碰（零 op）
				}
				if (g_state.tokens < 1.0) {
					break;
				}
				if (!PushTo(g_state.opList, c->ref)) {
					break;  // 背压
				}
				g_state.tokens -= 1.0;
				++g_state.playedCount;

				// 记进发光集合（同时保活）。到期时刻向前抖 0~30 秒：
				// 向后抖会让每个目标出现一段黑暗期，向前抖只是偶尔两个实例重叠（有界）。
				GlowEntry e;
				e.ref   = RE::NiPointer<RE::TESObjectREFR>{ c->ref };
				e.dueMs = a_nowMs +
				          static_cast<std::uint64_t>((g_cfg.glowDurationSec + 2.0f) * 1000.0f) -
				          static_cast<std::uint64_t>(std::rand() % 30000);
				g_state.glowing[c->ref] = std::move(e);
			}

			// 清掉已经过期的表项（只删表项，不发 Stop —— 走远的目标由 effect 自行过期）
			for (auto it = g_state.glowing.begin(); it != g_state.glowing.end();) {
				if (it->second.dueMs + 60000 < a_nowMs || !it->second.ref) {
					it = g_state.glowing.erase(it);
				} else {
					++it;
				}
			}
		}

		// 关掉开关时，把待熄灭的那批分批喂给 StopList。
		// ★ 只推进「已推送」的下标，**不释放 NiPointer** —— 释放的时机是
		//   「信箱被桥消费完（MaybeClear 返回真）且全部推完」，见 ReleaseSatisfiedStops()。
		void PumpPendingStop()
		{
			if (!g_state.stopList || g_state.pendingStopSent >= g_state.pendingStop.size()) {
				return;
			}
			auto& arr = g_state.stopList->arrayOfForms;
			while (g_state.pendingStopSent < g_state.pendingStop.size() && arr.size() < kMaxMailbox) {
				const auto& p = g_state.pendingStop[g_state.pendingStopSent];
				if (p) {
					arr.push_back(p.get());
					++g_state.stoppedCount;
				}
				++g_state.pendingStopSent;
			}
		}

		// 熄灭这批彻底走完之后才释放保活引用
		void ReleaseSatisfiedStops(bool a_stopMailboxCleared)
		{
			if (!a_stopMailboxCleared || g_state.pendingStopSent < g_state.pendingStop.size()) {
				return;
			}
			g_state.pendingStop.clear();
			g_state.pendingStopSent = 0;
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

			TickRetired(now);
			PollHotkey(now);

			// 原生模式下视觉完全在引擎里，**不需要** ESM / FLST 信箱 / Papyrus 桥，
			// 所以绑定失败也照跑（少一个能坏的地方）；旧方案必须有信箱。
			const bool legacy = (g_cfg.highlightMode != 1);
			if (!g_state.bound) {
				if (!BindForms() && legacy) {
					return;
				}
			}

			// 读档自愈：脚本把 Epoch +1，这里看到变化就整批重置
			double epochVal = 0.0;
			if (ReadGlobalValue(g_state.epoch, epochVal)) {
				const auto cur = static_cast<std::uint32_t>(epochVal);
				if (!g_state.epochSeen) {
					g_state.epochSeen = true;
					g_state.lastEpoch = cur;
				} else if (cur != g_state.lastEpoch) {
					g_state.lastEpoch = cur;
					// 读档：引用数组此刻正在被重建，重置稳定性判据并静置
					g_state.cellRefsOff   = 0;
					g_state.lastRefsSize  = 0;
					g_state.stableRounds  = 0;
					g_state.settleUntilMs = now + kSettleAfterSceneChangeMs;
					InvalidateReadRegions();
					ResetForNewScene(now, "epoch changed (load game)");
				}
			}

			// 清表（两个信箱都只在桥消费完之后清）—— 原生模式下没有信箱
			if (legacy) {
				MaybeClear(g_state.opList, g_state.opCursor);
				ReleaseSatisfiedStops(MaybeClear(g_state.stopList, g_state.stopCursor));
			}

			if (!g_state.on) {
				if (legacy) {
					PumpPendingStop();
				}
				return;
			}

			if (legacy) {
				PumpPendingStop();
			}

			// 扫描节流
			if (now - g_state.lastScanMs < static_cast<std::uint64_t>(g_cfg.scanIntervalMs)) {
				return;
			}
			g_state.lastScanMs = now;

			// ★ v2.2：载入画面期间不做任何引擎调用（哪怕只是「摘掉上一批」）。
			//   载入时主线程在跑加载，我们每条调用都会被拖慢几十毫秒，
			//   几百条叠起来就是实测日志里那个 18 秒的空档。
			g_state.loadingNow = IsLoadingScreenUp();
			if (g_state.loadingNow) {
				return;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			auto* cell = player->parentCell;
			if (!cell) {
				g_state.lastCell = nullptr;
				return;
			}
			if (cell != g_state.lastCell) {
				const bool first = (g_state.lastCell == nullptr);
				g_state.lastCell = cell;
				// 换 cell / 第一次进入世界：重置引用数组稳定性判据，并静置一段
				// 时间 —— 引擎此刻正在重建 3D 和引用数组，parentCell 是半初始化状态
				// （上一代项目 v18 就是在这里崩的）。
				g_state.cellRefsOff   = 0;
				g_state.lastRefsSize  = 0;
				g_state.stableRounds  = 0;
				g_state.settleUntilMs = now + kSettleAfterSceneChangeMs;
				InvalidateReadRegions();  // v2.3：旧 cell 的「可读区间」不再可信
				ResetForNewScene(now, first ? "first cell" : "cell changed");
			}
			if (now < g_state.settleUntilMs) {
				return;  // 静置期内不扫描
			}

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
				REX::INFO("scan#{} cell={:08X} off=0x{:X} refs={} cand={} sel={} lit={} outline={}/{}/{} played={} stopped={} play={}/{} stop={}/{} tok={:.1f} bp={} rej={} on={} retired={}",
					g_state.scanCount,
					cell->GetFormID(),
					g_state.cellRefsOff,
					g_state.lastRefsSize,
					g_state.candCount,
					g_state.selCount,
					g_state.glowing.size(),
					g_state.outlined.size(),
					g_state.nativeReady ? 1 : 0,
					CountLiveManagers(),
					g_state.playedCount,
					g_state.stoppedCount,
					g_state.opList ? g_state.opList->arrayOfForms.size() : 0,
					g_state.opCursor ? g_state.opCursor->value : 0.0f,
					g_state.stopList ? g_state.stopList->arrayOfForms.size() : 0,
					g_state.stopCursor ? g_state.stopCursor->value : 0.0f,
					g_state.tokens,
					g_state.backpressure,
					g_state.refsRejected,
					g_state.on ? 1 : 0,
					g_state.retired.size());

				// 摘除计数（诊断）：rmOk 应随 sel/outline 变化一起增长，
				// unhMiss 长期增长 = 连引擎自己的摘除函数都没动到管理器里的东西。
				// ★ v2.3：graphRemove=1 才说明「3D 图 visitor」那条路可用（这是唯一真正
				//   有效的摘除路径）。mapCnt = 当前状态管理器哈希表里的元素数，它应该
				//   跟着 outline 一起涨落 —— 只涨不落就说明还有摘不掉的残留。
				REX::INFO("  outline remove: rmOk={} unhMiss={} removeMiss={} removeReady={} unhighlightReady={} graphRemove={} mapCnt={}",
					g_state.outlineRemoved, g_state.outlineUnhighlightMiss,
					g_state.outlineRemoveMiss,
					g_state.nativeRemoveReady ? 1 : 0,
					g_outlineUnhighlightReady ? 1 : 0,
					g_outlineGraphRemoveReady ? 1 : 0,
					ManagerMapCount(OutlineManagerFor(static_cast<std::uint32_t>(g_cfg.outlineState))));

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

				// Papyrus 侧自报状态：guideHb 不再增长 = 脚本没跑；
				// guideState 0/2/3 的含义见 SAS_Bridge.psc 的说明。
				REX::INFO("  papyrus: guideHb={} guideState={} guideMarkers={}",
					g_state.guideHb ? static_cast<int>(g_state.guideHb->value) : -1,
					g_state.guideState ? static_cast<int>(g_state.guideState->value) : -1,
					g_state.guideMarkers ? static_cast<int>(g_state.guideMarkers->value) : -1);

				// 半径内、base 类型不在白名单的分布（上个统计窗口）。
				// 用途：发现「某个东西该亮却没亮」时，看它的 formType 是多少。
				{
					std::vector<std::pair<std::uint32_t, std::uint8_t>> hist;
					for (std::uint32_t ft = 0; ft < g_state.rejectTypes.size(); ++ft) {
						if (g_state.rejectTypes[ft]) {
							hist.emplace_back(g_state.rejectTypes[ft], static_cast<std::uint8_t>(ft));
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
						REX::INFO("  rejTypes (半径内但类型不在白名单，formType x 次数): {}", s);
					}
					g_state.rejectTypes.fill(0);
				}

				// 管理器哈希表的家底（每 30 秒一次就够，用于确认「键到底是什么」）。
				if (now - g_state.lastMapDiagMs > 30000) {
					g_state.lastMapDiagMs = now;
					LogManagerMapDiag("runtime", static_cast<std::uint32_t>(g_cfg.outlineState));
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
		g_state.tokens = g_cfg.bucketSize;

		// 原生 outline 的三个引擎函数（RVA 是 1.16.244.0 实测值，会做签名校验）
		ResolveNativeOutline();
		if (g_cfg.highlightMode == 1 && !g_state.nativeReady) {
			REX::WARN("HighlightMode=native 但原生 outline 不可用（签名不匹配）"
					  " -> 本次运行不会高亮。可把 INI 的 HighlightMode 改成 0 走旧方案。");
		}

		task->AddPermanentTask(Tick);

		REX::INFO("SAS_AlwaysScan installed (main thread {})", g_mainThreadId.load());
		return true;
	}
}
