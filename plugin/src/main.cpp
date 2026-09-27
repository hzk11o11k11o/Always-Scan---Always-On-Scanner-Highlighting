#include "PCH.h"

#include "AlwaysScan.h"

#include <atomic>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>

#include "REX/CONVERT.h"  // REX::UTF16_TO_UTF8（日志里打印真实路径）

#include <spdlog/details/file_helper.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/spdlog.h>

// 注意：SFSE_PLUGIN_VERSION 由 commonlibsf.plugin 规则生成的 commonlibsf-plugin.cpp
// 提供（配置见 plugin/xmake.lua 的 add_rules 参数），这里不要再定义一份，
// 否则会报 LNK2005 重复定义。

namespace
{
	std::atomic_bool g_installed{ false };

	// 日志名（落到 <esm 同级目录>\SAS_AlwaysScan.log；该目录不可写时回退到
	// SFSE 默认的 SFSE\Logs\<name>.log）。与下面 SFSE::Init 共用同一个常量，
	// 避免两处写死不一致。
	constexpr const char* kLogName = "SAS_AlwaysScan";

	// 日志文件上限 10 MiB：写新一行时若会超过就把旧内容整体清空，
	// 保证日志占用恒定 ~10 MiB（不是滚动保留旧文件）。
	constexpr std::size_t kLogMaxBytes = 10 * 1024 * 1024;

	// 「单文件封顶」文件 sink。commonlibsf 默认建的是 basic_file_sink
	// （纯追加、永不清理），长时间游玩日志会无限变大；本 sink 在写入前检查
	// 累计字节数，超限就截断重开（file_helper::reopen(true)），从空文件重新累积。
	class SizeLimitedFileSink final : public spdlog::sinks::base_sink<std::mutex>
	{
	public:
		SizeLimitedFileSink(const std::filesystem::path& a_path, std::size_t a_maxBytes) :
			_maxBytes(a_maxBytes)
		{
#ifdef SPDLOG_WCHAR_FILENAMES
			_file.open(a_path.wstring(), false);  // 追加打开：保留上次运行的内容
#else
			_file.open(a_path.string(), false);
#endif
			_bytesWritten = _file.size();
		}

	protected:
		void sink_it_(const spdlog::details::log_msg& a_msg) override
		{
			spdlog::memory_buf_t formatted;
			formatter_->format(a_msg, formatted);

			// 先判断后写：最多超出「单条消息」的字节数，之后从 0 重算。
			if (_bytesWritten + formatted.size() > _maxBytes) {
				_file.reopen(true);
				_bytesWritten = 0;
			}

			_file.write(formatted);
			_bytesWritten += formatted.size();
		}

		void flush_() override
		{
			_file.flush();
		}

	private:
		spdlog::details::file_helper _file;
		std::size_t                   _maxBytes;
		std::size_t                   _bytesWritten{};
	};

	// 本插件 DLL 所在目录（MO2 下 = mod 目录里的 SFSE\Plugins\；
	// 手动安装 = <游戏>\Data\SFSE\Plugins\）。
	// 用当前模块的 ImageBase 反查（REX::W32::GetCurrentModule 就是 &__ImageBase），
	// 精确到本 DLL —— 不依赖 SFSE 接口，也不怕同时加载了多个插件。
	std::filesystem::path PluginDir()
	{
		wchar_t buf[1024]{};
		const std::uint32_t n = REX::W32::GetModuleFileNameW(
			REX::W32::GetCurrentModule(), buf, static_cast<std::uint32_t>(std::size(buf)));
		if (n == 0 || n >= std::size(buf)) {
			return {};
		}
		return std::filesystem::path{ buf, buf + n }.parent_path();
	}

	// esm 所在目录 = <游戏>\Data（本 DLL 在 Data\SFSE\Plugins\ 下，上溯两级）。
	//
	// ★ 需求（AGENTS.md）：日志文件生成在**和 esm 同级目录**里 ⇒ 就写这里。
	//   MO2 下 usvfs 只把「写 mod 目录里**已存在**的文件」重定向回 mod 目录，
	//   全新的文件会被丢进 overwrite —— 所以部署脚本（tools/build-sas.ps1）会
	//   在 mod 根预置一个空的 SAS_AlwaysScan.log，日志才会真落在 esm 旁边；
	//   没预置（比如手动安装）时，日志仍写在 Data 根（内容一样能查到）。
	std::filesystem::path EsmDir()
	{
		const auto dllDir = PluginDir();
		if (dllDir.empty()) {
			return {};
		}
		return dllDir.parent_path().parent_path();
	}

