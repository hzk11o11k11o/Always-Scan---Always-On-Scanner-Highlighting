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
#include "RE/B/BSContainer.h"
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

		constexpr std::uint64_t kStatsLogIntervalMs = 5000;
		constexpr std::uint64_t kBindWarnIntervalMs = 5000;
		constexpr int           kTierWidthMeters    = 2;  // 距离分档宽度
		constexpr int           kPerTier            = 6;  // 每档最多几个目标

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
		};

		Config               g_cfg;
		State                g_state;
		std::mutex           g_tickLock;
		std::atomic_uint32_t g_mainThreadId{ 0 };
		std::string          g_iniPath;

		// ====================================================================
		// 小工具
		// ====================================================================
		std::uint64_t NowMs()
		{
			using namespace std::chrono;
			return static_cast<std::uint64_t>(
				duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
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

			REX::INFO("config: radius={:.1f}m duration={:.0f}s targets={} hotkeyVK=0x{:X} startEnabled={}",
				g_cfg.radiusMeters, g_cfg.glowDurationSec, g_cfg.maxTargets,
				g_cfg.hotkeyVk, g_cfg.startEnabled);
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
			REX::INFO("reset ({}) play={} stop={} glow={}",
				a_reason,
				g_state.opList ? g_state.opList->arrayOfForms.size() : 0,
				g_state.stopList ? g_state.stopList->arrayOfForms.size() : 0,
				g_state.glowing.size());

			// 顺序很重要：先丢信箱（裸指针），再释放保活引用，否则桥可能 Stop 到悬空对象
			ClearMailbox(g_state.opList);
			ClearMailbox(g_state.stopList);
			g_state.pendingStop.clear();
			g_state.pendingStopSent = 0;
			RetireGlowTable(a_nowMs);
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
		// 扫描
		// ====================================================================
		struct Candidate
		{
			RE::TESObjectREFR* ref = nullptr;
			float              d2  = 0.0f;  // 世界单位平方距离
			bool               lit = false; // 已在发光集合里且未到期
			float              key = 0.0f;  // 排序键
		};

		void Rescan(std::uint64_t a_nowMs, RE::PlayerCharacter* a_player)
		{
			auto* cell = a_player->parentCell;
			if (!cell || !cell->IsAttached()) {
				return;
			}

			const RE::NiPoint3 origin  = a_player->GetPosition();
			const float        radiusU = g_cfg.radiusMeters * kUnitsPerMeter;
			const float        radius2 = radiusU * radiusU;

			static std::vector<Candidate> cands;
			cands.clear();
			cands.reserve(512);

			cell->ForEachReference([&](const RE::NiPointer<RE::TESObjectREFR>& a_refPtr) {
				auto* ref = a_refPtr.get();
				if (!ref) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				// 纯内存读，零引擎调用
				if (ref->IsDeleted() || ref->IsDisabled() || ref->IsPlayerRef()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (ref->parentCell != cell) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (!IsHighlightableBase(ref->data.objectReference.get())) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const float d2 = origin.GetSquaredDistance(ref->data.location);
				if (d2 > radius2) {
					return RE::BSContainer::ForEachResult::kContinue;
				}

				Candidate c;
				c.ref = ref;
				c.d2  = d2;
				if (const auto it = g_state.glowing.find(ref); it != g_state.glowing.end()) {
					c.lit = it->second.dueMs > a_nowMs;
				}
				// 已发光的按 0.5 折扣参与排序 = 黏性：站着不动目标不抖，
				// 走动时近的新目标仍然能顶掉远的旧目标。
				c.key = c.lit ? d2 * 0.5f : d2;
				cands.push_back(c);
				return RE::BSContainer::ForEachResult::kContinue;
			});

			g_state.candCount = cands.size();
			if (cands.empty()) {
				return;
			}

			std::sort(cands.begin(), cands.end(),
				[](const Candidate& a, const Candidate& b) { return a.key < b.key; });

			// 距离分档：每 2 米一档、每档最多 kPerTier 个。
			// 只取「最近的 N 个」会让目标全挤在脚边（上一代 v27/v28 的实测教训）。
			std::vector<Candidate*> chosen;
			chosen.reserve(static_cast<std::size_t>(g_cfg.maxTargets));
			std::unordered_map<int, int> tierUsed;
			const float                  tierU     = static_cast<float>(kTierWidthMeters) * kUnitsPerMeter;
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
			g_state.selCount = chosen.size();

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
				ResetForNewScene(now, first ? "first cell" : "cell changed");
			}

			Rescan(now, player);
			++g_state.scanCount;

			if (g_cfg.logStats && now - g_state.lastStatsMs > kStatsLogIntervalMs) {
				g_state.lastStatsMs = now;
				REX::INFO("scan#{} cell={:08X} cand={} sel={} lit={} played={} stopped={} play={}/{} stop={}/{} tok={:.1f} bp={} on={} retired={}",
					g_state.scanCount,
					cell->GetFormID(),
					g_state.candCount,
					g_state.selCount,
					g_state.glowing.size(),
					g_state.playedCount,
					g_state.stoppedCount,
					g_state.opList->arrayOfForms.size(),
					g_state.opCursor->value,
					g_state.stopList->arrayOfForms.size(),
					g_state.stopCursor->value,
					g_state.tokens,
					g_state.backpressure,
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

		task->AddPermanentTask(Tick);

		REX::INFO("SAS_AlwaysScan installed (main thread {})", g_mainThreadId.load());
		return true;
	}
}
