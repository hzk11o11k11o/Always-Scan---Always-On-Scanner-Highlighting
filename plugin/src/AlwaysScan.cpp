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
#include "RE/N/NiPoint.h"
#include "RE/N/NiSmartPointer.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESGlobal.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/T/TESObjectREFR.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
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
		constexpr int           kTierWidthMeters    = 2;  // 距离分档宽度
		constexpr int           kPerTier            = 6;  // 每档最多几个目标

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
			float         glowDurationSec = 90.0f;
			int           maxTargets      = 160;
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
			// 每隔多久重新确认一次高亮（毫秒）。引擎换场景/退出扫描模式时会
			// 清掉自己的高亮管理器，所以需要周期性重申。
			int           reassertMs     = 3000;
			// 1 = 引擎的 12 个 HighlightManager 不存在时，自己调用引擎的
			//     「重建管理器」函数把它们建出来（不开扫描仪时引擎就不会建）。
			bool          autoEnsureManagers = true;
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
			bool          nativeReady      = false;  // 三个引擎函数都已按签名校验通过
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
			double        tokens        = 0.0;
			std::uint64_t scanCount     = 0;
			std::uint64_t playedCount   = 0;
			std::uint64_t stoppedCount  = 0;
			std::uint64_t tickCount     = 0;
			std::uint64_t candCount     = 0;
			std::uint64_t selCount      = 0;
			std::uint64_t backpressure  = 0;

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

		// 前置声明：定义在后面的「原生 outline」小节里（SetOn 要先用到）
		void ClearAllNativeOutline();

		// ====================================================================
		// 小工具
		// ====================================================================
		std::uint64_t NowMs()
		{
			using namespace std::chrono;
			return static_cast<std::uint64_t>(
				duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
		}

		// ----------------------------------------------------------------
		// 内存安全小工具
		// 凡是「按实测偏移读出来的指针 / 容器」，用之前一律先用它们验证；
		// 不通过就放弃（宁可这一轮不扫描，绝不乱读内存）。
		// ----------------------------------------------------------------
		bool IsPlausiblePointer(std::uint64_t a_ptr)
		{
			return a_ptr > 0x10000ULL && a_ptr < 0x7FFFFFFFFFFFULL;
		}

		bool IsReadable(const void* a_ptr, std::size_t a_len)
		{
			if (!IsPlausiblePointer(reinterpret_cast<std::uint64_t>(a_ptr))) {
				return false;
			}
			MEMORY_BASIC_INFORMATION mbi{};
			if (::VirtualQuery(a_ptr, &mbi, sizeof(mbi)) == 0) {
				return false;
			}
			if (mbi.State != MEM_COMMIT) {
				return false;
			}
			if (mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) {
				return false;
			}
			const auto start = reinterpret_cast<std::uintptr_t>(a_ptr);
			const auto base  = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
			return (start + a_len) <= (base + mbi.RegionSize);
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
			g_cfg.maxTargets     = std::clamp(getInt("MaxTargets", 160), 8, 512);
			g_cfg.scanIntervalMs = std::clamp(getInt("ScanIntervalMs", 200), 50, 2000);
			g_cfg.logStats       = getInt("LogStats", 1) != 0;
			g_cfg.radiusMeters   = std::clamp(getFloat("RadiusMeters", 50.0f), 5.0f, 500.0f);
			g_cfg.glowDurationSec = std::clamp(getFloat("GlowDurationSecs", 90.0f), 5.0f, 600.0f);
			g_cfg.opsPerSecond   = std::clamp(static_cast<double>(getFloat("OpsPerSecond", 8.0f)), 0.5, 60.0);
			g_cfg.bucketSize     = std::clamp(static_cast<double>(getFloat("BucketSize", 80.0f)), 4.0, 512.0);

			g_cfg.highlightMode = std::clamp(getInt("HighlightMode", 1), 0, 1);
			g_cfg.outlineState  = std::clamp(getInt("OutlineState", 0), 0, 11);
			g_cfg.onlyInFront   = getInt("OnlyInFront", 1) != 0;
			g_cfg.frontFovDeg   = std::clamp(getFloat("FrontFovDeg", 110.0f), 20.0f, 360.0f);
			g_cfg.reassertMs    = std::clamp(getInt("ReassertMs", 3000), 500, 60000);
			g_cfg.autoEnsureManagers = getInt("AutoEnsureManagers", 1) != 0;

			REX::INFO("config: radius={:.1f}m duration={:.0f}s targets={} hotkeyVK=0x{:X} startEnabled={}",
				g_cfg.radiusMeters, g_cfg.glowDurationSec, g_cfg.maxTargets,
				g_cfg.hotkeyVk, g_cfg.startEnabled);
			REX::INFO("config: highlightMode={} outlineState={} onlyInFront={} frontFov={:.0f}deg reassert={}ms autoEnsure={}",
				g_cfg.highlightMode == 1 ? "native-outline" : "legacy-efsh",
				g_cfg.outlineState, g_cfg.onlyInFront, g_cfg.frontFovDeg, g_cfg.reassertMs, g_cfg.autoEnsureManagers);
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
			g_state.bound      = true;

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

			// 换场景后引擎那边的高亮管理器可能已经换了一批，我们只清自己的账，
			// 不做「逐个摘掉」——原 cell 的引用已经不可靠了，逐个反查反而危险。
			g_state.outlined.clear();
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
				// 原生 outline：直接把这批引用从引擎的高亮表里摘掉（状态写回 12）
				ClearAllNativeOutline();
			}

			if (!a_on) {
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
		// 引擎自己的「确保 12 个管理器都存在并把配色参数刷成当前 GMST 值」函数。
		// 不开扫描仪时引擎可能还没建它们，我们就自己叫一次（幂等）。
		constexpr std::uintptr_t kRvaOutlineEnsureManagers = 0x17D47B0;

		// 状态 12 = 「无高亮」（0..11 才是真实样式）
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

		using OutlineLookupOrAdd_t = std::uint32_t* (*)(void*, RE::TESObjectREFR**);
		using OutlineSet_t         = void (*)(RE::TESObjectREFR**, std::uint32_t);
		using OutlineClear_t       = void (*)(void*, RE::TESObjectREFR**);
		using OutlineEnsure_t      = void (*)(void*);

		OutlineLookupOrAdd_t g_outlineLookupOrAdd  = nullptr;
		OutlineSet_t         g_outlineSet          = nullptr;
		OutlineClear_t       g_outlineClear        = nullptr;
		OutlineEnsure_t      g_outlineEnsure       = nullptr;
		std::uintptr_t       g_outlineManagerArray = 0;

		// 前置声明（定义在 ResolveNativeOutline 之后）
		std::uintptr_t OutlineManagerFor(std::uint32_t a_state);
		void           LogManagerDiagnostics(const char* a_tag);
		bool           EnsureManagerFor(std::uint32_t a_state);

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

			REX::INFO("native outline ready: lookupOrAdd=+0x{:X} set=+0x{:X} clear=+0x{:X} ensure=+0x{:X} managers=+0x{:X} (sig verified)",
				kRvaOutlineLookupOrAdd, kRvaOutlineSet, kRvaOutlineClear,
				kRvaOutlineEnsureManagers, kRvaOutlineManagers);
			LogManagerDiagnostics("install");
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
			for (std::uint32_t i = 0; i < kOutlineManagerCount; ++i) {
				if (OutlineManagerFor(i)) {
					++n;
				}
			}
			return n;
		}

		void LogManagerDiagnostics(const char* a_tag)
		{
			REX::INFO("native outline managers[{}] = {}/{} alive (state {} -> {})",
				a_tag, CountLiveManagers(), kOutlineManagerCount,
				g_cfg.outlineState, OutlineManagerFor(static_cast<std::uint32_t>(g_cfg.outlineState)) ? "ok" : "NULL");
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

		// 摘掉原生 outline（状态写回 12 = 无）
		void UnoutlineRef(RE::TESObjectREFR* a_ref)
		{
			if (!g_state.nativeReady || !a_ref) {
				return;
			}
			RE::TESObjectREFR* slot = a_ref;
			if (auto* p = g_outlineLookupOrAdd(nullptr, &slot); p && IsReadable(p, sizeof(std::uint32_t))) {
				*p = kOutlineStateNone;
			}
			g_outlineClear(nullptr, &slot);
		}

		// 原生模式下：把选中的目标挂上高亮，把掉队的目标摘掉。
		// 只有「新目标」和「距离上次重申超过 ReassertMs 的目标」才会真正调引擎
		// （每轮都全量调会白白吃掉主线程）。
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

			// 掉队的：摘掉
			for (auto it = g_state.outlined.begin(); it != g_state.outlined.end();) {
				if (chosenSet.find(it->first) == chosenSet.end()) {
					UnoutlineRef(it->second.ref.get());
					it = g_state.outlined.erase(it);
				} else {
					++it;
				}
			}

			// 新目标 / 到重申时刻的
			const auto wantState = static_cast<std::uint32_t>(g_cfg.outlineState);
			const auto reassert  = static_cast<std::uint64_t>(g_cfg.reassertMs);
			for (auto* c : a_chosen) {
				auto it = g_state.outlined.find(c->ref);
				if (it == g_state.outlined.end()) {
					if (OutlineRef(c->ref, wantState)) {
						OutlineEntry e;
						e.ref        = RE::NiPointer<RE::TESObjectREFR>{ c->ref };
						e.reassertMs = a_nowMs + reassert;
						g_state.outlined[c->ref] = std::move(e);
					}
				} else if (a_nowMs >= it->second.reassertMs) {
					if (OutlineRef(c->ref, wantState)) {
						it->second.reassertMs = a_nowMs + reassert;
					} else {
						// 挂不上（管理器又没了）：本轮撤账，下轮重新试
						it = g_state.outlined.erase(it);
					}
				}
			}
		}

		// 把当前所有已挂高亮的引用全部摘掉（关开关）
		void ClearAllNativeOutline()
		{
			if (g_state.outlined.empty()) {
				return;
			}
			const auto n = g_state.outlined.size();
			for (auto& [key, entry] : g_state.outlined) {
				UnoutlineRef(entry.ref.get());
			}
			g_state.outlined.clear();
			REX::INFO("native outline cleared (n={})", n);
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
			RawArray    arr{};
			std::size_t usedOff = 0;
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

			const RE::NiPoint3 origin  = a_player->GetPosition();
			const float        radiusU = g_cfg.radiusMeters * kUnitsPerMeter;
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

			static std::vector<Candidate> cands;
			cands.clear();
			cands.reserve(512);

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
				if (!IsHighlightableBase(ref->data.objectReference.get())) {
					continue;
				}
				const float d2 = origin.GetSquaredDistance(ref->data.location);
				if (d2 > radius2) {
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

			// 距离分档：每 2 米一档、每档最多 kPerTier 个。
			// 只取「最近的 N 个」会让目标全挤在脚边（上一代 v27/v28 的实测教训）。
			std::vector<Candidate*> chosen;
			chosen.reserve(static_cast<std::size_t>(g_cfg.maxTargets));
			if (!cands.empty()) {
				std::unordered_map<int, int> tierUsed;
				const float                  tierU = static_cast<float>(kTierWidthMeters) * kUnitsPerMeter;
				for (auto& c : cands) {
					if (chosen.size() >= static_cast<std::size_t>(g_cfg.maxTargets)) {
						break;
					}
					const int tier = static_cast<int>(std::sqrt(c.d2) / tierU);
					if (tierUsed[tier] >= kPerTier) {
						continue;
					}
					++tierUsed[tier];
					chosen.push_back(&c);
				}
			}
			g_state.selCount = chosen.size();

			// ------------------------------------------------------------------
			// ★ 视觉层
			// ------------------------------------------------------------------
			if (g_cfg.highlightMode == 1) {
				// 原生 outline：引擎自己画描边（和原版扫描仪同一套），
				// 不需要 Papyrus、不需要 EFSH、也不需要令牌桶。
				SyncNativeOutline(chosen, a_nowMs);
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

			if (!g_state.bound && !BindForms()) {
				return;
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
					ResetForNewScene(now, "epoch changed (load game)");
				}
			}

			// 清表（两个信箱都只在桥消费完之后清）
			MaybeClear(g_state.opList, g_state.opCursor);
			ReleaseSatisfiedStops(MaybeClear(g_state.stopList, g_state.stopCursor));

			if (!g_state.on) {
				PumpPendingStop();
				return;
			}

			PumpPendingStop();

			// 扫描节流
			if (now - g_state.lastScanMs < static_cast<std::uint64_t>(g_cfg.scanIntervalMs)) {
				return;
			}
			g_state.lastScanMs = now;

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
				ResetForNewScene(now, first ? "first cell" : "cell changed");
			}
			if (now < g_state.settleUntilMs) {
				return;  // 静置期内不扫描
			}

			Rescan(now, player);
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
					g_state.opList->arrayOfForms.size(),
					g_state.opCursor->value,
					g_state.stopList->arrayOfForms.size(),
					g_state.stopCursor->value,
					g_state.tokens,
					g_state.backpressure,
					g_state.refsRejected,
					g_state.on ? 1 : 0,
					g_state.retired.size());
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