	// 路径转 UTF-8（spdlog 的文本都是 UTF-8；非 ASCII 路径在中文系统上
	// 直接 .string() 会变 GBK 字节 —— 日志里要能对得上真实路径）。
	std::string ToUtf8(const std::filesystem::path& a_path)
	{
		std::string s;
		REX::UTF16_TO_UTF8(a_path.wstring(), s);
		return s;
	}

	// 接管日志：建「限长文件 sink」并把默认 logger 的 sink 全换成它。
	// 必须在 SFSE::Init 之后调用。
	//
	// ★ 需求（AGENTS.md）：日志写在**和 esm 同级**的目录（<游戏>\Data\，MO2 下
	//   = mod 目录根）；那里建不出文件时回退 SFSE 默认日志目录
	//   （Documents\My Games\Starfield\SFSE\Logs\）—— 保证任何时候都有日志可查。
	//
	// ★ 调用方传了 `.log = false`（不建 commonlibsf 默认的 Documents 日志），
	//   所以这里要自己把 level / flush_on / pattern 一并设好（值与 commonlibsf
	//   InitLog 的默认一致：debug 构建 Debug、发布 Info，写一行 flush 一次）。
	void ApplyLogSizeLimit()
	{
		auto* logger = spdlog::default_logger_raw();
		if (!logger) {
			return;
		}

		std::filesystem::path fileName{ kLogName };
		fileName += ".log";

		std::filesystem::path usedDir;
		std::shared_ptr<spdlog::sinks::sink> fileSink;
		if (const auto dir = EsmDir(); !dir.empty()) {
			try {
				fileSink = std::make_shared<SizeLimitedFileSink>(dir / fileName, kLogMaxBytes);
				usedDir = dir;
			} catch (const std::exception& e) {
				REX::WARN("esm 同级目录里建日志失败（{}）—— 回退到 SFSE 默认日志目录", e.what());
			}
		}
		if (!fileSink) {
			if (const auto dir = SFSE::log::log_directory()) {
				try {
					fileSink = std::make_shared<SizeLimitedFileSink>(*dir / fileName, kLogMaxBytes);
					usedDir = *dir;
				} catch (const std::exception&) {
				}
			}
		}
		if (!fileSink) {
			REX::WARN("日志文件建不出来（继续用 MSVC 调试输出；不影响任何功能）");
			return;
		}

		// clear() 会把 spdlog 自带的 stdout sink 换掉（GUI 进程里 stdout 本来也没用）。
		logger->sinks().clear();
		logger->sinks().push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
		logger->sinks().push_back(fileSink);

		// level / flush_on / pattern：值都对齐 commonlibsf InitLog 的默认。
#ifdef NDEBUG
		logger->set_level(spdlog::level::info);
		logger->flush_on(spdlog::level::info);
#else
		logger->set_level(spdlog::level::debug);
		logger->flush_on(spdlog::level::debug);
#endif
		spdlog::set_pattern("[%T.%e] [%=5t] [%L] %v");
		REX::INFO("日志文件：{}（上限 {} KiB，写满清空重来）", ToUtf8(usedDir / fileName), kLogMaxBytes / 1024);
	}

	// 插件加载时可能过早（SFSE 的任务系统还没起来），所以在
	// PostPostLoad / PostDataLoad 消息里再兜底试一次。
	void TryInstall()
	{
		if (g_installed.load()) {
			return;
		}
		if (SAS::Install()) {
			g_installed.store(true);
		}
	}

	void OnMessage(SFSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SFSE::MessagingInterface::kPostPostLoad:
		case SFSE::MessagingInterface::kPostDataLoad:
			TryInstall();
			break;
		default:
			break;
		}
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	// ★ `.log = false`：不要 commonlibsf 默认那个 Documents\My Games\Starfield\SFSE\Logs\
	//   文件 sink（需求要求日志在「和 esm 同级目录」，由 ApplyLogSizeLimit() 自己建）。
	SFSE::Init(a_sfse, { .log = false, .logName = kLogName });
	ApplyLogSizeLimit();

	// ★ 2026-09-27 订正 R2（公开版仍是 2.0，build 号按用户要求不提升）：
	//   ① 引擎状态表树头 RVA 0x5949CE0 → **0x5F39CE0**（手算 rip 相对地址时少看一位，R1）；
	//   ② 「青」记忆不再短路主判据 / 资源链（只有「绿」单向短路，R1）；
	//   ③ ★ **按物种（base）扩散**：任一实例被权威确认「已扫描」⇒ 同 species / 同资源
	//      的**所有**实例一起变绿（引擎知识库本来就是这一级）—— 治用户实测的
	//      「有些植物扫描后还是青，打开扫描仪再关闭才变绿」（R2）。
	//   这三条一起治「扫描后没变色」—— 启动日志里认这个括号即可确认跑的是订正版。
	// ★★★ 2026-09-27 深夜 订正 R5（公开版仍是 2.0、DLL build 仍是 5.1.0）：
	//   **「已扫描」记忆按存档隔离** —— 治用户实测的「未扫描星球资源直接显示绿色，
	//   特别是重新读档（未扫描时的存档）后问题非常严重」（docs/38）：
	//   · 读档边界 = 引擎自己的 `TESLoadGameEvent`（快速旅行 / 进门不算）；
	//   · 每次读档：记忆作用域 +1（只有「本存档内当场见证」的绿才算数）+ 清空按物种
	//     扩散表 + 用资源链（0x1597A50）复核并丢掉「本存档里必然没扫描」的条目；
	//   · 落盘记忆仍跨会话保留（本会话**第一次**读档时有效 = 重开游戏继续玩同一存档）；
	//   · 另外：**按物种（base）扩散只在同一颗星球（worldspace）内生效** ——
	//     v5.1.2 的全局扩散会让「别的星球上没扫过的同种资源」也变绿。
	// ★★★ 2026-09-27 订正 R6（公开版仍是 2.0、DLL build 仍是 5.1.0）：
	//   **「读档边界」改为证据驱动** —— 治用户实测的「传送切换地图后，已扫描物品
	//   变成青色，开启扫描仪再关闭又变回绿色」：
	//   · 在 R5 里引擎的 `TESLoadGameEvent` 被当作「读档」的无条件边界 —— 而实测
	//     它会**在部分传送 / 换世界空间时也发**（用户那次「传送」触发了读档 #2）；
	//   · R6：事件只当**候选**（且复核延后 4s 到世界稳定后做）—— 资源链复核
	//     **一个 base 都证伪不了** ⇒ 判定「传送 / 读同一存档」⇒ 记忆保持有效
	//     （作用域不推进、物种表不清、条目不丢）；有证伪才是真「读回更早的存档」；
	//   · 另外：复核时**跳过「只见证于别的世界空间」的 base**（跨世界空间查必然
	//     假证伪）+ 链「已扫描」的 base 会被「重见证」（恢复作用域）。
	//   回退：INI `FloraLoadBoundaryEvidence=0`（回到 R5 行为）。
	// ★★★ 2026-09-27 订正 R7（公开版仍是 2.0、DLL build 仍是 5.1.0）：
	//   **读档边界改用「游戏时间指纹」**（`Calendar::gameDaysPassed`）—— R6 的链证据
	//   口径实测**仍然误作废**（用户 2026-09-27 10:15 那一局：载入画面关闭 + 4s 的复核
	//   把 9 个 base 判成「本存档没扫描」并删掉 132 条记忆，而 10s 后引擎自己拿着扫描仪
	//   把**同一批** base 画成绿、80s 后资源链对同一个资源也答「已扫描」⇒ 「没扫描」
	//   这个否定答案在载入刚结束时**没有区分度**，见 docs/40）：
	//   · 每条记忆自带**学习时刻**（落盘行第 3 字段 = 游戏时间，天）；
	//   · 读档时把「本存档的游戏时间」当**锚点**：学习时刻 ≤ 锚点 ⇒ 记忆有效；
	//     > 锚点 ⇒ 属于更新的时间线 ⇒ 不在作用域（★ **不删条目**，换更新的存档就回来）；
	//   · 传送 / 继续同一存档 ⇒ 锚点几乎不动 ⇒ **什么都不作废**（本次修复的核心）；
	//   · 读回更早的存档 ⇒ 只有「未来」的条目失效，其余照常有效（比 R5 的
	//     「一律作废、植物靠举扫描仪重学」精准得多）。
	//   回退：INI `FloraSaveFingerprint=0`（回到 R6 链证据路径）；读不到游戏时间时也会自动回退。
	// ★★★ 2026-09-27 订正 R8（公开版仍是 2.0、DLL build 仍是 5.1.0）：
	//   **治「传送后已扫描物品变青」的剩余那一半**（R7 之后用户第三次报同一现象）——
	//   R7 的读档边界已经不再误作废（那一局 `作废=0 条`），但**按物种扩散被星球口径挡住** +
	//   **引用级记忆命中不了**（外景临时引用每次会话换 FormID），于是：
	//   · 在星球 A 本会话亲手扫过的物种，星图快速旅行到 B 后**引擎自己**（一举扫描仪）
	//     把同 species 实例画成绿 4/5 ⇒ 引擎的物种知识**跨星球生效**，我们却涂成青色；
	//   · 载入 / 传送后同一个物种的实例换了 FormID ⇒ 落盘记忆（按引用）命中不到。
	//   R8 两处改动（都在 `AlwaysScan.cpp`）：
	//     ① `FloraBaseKnown()` **默认不再按星球硬拒绝**（`FloraSpeciesPlanetScope` 默认 0 =
	//        引擎口径）；作用域仍由**存档边界**管（物种表会话级不落盘 + 读档按游戏时间锚点剪枝）；
	//     ② 新增 `SeedFloraBaseFromRefMemory()`：把**作用域内**的落盘绿条目按 base **播种**进
	//        物种表（启动载入后 + 每次读档剪枝后各一次）⇒ 同 species 的实例**不必等举扫描仪**
	//        直接是绿的。
	//   回退：INI `FloraSpeciesPlanetScope=1`（回到 R5 的严格口径，只为对照）。
	// ★★★ 2026-09-27 订正 R9（公开版仍是 2.0、DLL build 仍是 5.1.0）：
	//   **植物「已扫描」改为直读引擎自己的扫描进度表 + 默认抛弃自建记忆**（用户指令：
	//   「我们还是走植物也直接读游戏本身物件状态的方式，抛弃自己记忆的方法」）。
	//   · 新增 `QueryFloraScanProgressDirect/Cached`：复刻引擎给「已扫描」植物写 state 4/5
	//     时用的那条**纯查询**（`PlayerKnowledge` 物种槽 `percent`；`0x1307180(ref)` 取知识 ID +
	//     两级只读哈希 `0x24105D0`/`0x23467B0`，命中读 `[元素+0x20]`）⇒ **任何时刻、任何
	//     引用**（含外景运行时临时引用）都能问「这个物种在这个存档里扫过没」；
	//     `percent == 100` ⇒ 已扫描（绿）；结果按 **base** 缓存（物种级，引擎口径）。
	//   · 新增 `FloraUseMemory`（默认 **0 = 抛弃记忆**）：按引用记忆 / 按物种扩散 /
	//     落盘播种 / 「沿用旧结论」全部**不参与判定** —— 用户报的「所有星球资源固定
	//     显示已扫描绿」的主嫌就是 R8 的「跨星球放行 + 播种」把物种表大面积填充。
	//   · 保留的判据全部是「引擎自己的数据」：进度直读 + 状态表只读探针（4/5）+
	//     `GetOutlineState` + 资源链（LVLI）；全程签名 / 形状 / SEH 三重保护，任一环
	//     走不通自动回退（不影响高亮本身）。
	//   回退：INI `FloraEngineProgress=0`（关新判据）/ `FloraUseMemory=1`（回 v5.1.8 记忆口径）。
	REX::INFO("SAS_AlwaysScan v5.1.0 loading（订正 R9：植物「已扫描」直读引擎扫描进度表（PlayerKnowledge 物种槽 percent=100）+ 默认抛弃自建记忆（FloraUseMemory=0）—— 不再依赖「引擎画过 / 我们记下来」）(SFSE build {})",
		SFSE::GetSFSEVersion());

	if (auto* messaging = SFSE::GetMessagingInterface()) {
		messaging->RegisterListener(OnMessage);
	} else {
		REX::WARN("messaging interface unavailable");
	}

	TryInstall();
	return true;
}
